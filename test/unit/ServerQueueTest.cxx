/*
 * ServerQueueTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "FaultInjection.hxx"
#include "JsonLineClient.hxx"
#include "ServerHarness.hxx"
#include "ServerTestUtil.hxx"
#include "TestSupport.hxx"
#include "ToolStubs.hxx"

extern "C" {
#include "mcpjson.h"
#include "mcp.h"
#include "mcpconf.h"
}

/* CppUTest after the standard headers: its new macros break them otherwise */
#include "CppUTest/TestHarness.h"

/*
 * The bounded queue between the listener thread and Alpine's main thread:
 * its capacity of eight, answers going to the client that asked, calls of
 * a client that has gone being dropped before they start, and a tool that
 * is slow or whose client leaves while it runs.
 */

TEST_GROUP(ServerQueue)
{
    ServerHarness *h;

    void setup()
    {
        h = new ServerHarness();
        h->usePolicy();
        LONGS_EQUAL(0, h->start());
        stubBehavior.reset();
        Fault::reset();
    }

    void teardown()
    {
        delete h;
        stubBehavior.reset();
    }

    std::string listCall(int id)
    {
        return JsonLineClient::toolCall(std::to_string(id), "mail_list_folders", "{}");
    }
};

TEST(ServerQueue, TheNinthQueuedCallIsRefusedAtOnce)
{
    JsonLineClient c;

    CHECK(c.connectTo(h->socketPath()));
    std::string all;

    for (int i = 1; i <= 9; i++) {
        all += listCall(i) + "\n";
    }

    CHECK(c.sendRaw(all));

    /* no drain yet: the only thing that can come back is the refusal */
    Json refused(c.readJson(2000));

    CHECK(refused.ok());
    LONGS_EQUAL(-32005, refused.errorCode());
    LONGS_EQUAL(9, (int) refused.at("id")->valuedouble);
    LONGS_EQUAL(0, stubBehavior.runs);

    h->drainAll();
    for (int i = 1; i <= 8; i++) {
        Json r(c.readJson(2000));

        CHECK(r.ok());
        LONGS_EQUAL(i, (int) r.at("id")->valuedouble);
        CHECK(r.at("error") == NULL);
    }

    LONGS_EQUAL(8, stubBehavior.runs);
}

TEST(ServerQueue, TheQueueIsEmptyAgainAfterADrain)
{
    JsonLineClient c;

    CHECK(c.connectTo(h->socketPath()));
    for (int round = 0; round < 3; round++) {
        std::string all;

        for (int i = 1; i <= 8; i++) {
            all += listCall(round * 100 + i) + "\n";
        }

        CHECK(c.sendRaw(all));
        usleep(100 * 1000);
        h->drainAll();
        for (int i = 1; i <= 8; i++) {
            Json r(c.readJson(2000));

            CHECK(r.ok());
            CHECK(r.at("error") == NULL);
        }
    }

    LONGS_EQUAL(24, stubBehavior.runs);
}

TEST(ServerQueue, AClientThatLeavesTakesItsQueuedCallsWithIt)
{
    {
        JsonLineClient gone;

        CHECK(gone.connectTo(h->socketPath()));
        for (int i = 1; i <= 3; i++) {
            gone.sendLine(listCall(i));
        }

        usleep(150 * 1000);        /* queued by the listener */
    }                               /* closed here, nothing drained */

    usleep(150 * 1000);
    JsonLineClient stays;

    CHECK(stays.connectTo(h->socketPath()));
    stays.sendLine(listCall(10));
    h->drainAll();
    Json r(stays.readJson(2000));

    CHECK(r.ok());
    LONGS_EQUAL(10, (int) r.at("id")->valuedouble);
    LONGS_EQUAL(1, stubBehavior.runs);          /* only the live client's */
}

TEST(ServerQueue, OnlyTheLeavingClientsCallsAreCancelled)
{
    JsonLineClient a;
    JsonLineClient b;

    CHECK(a.connectTo(h->socketPath()));
    CHECK(b.connectTo(h->socketPath()));
    a.sendLine(listCall(1));
    b.sendLine(listCall(2));
    a.sendLine(listCall(3));
    b.sendLine(listCall(4));
    usleep(150 * 1000);
    a.closeNow();
    usleep(150 * 1000);
    h->drainAll();

    Json r1(b.readJson(2000));
    Json r2(b.readJson(2000));

    LONGS_EQUAL(2, (int) r1.at("id")->valuedouble);
    LONGS_EQUAL(4, (int) r2.at("id")->valuedouble);
    LONGS_EQUAL(2, stubBehavior.runs);
}

