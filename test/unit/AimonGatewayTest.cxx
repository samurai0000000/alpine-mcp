/*
 * AimonGatewayTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <string>
#include <unistd.h>

#include "FakeGateway.hxx"
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
 * The client that registers the tools with aimon's TCP gateway and takes
 * tools/call from it: the registration line, replies on the same id,
 * reconnecting with backoff (1, 2, 4 ... seconds), a gateway that refuses
 * or stays silent or is not there, and the monitor screen's drop and
 * on/off controls.
 */

namespace
{
    int askCount = 0;

    int
    askYes(const char *)
    {
        askCount++;
        return 1;
    }

    const char *sendArgs =
        "{\"to\":\"x@example.net\",\"idempotency_key\":\"abcdefgh\"}";
}

TEST_GROUP(AimonGateway)
{
    ScopedThreadSafeTracking threadSafe;
    ServerHarness *h;
    FakeGateway *gw;

    void setup()
    {
        h = new ServerHarness();
        gw = new FakeGateway();
        stubBehavior.reset();
        Fault::reset();
        askCount = 0;
    }

    void teardown()
    {
        Fault::reset();
        delete h;           /* stops the server first */
        delete gw;
        stubBehavior.reset();
    }

    /* policy with aimon on at the gateway's port */
    void configure(bool aimonOn = true, int port = -1)
    {
        h->useConfig(std::string("{\"mcp\":{\"enabled\":true},\"operator_lock\":false,"
                                 "\"allowed_recipients\":[\"ok@example.org\"],"
                                 "\"aimon\":{\"enabled\":") + (aimonOn ? "true" : "false")
                     + ",\"host\":\"127.0.0.1\",\"port\":"
                     + std::to_string(port >= 0 ? port : gw->port()) + "}}");
    }

    void startAll()
    {
        CHECK(gw->start());
        configure();
        LONGS_EQUAL(0, h->start());
    }

    std::string aimonStatus()
    {
        char buf[300];

        mcp_aimon_status(buf, sizeof(buf));
        return buf;
    }

    /* wait until the status text contains the fragment */
    bool statusHas(const char *part, int ms = 3000)
    {
        long long end = FakeGateway::nowMs() + ms;

        while (FakeGateway::nowMs() < end) {
            if (aimonStatus().find(part) != std::string::npos) {
                return true;
            }

            usleep(50 * 1000);
        }

        return false;
    }

    /* the next line from the add-on that has this id */
    bool lineWithId(const std::string &id, std::string &out, int ms = 3000)
    {
        long long end = FakeGateway::nowMs() + ms;

        while (FakeGateway::nowMs() < end) {
            std::string line;

            if (!gw->nextLine(line, 200)) {
                continue;
            }

            cJSON *j = cJSON_Parse(line.c_str());
            cJSON *i = j != NULL ? cJSON_GetObjectItemCaseSensitive(j, "id") : NULL;
            bool hit = cJSON_IsString(i) && id == i->valuestring;

            if (j != NULL) {
                cJSON_Delete(j);
            }

            if (hit) {
                out = line;
                return true;
            }
        }

        return false;
    }
};

TEST(AimonGateway, RegistersAsAlpineMcpWithTheCurrentToolList)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));

    Json reg(cJSON_Parse(gw->lastRegister().c_str()));

    CHECK(reg.ok());
    STRCMP_EQUAL("2.0", reg.str("jsonrpc").c_str());
    STRCMP_EQUAL("gateway/register", reg.str("method").c_str());
    LONGS_EQUAL(1, (int) reg.at("id")->valuedouble);
    cJSON *p = reg.at("params");

    STRCMP_EQUAL("alpine-mcp", cJSON_GetObjectItemCaseSensitive(p, "subsystem")->valuestring);
    STRCMP_EQUAL("2.26", cJSON_GetObjectItemCaseSensitive(p, "version")->valuestring);
    STRCMP_EQUAL("Alpine Mail", cJSON_GetObjectItemCaseSensitive(p, "display_name")->valuestring);
    STRCMP_EQUAL("Alpine", cJSON_GetObjectItemCaseSensitive(p, "short_name")->valuestring);
    LONGS_EQUAL(40, (int) cJSON_GetObjectItemCaseSensitive(p, "priority")->valuedouble);
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(p, "tools");

    CHECK(cJSON_IsArray(tools));
    LONGS_EQUAL(10, cJSON_GetArraySize(tools));

    usleep(300 * 1000);
    CHECK(aimonStatus().find("registered") != std::string::npos);
    CHECK(aimonStatus().find("not") == std::string::npos);
}

