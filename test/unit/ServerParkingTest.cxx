/*
 * ServerParkingTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <string>
#include <thread>
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
 * A call that needs the human's answer (an off-list send) is parked by the
 * drain and put to the human from the command loop.  These tests pin down
 * who is asked, in what order, what a yes, a no, a vanished client, the
 * lock and an old question each do, and that nothing is sent without a
 * yes.
 */

namespace
{
    int askCount = 0;
    std::string askedPrompts;
    int askAnswer = 0;
    void (*askHook)() = NULL;

    int
    askQuestion(const char *prompt)
    {
        askCount++;
        askedPrompts += std::string(prompt) + "|";

        int answer = askAnswer;         /* the hook changes it for the next */

        if (askHook != NULL) {
            askHook();
        }

        return answer;
    }

    void
    resetAsk()
    {
        askCount = 0;
        std::string().swap(askedPrompts);
        askAnswer = 0;
        askHook = NULL;
    }

    const char *sendArgs =
        "{\"to\":\"x@example.net\",\"idempotency_key\":\"abcdefgh\"}";
}

TEST_GROUP(ServerParking)
{
    ServerHarness *h;
    JsonLineClient *c;

    void setup()
    {
        h = new ServerHarness();
        h->usePolicy();
        LONGS_EQUAL(0, h->start());
        c = new JsonLineClient();
        CHECK(c->connectTo(h->socketPath()));
        stubBehavior.reset();
        stubBehavior.park = true;
        stubBehavior.prompt = "may the agent send?";
        resetAsk();
        Fault::reset();
    }

    void teardown()
    {
        delete c;
        delete h;
        stubBehavior.reset();
        resetAsk();
        Fault::reset();
    }

    /* send a parked call and let the drain park it */
    void park(const std::string &id)
    {
        c->sendLine(JsonLineClient::toolCall(id, "mail_send", sendArgs));
        CHECK(h->drain());
    }

    /* nothing may come back before the human answers */
    void expectSilence(int ms = 250)
    {
        std::string line;

        CHECK(!c->readLine(line, ms));
    }
};

TEST(ServerParking, ACallThatAsksIsHeldAndNotRun)
{
    park("1");
    expectSilence();
    LONGS_EQUAL(1, stubBehavior.runs);              /* the first pass only */
    LONGS_EQUAL(0, stubBehavior.confirmedRuns);
    LONGS_EQUAL(1, mcp_pending_confirms());
}

TEST(ServerParking, TakeNewParkReportsEachParkedCallOnce)
{
    LONGS_EQUAL(0, mcp_take_new_park());
    park("1");
    LONGS_EQUAL(1, mcp_take_new_park());
    LONGS_EQUAL(0, mcp_take_new_park());
    park("2");
    park("3");
    LONGS_EQUAL(1, mcp_take_new_park());            /* a flag, not a count */
    LONGS_EQUAL(0, mcp_take_new_park());
    LONGS_EQUAL(3, mcp_pending_confirms());
}

TEST(ServerParking, YesRunsTheConfirmedPartAndAnswersTheClient)
{
    park("1");
    askAnswer = 1;
    mcp_confirm_pending(askQuestion);

    LONGS_EQUAL(1, askCount);
    STRCMP_EQUAL("may the agent send?|", askedPrompts.c_str());
    LONGS_EQUAL(1, stubBehavior.confirmedRuns);
    LONGS_EQUAL(0, mcp_pending_confirms());

    Json r(c->readJson(2000));

    CHECK(r.ok());
    CHECK(r.at("error") == NULL);
    LONGS_EQUAL(1, (int) r.at("id")->valuedouble);
    STRCMP_EQUAL(sendArgs, stubBehavior.lastArgs);   /* same arguments */
}

TEST(ServerParking, NoAnswersMinus32007AndNothingRuns)
{
    park("1");
    askAnswer = 0;
    mcp_confirm_pending(askQuestion);

    LONGS_EQUAL(0, stubBehavior.confirmedRuns);
    Json r(c->readJson(2000));

    LONGS_EQUAL(-32007, r.errorCode());
    CHECK(r.errorMessage().find("no") != std::string::npos);
    LONGS_EQUAL(0, mcp_pending_confirms());
}

TEST(ServerParking, ParkedCallsAreAskedInTheOrderTheyArrived)
{
    stubBehavior.prompt = "first";
    park("1");
    stubBehavior.prompt = "second";
    park("2");
    stubBehavior.prompt = "third";
    park("3");

    askAnswer = 1;
    mcp_confirm_pending(askQuestion);
    STRCMP_EQUAL("first|second|third|", askedPrompts.c_str());
    LONGS_EQUAL(3, stubBehavior.confirmedRuns);
    for (int i = 1; i <= 3; i++) {
        Json r(c->readJson(2000));

        LONGS_EQUAL(i, (int) r.at("id")->valuedouble);
    }
}

TEST(ServerParking, EachCallGetsItsOwnAnswer)
{
    park("1");
    park("2");
    askAnswer = 1;
    askHook = [] { askAnswer = 0; };        /* yes for the first, no for the rest */
    mcp_confirm_pending(askQuestion);

    Json r1(c->readJson(2000));
    Json r2(c->readJson(2000));

    LONGS_EQUAL(1, (int) r1.at("id")->valuedouble);
    CHECK(r1.at("error") == NULL);
    LONGS_EQUAL(2, (int) r2.at("id")->valuedouble);
    LONGS_EQUAL(-32007, r2.errorCode());
    LONGS_EQUAL(1, stubBehavior.confirmedRuns);
}