TEST(ServerQueue, AToolThatTakesAWhileDelaysOnlyItsOwnReply)
{
    ScopedThreadSafeTracking threadSafe;

    stubBehavior.delayMs = 250;
    JsonLineClient c;

    CHECK(c.connectTo(h->socketPath()));
    c.sendLine(listCall(1));
    c.sendLine(JsonLineClient::request("2", "ping"));

    /* the listener answers a ping while the main thread is busy */
    std::thread busy([this] { h->drainAll(3000, 400); });
    Json ping(c.readJson(2000));

    CHECK(ping.ok());
    LONGS_EQUAL(2, (int) ping.at("id")->valuedouble);
    Json r(c.readJson(3000));

    CHECK(r.ok());
    LONGS_EQUAL(1, (int) r.at("id")->valuedouble);
    busy.join();
}

TEST(ServerQueue, ClientThatLeavesWhileItsToolRunsDoesNoHarm)
{
    ScopedThreadSafeTracking threadSafe;

    stubBehavior.delayMs = 400;
    JsonLineClient c;

    CHECK(c.connectTo(h->socketPath()));
    c.sendLine(listCall(1));

    std::thread leaver([&c] { usleep(150 * 1000); c.closeNow(); });

    CHECK(h->drain());              /* runs the tool for 400 ms */
    leaver.join();
    usleep(200 * 1000);

    stubBehavior.delayMs = 0;
    JsonLineClient d;

    CHECK(d.connectTo(h->socketPath()));
    d.sendLine(listCall(2));
    h->drainAll();
    Json r(d.readJson(2000));

    CHECK(r.ok());
    LONGS_EQUAL(2, (int) r.at("id")->valuedouble);
}

TEST(ServerQueue, EveryClientGetsItsOwnAnswersUnderConcurrentLoad)
{
    ScopedThreadSafeTracking threadSafe;

    const int clients = 6;
    const int rounds = 25;
    std::atomic<int> failures(0);
    std::atomic<int> finished(0);
    std::vector<std::thread> threads;

    for (int k = 0; k < clients; k++) {
        threads.push_back(std::thread([this, k, rounds, &failures, &finished] {
            JsonLineClient c;

            if (!c.connectTo(h->socketPath())) {
                failures++;
                finished++;
                return;
            }

            for (int i = 0; i < rounds; i++) {
                int id = k * 1000 + i;
                std::string line = (i % 2) ? JsonLineClient::request(std::to_string(id), "ping")
                                           : listCall(id);

                c.sendLine(line);
                cJSON *r = c.readJson(15000);

                if (r == NULL) {
                    failures++;
                    break;
                }

                cJSON *rid = cJSON_GetObjectItemCaseSensitive(r, "id");

                if (!cJSON_IsNumber(rid) || (int) rid->valuedouble != id
                    || cJSON_GetObjectItemCaseSensitive(r, "error") != NULL) {
                    failures++;
                }

                cJSON_Delete(r);
            }

            finished++;
        }));
    }

    while (finished.load() < clients) {
        h->drain(20);
    }

    for (size_t i = 0; i < threads.size(); i++) {
        threads[i].join();
    }

    LONGS_EQUAL(0, failures.load());
    LONGS_EQUAL(clients * ((rounds + 1) / 2), stubBehavior.runs);
}

TEST(ServerQueue, BurstFromManyClientsAnswersEveryCallExactlyOnce)
{
    ScopedThreadSafeTracking threadSafe;

    const int clients = 8;
    const int perClient = 8;
    std::atomic<int> answered(0);
    std::atomic<int> refused(0);
    std::atomic<int> bad(0);
    std::vector<std::thread> threads;
    std::atomic<int> finished(0);

    /* hold the main thread back so the queue really fills */
    for (int k = 0; k < clients; k++) {
        threads.push_back(std::thread([this, k, perClient, &answered, &refused, &bad, &finished] {
            JsonLineClient c;

            if (!c.connectTo(h->socketPath())) {
                bad++;
                finished++;
                return;
            }

            std::string all;

            for (int i = 0; i < perClient; i++) {
                all += listCall(k * 100 + i) + "\n";
            }

            c.sendRaw(all);
            for (int i = 0; i < perClient; i++) {
                cJSON *r = c.readJson(15000);

                if (r == NULL) {
                    bad++;
                    break;
                }

                cJSON *e = cJSON_GetObjectItemCaseSensitive(r, "error");

                if (e == NULL) {
                    answered++;
                } else if (cJSON_GetObjectItemCaseSensitive(e, "code")->valuedouble == -32005) {
                    refused++;
                } else {
                    bad++;
                }

                cJSON_Delete(r);
            }

            finished++;
        }));
    }

    usleep(300 * 1000);             /* everything that fits is queued */
    while (finished.load() < clients) {
        h->drain(20);
    }

    for (size_t i = 0; i < threads.size(); i++) {
        threads[i].join();
    }

    LONGS_EQUAL(0, bad.load());
    LONGS_EQUAL(clients * perClient, answered.load() + refused.load());
    LONGS_EQUAL(answered.load(), stubBehavior.runs);
    CHECK(refused.load() > 0);                  /* the cap of 8 did bite */
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