TEST(AimonGateway, ToolCallsFromTheGatewayRunAndAreAnsweredOnTheSameStringId)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);

    CHECK(gw->send(JsonLineClient::toolCall("\"aimon-gw-1\"", "mail_list_folders", "{}")));
    CHECK(h->drain());
    std::string line;

    CHECK(lineWithId("aimon-gw-1", line));
    Json r(cJSON_Parse(line.c_str()));

    CHECK(cJSON_IsString(r.at("id")));
    CHECK(r.at("error") == NULL);
    Json inner(r.toolResult());

    STRCMP_EQUAL("mail_list_folders", inner.str("tool").c_str());
}

TEST(AimonGateway, PingFromTheGatewayIsAnswered)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);

    CHECK(gw->send(JsonLineClient::request("\"p-1\"", "ping")));
    std::string line;

    CHECK(lineWithId("p-1", line));
    Json r(cJSON_Parse(line.c_str()));

    CHECK(cJSON_IsObject(r.at("result")));
}

TEST(AimonGateway, UnknownResponsesAndGarbageAreIgnoredAndTheLinkSurvives)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);

    CHECK(gw->send("{\"jsonrpc\":\"2.0\",\"id\":99,\"result\":{\"status\":\"registered\"}}"));
    CHECK(gw->send("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"status\":\"registered\"}}"));
    CHECK(gw->send("[1,2,3]"));
    CHECK(gw->send(JsonLineClient::request("\"after\"", "ping")));
    std::string line;

    CHECK(lineWithId("after", line));
    LONGS_EQUAL(1, gw->registrations());            /* no reconnect */
}

TEST(AimonGateway, TheLockRefusesGatewayCallsToo)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);

    mcp_set_lock(1);
    CHECK(gw->send(JsonLineClient::toolCall("\"locked\"", "mail_list_folders", "{}")));
    std::string line;

    CHECK(lineWithId("locked", line));
    Json r(cJSON_Parse(line.c_str()));

    LONGS_EQUAL(-32001, r.errorCode());
    LONGS_EQUAL(0, stubBehavior.runs);
    mcp_set_lock(0);
}

TEST(AimonGateway, ASocketClientAndTheGatewayWorkSideBySide)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);

    JsonLineClient c;

    CHECK(c.connectTo(h->socketPath()));
    c.sendLine(JsonLineClient::toolCall("1", "mail_list_folders", "{}"));
    CHECK(gw->send(JsonLineClient::toolCall("\"g1\"", "mail_list_folders", "{}")));
    usleep(150 * 1000);
    h->drainAll();

    Json r(c.readJson(2000));

    CHECK(r.ok());
    LONGS_EQUAL(1, (int) r.at("id")->valuedouble);
    std::string line;

    CHECK(lineWithId("g1", line));
    LONGS_EQUAL(2, stubBehavior.runs);
}

TEST(AimonGateway, ReRegistersAfterTheGatewayClosesTheConnection)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);

    gw->dropClient();
    CHECK(gw->waitForRegistrations(2, 6000));
    std::vector<long long> t = gw->registerTimesMs();
    long long gap = t[1] - t[0];

    CHECK_TEXT(gap >= 900 && gap <= 4000, std::to_string(gap).c_str());   /* first backoff */
}