TEST(ServerParking, ACallWhoseClientLeftIsNeverAsked)
{
    park("1");
    c->closeNow();
    usleep(200 * 1000);                     /* the listener notices */
    askAnswer = 1;
    mcp_confirm_pending(askQuestion);

    LONGS_EQUAL(0, askCount);
    LONGS_EQUAL(0, stubBehavior.confirmedRuns);
    LONGS_EQUAL(0, mcp_pending_confirms());
}

TEST(ServerParking, ClientThatLeavesWhileTheQuestionIsUpGetsNothingSent)
{
    ScopedThreadSafeTracking threadSafe;

    park("1");
    askAnswer = 1;
    askHook = [] { usleep(0); };
    JsonLineClient *client = c;

    std::thread leaver([client] { usleep(100 * 1000); client->closeNow(); });

    /* the question stays up for 400 ms; the client goes in the middle */
    askHook = [] { usleep(400 * 1000); };
    mcp_confirm_pending(askQuestion);
    leaver.join();

    LONGS_EQUAL(1, askCount);
    LONGS_EQUAL(0, stubBehavior.confirmedRuns);     /* a late yes sends nothing */
    LONGS_EQUAL(0, mcp_pending_confirms());

    JsonLineClient other;

    CHECK(other.connectTo(h->socketPath()));
    other.sendLine(JsonLineClient::request("9", "ping"));
    Json ok(other.readJson(2000));

    CHECK(ok.ok());
}

TEST(ServerParking, LockTurnedOnWhileTheQuestionIsUpStopsTheSend)
{
    park("1");
    askAnswer = 1;
    askHook = [] { mcp_set_lock(1); };
    mcp_confirm_pending(askQuestion);

    LONGS_EQUAL(0, stubBehavior.confirmedRuns);
    Json r(c->readJson(2000));

    LONGS_EQUAL(-32001, r.errorCode());
    mcp_set_lock(0);
}

TEST(ServerParking, ConfirmPendingIsNotReentrant)
{
    park("1");
    park("2");
    askAnswer = 1;
    askHook = [] { mcp_confirm_pending(askQuestion); };     /* a nested call */
    mcp_confirm_pending(askQuestion);

    LONGS_EQUAL(2, askCount);                       /* once per call, no more */
    LONGS_EQUAL(2, stubBehavior.confirmedRuns);
}

TEST(ServerParking, NothingToAskAndNoQuestionFunctionAreHarmless)
{
    mcp_confirm_pending(askQuestion);
    LONGS_EQUAL(0, askCount);
    park("1");
    mcp_confirm_pending(NULL);
    LONGS_EQUAL(1, mcp_pending_confirms());         /* still waiting */
    askAnswer = 0;
    mcp_confirm_pending(askQuestion);
    LONGS_EQUAL(0, mcp_pending_confirms());
}

TEST(ServerParking, ToolWithoutAConfirmedPartIsNotParked)
{
    /* only mail_send has a confirmed part; a request to wait from any other
       tool must not strand the call */
    c->sendLine(JsonLineClient::toolCall("1", "mail_list_folders", "{}"));
    CHECK(h->drain());
    Json r(c->readJson(2000));

    CHECK(r.ok());
    LONGS_EQUAL(1, (int) r.at("id")->valuedouble);
    LONGS_EQUAL(0, mcp_pending_confirms());
}

TEST(ServerParking, HalfClosedClientStillGetsTheAnswerAfterTheQuestion)
{
    c->sendLine(JsonLineClient::toolCall("1", "mail_send", sendArgs));
    c->shutdownWrite();
    CHECK(h->drain());
    usleep(200 * 1000);                     /* the server sees the end of input */
    askAnswer = 1;
    mcp_confirm_pending(askQuestion);

    Json r(c->readJson(2000));

    CHECK(r.ok());
    CHECK(r.at("error") == NULL);
    std::string more;

    CHECK(!c->readLine(more, 2000));
    CHECK(c->atEof());
}

TEST(ServerParking, LockedBeforeTheDrainNeverParks)
{
    mcp_set_lock(1);
    c->sendLine(JsonLineClient::toolCall("1", "mail_send", sendArgs));
    Json r(c->readJson(2000));

    LONGS_EQUAL(-32001, r.errorCode());
    LONGS_EQUAL(0, stubBehavior.runs);
    LONGS_EQUAL(0, mcp_pending_confirms());
    mcp_set_lock(0);
}

TEST(ServerParking, ARunThatFailsAfterYesReportsItsErrorCode)
{
    park("1");
    stubBehavior.park = false;                  /* the confirmed pass fails */
    stubBehavior.code = -32008;
    stubBehavior.message = "smtp not logged in";
    askAnswer = 1;
    mcp_confirm_pending(askQuestion);

    Json r(c->readJson(2000));

    LONGS_EQUAL(-32008, r.errorCode());
    STRCMP_EQUAL("smtp not logged in", r.errorMessage().c_str());
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