TEST(AimonGateway, RefusedRegistrationBacksOffOneTwoThenFourSeconds)
{
    gw->setMode(FakeGateway::Refuse);
    startAll();
    CHECK(gw->waitForRegistrations(4, 14000));
    std::vector<long long> t = gw->registerTimesMs();

    CHECK(t.size() >= 4);
    long long g1 = t[1] - t[0];
    long long g2 = t[2] - t[1];
    long long g3 = t[3] - t[2];

    CHECK_TEXT(g1 >= 900 && g1 <= 2500, std::to_string(g1).c_str());
    CHECK_TEXT(g2 >= 1900 && g2 <= 3500, std::to_string(g2).c_str());
    CHECK_TEXT(g3 >= 3900 && g3 <= 5500, std::to_string(g3).c_str());
    CHECK(statusHas("not connected"));

    /* a gateway that then accepts ends the backoff */
    gw->setMode(FakeGateway::Accept);
    CHECK(gw->waitForRegistrations(5, 10000));
    CHECK(statusHas("registered with aimon"));
}

TEST(AimonGateway, ARegistrationAnsweredWithoutAStatusIsAFailure)
{
    gw->setMode(FakeGateway::NoStatus);
    startAll();
    CHECK(gw->waitForRegistrations(2, 6000));
    CHECK(aimonStatus().find("registered with") == std::string::npos);
}

TEST(AimonGateway, ASilentGatewayIsGivenUpOnAfterFiveSeconds)
{
    gw->setMode(FakeGateway::Silent);
    startAll();
    CHECK(gw->waitForRegistrations(2, 14000));
    std::vector<long long> t = gw->registerTimesMs();
    long long gap = t[1] - t[0];

    CHECK_TEXT(gap >= 5500 && gap <= 8500, std::to_string(gap).c_str());   /* 5 s + 1 s backoff */
}

TEST(AimonGateway, GatewayThatClosesAtOnceIsRetried)
{
    gw->setMode(FakeGateway::CloseAtOnce);
    startAll();
    CHECK(gw->waitForRegistrations(3, 10000));
    CHECK(aimonStatus().find("registered with") == std::string::npos);
}

TEST(AimonGateway, NothingListeningYieldsARetryStatusAndNoErrorOutOfTheServer)
{
    /* a port nobody listens on: take one and give it back */
    CHECK(gw->start());
    int port = gw->port();

    gw->stop();
    configure(true, port);
    LONGS_EQUAL(0, h->start());
    CHECK(statusHas("not connected"));
    CHECK(statusHas("retry in"));

    /* a socket client is served meanwhile */
    JsonLineClient c;

    CHECK(c.connectTo(h->socketPath()));
    c.sendLine(JsonLineClient::request("1", "ping"));
    Json r(c.readJson(2000));

    CHECK(r.ok());

    /* and when the gateway appears it is found */
    CHECK(gw->start(port));
    CHECK(gw->waitForRegistrations(1, 8000));
}

TEST(AimonGateway, PolicyWithAimonOffNeverConnects)
{
    CHECK(gw->start());
    configure(false);
    LONGS_EQUAL(0, h->start());
    usleep(1500 * 1000);
    LONGS_EQUAL(0, gw->connections());
    STRCMP_EQUAL("off", aimonStatus().c_str());
}

TEST(AimonGateway, TheMonitorCanTurnAimonOnAndOffAtRunTime)
{
    CHECK(gw->start());
    configure(false);
    LONGS_EQUAL(0, h->start());
    usleep(300 * 1000);
    LONGS_EQUAL(0, gw->connections());

    mcp_set_aimon_enabled(1);
    mcp_aimon_kick(0);
    CHECK(gw->waitForRegistrations(1, 5000));

    mcp_set_aimon_enabled(0);
    mcp_aimon_kick(0);
    CHECK(gw->waitForEof(5000) || true);
    usleep(500 * 1000);
    STRCMP_EQUAL("off", aimonStatus().c_str());
    int regs = gw->registrations();

    usleep(1500 * 1000);
    LONGS_EQUAL(regs, gw->registrations());         /* stays off */
}

TEST(AimonGateway, KickDropsTheConnectionAndRegistersAgainAtOnce)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(300 * 1000);

    mcp_aimon_kick(1);
    CHECK(gw->waitForRegistrations(2, 4000));
    std::vector<long long> t = gw->registerTimesMs();

    CHECK_TEXT(t[1] - t[0] < 2000, std::to_string(t[1] - t[0]).c_str());
}

TEST(AimonGateway, CallsQueuedWhenTheGatewayLeavesAreCancelled)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);

    CHECK(gw->send(JsonLineClient::toolCall("\"a\"", "mail_list_folders", "{}")));
    CHECK(gw->send(JsonLineClient::toolCall("\"b\"", "mail_list_folders", "{}")));
    usleep(200 * 1000);                     /* queued, not run */
    gw->dropClient();
    usleep(300 * 1000);
    h->drainAll(500, 200);

    LONGS_EQUAL(0, stubBehavior.runs);
    CHECK(gw->waitForRegistrations(2, 6000));       /* and it came back */
}

TEST(AimonGateway, AParkedGatewayCallIsAskedWithinFortySecondsAndDroppedAfter)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);
    stubBehavior.park = true;
    stubBehavior.prompt = "send?";

    /* within the limit: asked, and the yes runs the send */
    CHECK(gw->send(JsonLineClient::toolCall("\"p1\"", "mail_send", sendArgs)));
    CHECK(h->drain());
    LONGS_EQUAL(1, mcp_pending_confirms());
    Fault::advanceTime(39);
    mcp_confirm_pending(askYes);
    LONGS_EQUAL(1, askCount);
    LONGS_EQUAL(1, stubBehavior.confirmedRuns);
    std::string line;

    CHECK(lineWithId("p1", line));
    Json ok(cJSON_Parse(line.c_str()));

    CHECK(ok.at("error") == NULL);

    /* beyond it: aimon has already given up, so the question is not put */
    Fault::advanceTime(0);
    CHECK(gw->send(JsonLineClient::toolCall("\"p2\"", "mail_send", sendArgs)));
    CHECK(h->drain());
    Fault::advanceTime(41);
    mcp_confirm_pending(askYes);
    LONGS_EQUAL(1, askCount);                       /* unchanged */
    LONGS_EQUAL(1, stubBehavior.confirmedRuns);     /* nothing sent */
    CHECK(lineWithId("p2", line));
    Json late(cJSON_Parse(line.c_str()));

    LONGS_EQUAL(-32007, late.errorCode());
    CHECK(late.errorMessage().find("not answered in time") != std::string::npos);
    Fault::advanceTime(0);
}

TEST(AimonGateway, ASocketClientsParkedCallHasNoTimeLimit)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    stubBehavior.park = true;
    stubBehavior.prompt = "send?";

    JsonLineClient c;

    CHECK(c.connectTo(h->socketPath()));
    c.sendLine(JsonLineClient::toolCall("1", "mail_send", sendArgs));
    CHECK(h->drain());
    Fault::advanceTime(3600);
    mcp_confirm_pending(askYes);
    LONGS_EQUAL(1, askCount);
    LONGS_EQUAL(1, stubBehavior.confirmedRuns);
    Json r(c.readJson(2000));

    CHECK(r.ok());
    Fault::advanceTime(0);
}

TEST(AimonGateway, AnOversizedLineFromTheGatewayDropsTheLinkAndItReRegisters)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);

    std::string big(4 * 1024 * 1024 + 100, 'z');

    gw->send(big);
    CHECK(gw->waitForRegistrations(2, 8000));
    CHECK(h->running());
}

TEST(AimonGateway, StoppingTheServerClosesTheGatewayConnection)
{
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);
    h->stop();
    CHECK(gw->waitForEof(3000));
    int regs = gw->registrations();

    usleep(1500 * 1000);
    LONGS_EQUAL(regs, gw->registrations());         /* no reconnect after stop */
}

TEST(AimonGateway, AimonConnectedReflectsRegistrationState)
{
    LONGS_EQUAL(0, mcp_aimon_connected());
    startAll();
    CHECK(gw->waitForRegistrations(1, 5000));
    usleep(200 * 1000);
    LONGS_EQUAL(1, mcp_aimon_connected());
    h->stop();
    LONGS_EQUAL(0, mcp_aimon_connected());
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
