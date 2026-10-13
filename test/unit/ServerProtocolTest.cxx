/*
 * ServerProtocolTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cstring>
#include <string>
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
 * The wire protocol of the socket, exercised with a real client against the
 * real listener thread: framing, JSON-RPC error codes, id types, the
 * operator lock, and how a tool's outcome becomes a reply.
 */

TEST_GROUP(ServerProtocol)
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
        Fault::reset();
    }

    void teardown()
    {
        delete c;
        delete h;
        stubBehavior.reset();
    }

    /* a reply that does not need the main thread */
    cJSON *reply(int ms = 2000)
    {
        return c->readJson(ms);
    }

    /* a tool call: send, run it on this thread, read the reply */
    cJSON *callTool(const std::string &tool, const std::string &args = "{}",
                    const std::string &id = "1")
    {
        c->sendLine(JsonLineClient::toolCall(id, tool, args));
        h->drain();
        return reply();
    }

    cJSON *ask(const std::string &line, int ms = 2000)
    {
        c->sendLine(line);
        return reply(ms);
    }
};

TEST(ServerProtocol, InitializeAnswersWithTheProtocolAndServerIdentity)
{
    Json r(ask(JsonLineClient::request("1", "initialize",
        "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"t\",\"version\":\"0\"}}")));

    CHECK(r.ok());
    STRCMP_EQUAL("2.0", r.str("jsonrpc").c_str());
    cJSON *res = r.at("result");

    CHECK(res != NULL);
    STRCMP_EQUAL("2025-11-25",
                 cJSON_GetObjectItemCaseSensitive(res, "protocolVersion")->valuestring);
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(
                       cJSON_GetObjectItemCaseSensitive(res, "capabilities"), "tools");

    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(tools, "listChanged")));
    cJSON *info = cJSON_GetObjectItemCaseSensitive(res, "serverInfo");

    STRCMP_EQUAL("alpine-mcp", cJSON_GetObjectItemCaseSensitive(info, "name")->valuestring);
    STRCMP_EQUAL("2.26", cJSON_GetObjectItemCaseSensitive(info, "version")->valuestring);
}

TEST(ServerProtocol, NotificationsGetNoReply)
{
    c->sendLine("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    c->sendLine("{\"jsonrpc\":\"2.0\",\"method\":\"anything/at/all\",\"params\":{}}");
    Json r(ask(JsonLineClient::request("7", "ping")));

    CHECK(r.ok());                       /* the first reply is the ping's */
    LONGS_EQUAL(7, (int) r.at("id")->valuedouble);
}

TEST(ServerProtocol, PingReturnsAnEmptyObject)
{
    Json r(ask(JsonLineClient::request("1", "ping")));

    CHECK(cJSON_IsObject(r.at("result")));
    LONGS_EQUAL(0, cJSON_GetArraySize(r.at("result")));
}

TEST(ServerProtocol, IdsKeepTheirJsonType)
{
    Json num(ask(JsonLineClient::request("42", "ping")));

    CHECK(cJSON_IsNumber(num.at("id")));
    LONGS_EQUAL(42, (int) num.at("id")->valuedouble);

    Json str(ask(JsonLineClient::request("\"aimon-gw-7\"", "ping")));

    CHECK(cJSON_IsString(str.at("id")));
    STRCMP_EQUAL("aimon-gw-7", str.str("id").c_str());

    Json big(ask(JsonLineClient::request("9007199254740991", "ping")));

    CHECK(cJSON_IsNumber(big.at("id")));
    Json zero(ask(JsonLineClient::request("0", "ping")));

    CHECK(cJSON_IsNumber(zero.at("id")));
    Json empty(ask(JsonLineClient::request("\"\"", "ping")));

    CHECK(cJSON_IsString(empty.at("id")));
}

TEST(ServerProtocol, ToolsListHoldsAllTenTools)
{
    Json r(ask(JsonLineClient::request("1", "tools/list")));
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(r.at("result"), "tools");

    CHECK(cJSON_IsArray(tools));
    LONGS_EQUAL(10, cJSON_GetArraySize(tools));
}

TEST(ServerProtocol, UnknownMethodIsMinus32601)
{
    Json r(ask(JsonLineClient::request("1", "resources/list")));

    LONGS_EQUAL(-32601, r.errorCode());
    LONGS_EQUAL(1, (int) r.at("id")->valuedouble);
}

TEST(ServerProtocol, MalformedLinesGetParseOrInvalidRequestErrors)
{
    Json notJson(ask("this is not json"));

    LONGS_EQUAL(-32700, notJson.errorCode());
    CHECK(cJSON_IsNull(notJson.at("id")));

    Json truncated(ask("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"));

    LONGS_EQUAL(-32700, truncated.errorCode());

    Json blanks(ask("   "));

    LONGS_EQUAL(-32700, blanks.errorCode());

    Json batch(ask("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]"));

    LONGS_EQUAL(-32600, batch.errorCode());

    Json scalar(ask("42"));

    LONGS_EQUAL(-32600, scalar.errorCode());

    Json str(ask("\"ping\""));

    LONGS_EQUAL(-32600, str.errorCode());

    Json nul(ask("null"));

    LONGS_EQUAL(-32600, nul.errorCode());

    Json trailing(ask("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"} extra"));

    LONGS_EQUAL(-32700, trailing.errorCode());

    /* and the connection is still good */
    Json ok(ask(JsonLineClient::request("2", "ping")));

    CHECK(ok.at("result") != NULL);
}

TEST(ServerProtocol, WrongJsonRpcVersionAndBadIdsAreInvalidRequests)
{
    Json v1(ask("{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"ping\"}"));

    LONGS_EQUAL(-32600, v1.errorCode());
    Json noVer(ask("{\"id\":1,\"method\":\"ping\"}"));

    LONGS_EQUAL(-32600, noVer.errorCode());
    Json numVer(ask("{\"jsonrpc\":2,\"id\":1,\"method\":\"ping\"}"));

    LONGS_EQUAL(-32600, numVer.errorCode());

    for (const char *badId : { "null", "true", "[1]", "{}", "[]" }) {
        Json r(ask(std::string("{\"jsonrpc\":\"2.0\",\"id\":") + badId
                   + ",\"method\":\"ping\"}"));

        LONGS_EQUAL_TEXT(-32600, r.errorCode(), badId);
    }

    Json noMethod(ask("{\"jsonrpc\":\"2.0\",\"id\":1}"));

    LONGS_EQUAL(-32600, noMethod.errorCode());
    Json numMethod(ask("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":5}"));

    LONGS_EQUAL(-32600, numMethod.errorCode());
}

TEST(ServerProtocol, ResponsesFromThePeerAreIgnored)
{
    c->sendLine("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");
    c->sendLine("{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":1,\"message\":\"x\"}}");
    Json r(ask(JsonLineClient::request("3", "ping")));

    LONGS_EQUAL(3, (int) r.at("id")->valuedouble);
}

TEST(ServerProtocol, FramingCrLfBlankLinesAndSplitWrites)
{
    c->sendRaw("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\r\n");
    Json crlf(reply());

    LONGS_EQUAL(1, (int) crlf.at("id")->valuedouble);

    c->sendRaw("\n\n\n");                       /* blank lines: no reply */
    std::string line = JsonLineClient::request("2", "ping") + "\n";

    for (size_t i = 0; i < line.size(); i += 5) {
        CHECK(c->sendRaw(line.substr(i, 5)));
        usleep(2000);
    }

    Json split(reply());

    LONGS_EQUAL(2, (int) split.at("id")->valuedouble);

    /* two requests in one write, answered in order */
    c->sendRaw(JsonLineClient::request("3", "ping") + "\n"
               + JsonLineClient::request("4", "ping") + "\n");
    Json a(reply());
    Json b(reply());

    LONGS_EQUAL(3, (int) a.at("id")->valuedouble);
    LONGS_EQUAL(4, (int) b.at("id")->valuedouble);
}

TEST(ServerProtocol, UnicodeEscapesAndOddBytesInsideStringsAreHandled)
{
    Json esc(ask("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"p\\u0069ng\"}"));

    CHECK(esc.at("result") != NULL);
    Json bytes(ask("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\",\"x\":\"\xff\xfe\xc3\x28\"}"));

    CHECK(bytes.at("result") != NULL);
}

TEST(ServerProtocol, NulByteInALineIsAParseError)
{
    std::string line = JsonLineClient::request("1", "ping");

    line.insert(10, 1, '\0');
    c->sendLine(line);
    Json r(reply());

    LONGS_EQUAL(-32700, r.errorCode());
    Json ok(ask(JsonLineClient::request("2", "ping")));

    CHECK(ok.at("result") != NULL);
}

TEST(ServerProtocol, DeeplyNestedJsonIsRefusedNotACrash)
{
    Json r(ask(std::string(100000, '[')));

    LONGS_EQUAL(-32700, r.errorCode());
    Json r2(ask(std::string("{\"a\":") + std::string(50000, '{')));

    LONGS_EQUAL(-32700, r2.errorCode());
    Json ok(ask(JsonLineClient::request("2", "ping")));

    CHECK(ok.at("result") != NULL);
}

TEST(ServerProtocol, LineSizeLimitIsFourMegabytes)
{
    const size_t limit = 4 * 1024 * 1024;
    const std::string head = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"pad\":\"";
    const std::string tail = "\"}";
    size_t room = limit - head.size() - tail.size();

    /* a line of exactly the limit is accepted */
    c->sendLine(head + std::string(room, 'p') + tail);
    Json ok(reply(5000));

    CHECK(ok.ok());
    CHECK(ok.at("result") != NULL);

    /* a line one byte over is refused and the connection is dropped */
    JsonLineClient over;

    CHECK(over.connectTo(h->socketPath()));
    over.sendRaw(head + std::string(room + 1, 'p') + tail + "\n");
    Json refused(over.readJson(5000));

    CHECK(refused.ok());
    LONGS_EQUAL(-32600, refused.errorCode());
    std::string more;

    over.readLine(more, 2000);
    CHECK(over.atEof());

    /* a line that never ends is cut off the same way */
    JsonLineClient endless;

    CHECK(endless.connectTo(h->socketPath()));
    std::string chunk(65536, 'x');

    for (size_t sent = 0; sent < limit + 200000; sent += chunk.size()) {
        if (!endless.sendRaw(chunk)) {
            break;
        }
    }

    Json cut(endless.readJson(5000));

    CHECK(cut.ok());
    LONGS_EQUAL(-32600, cut.errorCode());

    /* other clients are unaffected */
    Json fine(ask(JsonLineClient::request("2", "ping")));

    CHECK(fine.at("result") != NULL);
}

TEST(ServerProtocol, ToolsCallShapeErrors)
{
    Json noParams(ask(JsonLineClient::request("1", "tools/call")));

    LONGS_EQUAL(-32602, noParams.errorCode());
    Json arrParams(ask(JsonLineClient::request("2", "tools/call", "[1]")));

    LONGS_EQUAL(-32602, arrParams.errorCode());
    Json noName(ask(JsonLineClient::request("3", "tools/call", "{\"arguments\":{}}")));

    LONGS_EQUAL(-32602, noName.errorCode());
    Json numName(ask(JsonLineClient::request("4", "tools/call", "{\"name\":5}")));

    LONGS_EQUAL(-32602, numName.errorCode());
    Json unknown(ask(JsonLineClient::request("5", "tools/call",
                                             "{\"name\":\"mail_delete\",\"arguments\":{}}")));

    LONGS_EQUAL(-32601, unknown.errorCode());
    Json badArgs(ask(JsonLineClient::request("6", "tools/call",
        "{\"name\":\"mail_list_folders\",\"arguments\":[1]}")));

    LONGS_EQUAL(-32602, badArgs.errorCode());
    Json nullArgs(ask(JsonLineClient::request("7", "tools/call",
        "{\"name\":\"mail_list_folders\",\"arguments\":null}")));

    LONGS_EQUAL(-32602, nullArgs.errorCode());
    LONGS_EQUAL(0, stubBehavior.runs);          /* nothing reached a tool */
}

TEST(ServerProtocol, ArgumentsThatFailTheSchemaAreRefusedBeforeTheyQueue)
{
    Json r(ask(JsonLineClient::toolCall("1", "mail_search",
                                        "{\"folder\":\"x\",\"criteria\":\"ALL\"}")));

    LONGS_EQUAL(-32602, r.errorCode());
    CHECK(r.errorMessage().find("criteria") != std::string::npos);
    Json r2(ask(JsonLineClient::toolCall("2", "mail_set_flag",
        "{\"folder\":\"x\",\"uid\":1,\"uidvalidity\":1,\"flag\":\"\\\\Draft\",\"set\":true}")));

    LONGS_EQUAL(-32602, r2.errorCode());
    LONGS_EQUAL(0, stubBehavior.runs);
}

TEST(ServerProtocol, SetFlagWithDeletedPassesSchemaAndQueues)
{
    stubBehavior.runs = 0;
    Json r(callTool("mail_set_flag",
        "{\"folder\":\"x\",\"uid\":1,\"uidvalidity\":1,\"flag\":\"\\\\Deleted\",\"set\":true}"));

    CHECK(r.ok());
    LONGS_EQUAL(1, stubBehavior.runs);
    STRCMP_EQUAL("mail_set_flag", stubBehavior.lastTool);
}

TEST(ServerProtocol, BatchOperationsPassSchemaAndQueue)
{
    stubBehavior.runs = 0;
    Json rFlag(callTool("mail_set_flag",
        "{\"folder\":\"x\",\"uidvalidity\":1,\"uids\":[1,2],\"flag\":\"\\\\Seen\",\"set\":true}"));
    CHECK(rFlag.ok());
    LONGS_EQUAL(1, stubBehavior.runs);
    STRCMP_EQUAL("mail_set_flag", stubBehavior.lastTool);

    stubBehavior.runs = 0;
    Json rCopy(callTool("mail_copy",
        "{\"folder\":\"x\",\"uidvalidity\":1,\"destination\":\"y\",\"uids\":[1,2]}"));
    CHECK(rCopy.ok());
    LONGS_EQUAL(1, stubBehavior.runs);
    STRCMP_EQUAL("mail_copy", stubBehavior.lastTool);

    stubBehavior.runs = 0;
    Json rStatus(callTool("mail_folder_status",
        "{\"folders\":[\"x\",\"y\"]}"));
    CHECK(rStatus.ok());
    LONGS_EQUAL(1, stubBehavior.runs);
    STRCMP_EQUAL("mail_folder_status", stubBehavior.lastTool);
}

TEST(ServerProtocol, BatchOperationsFailingSchemaAreRefusedBeforeQueue)
{
    stubBehavior.runs = 0;
    /* duplicate UIDs refused */
    Json rDup(callTool("mail_set_flag",
        "{\"folder\":\"x\",\"uidvalidity\":1,\"uids\":[1,1],\"flag\":\"\\\\Seen\",\"set\":true}"));
    LONGS_EQUAL(-32602, rDup.errorCode());
    LONGS_EQUAL(0, stubBehavior.runs);

    /* both uid and uids refused */
    Json rBoth(callTool("mail_copy",
        "{\"folder\":\"x\",\"uidvalidity\":1,\"destination\":\"y\",\"uid\":1,\"uids\":[1]}"));
    LONGS_EQUAL(-32602, rBoth.errorCode());
    LONGS_EQUAL(0, stubBehavior.runs);

    /* duplicate folders refused */
    Json rFoldDup(callTool("mail_folder_status",
        "{\"folders\":[\"x\",\"x\"]}"));
    LONGS_EQUAL(-32602, rFoldDup.errorCode());
    LONGS_EQUAL(0, stubBehavior.runs);
}

TEST(ServerProtocol, ASuccessfulToolCallBecomesATextContentResult)
{
    Json r(callTool("mail_list_folders"));

    CHECK(r.ok());
    CHECK(r.at("error") == NULL);
    CHECK(!r.isErrorFlag());
    cJSON *content = cJSON_GetObjectItemCaseSensitive(r.at("result"), "content");

    LONGS_EQUAL(1, cJSON_GetArraySize(content));
    STRCMP_EQUAL("text",
                 cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content, 0), "type")->valuestring);
    Json inner(r.toolResult());

    CHECK(inner.ok());
    STRCMP_EQUAL("mail_list_folders", inner.str("tool").c_str());
    STRCMP_EQUAL("mail_list_folders", stubBehavior.lastTool);
}

TEST(ServerProtocol, TheToolReceivesTheArgumentsAsSent)
{
    Json r(callTool("mail_folder_status", "{\"folder\":\"{h}INBOX\"}"));

    CHECK(r.ok());
    STRCMP_EQUAL("{\"folder\":\"{h}INBOX\"}", stubBehavior.lastArgs);
    stubBehavior.lastArgs[0] = '\0';
    Json r2(callTool("mail_list_folders", ""));       /* "arguments": nothing */

    (void) r2;
}

TEST(ServerProtocol, ToolFailuresBecomeJsonRpcErrorsWithTheirCode)
{
    static const int codes[] = { -32603, -32008, -32602, -32006, -32007 };

    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        stubBehavior.code = codes[i];
        stubBehavior.message = "tool says no";
        Json r(callTool("mail_list_folders"));

        LONGS_EQUAL(codes[i], r.errorCode());
        STRCMP_EQUAL("tool says no", r.errorMessage().c_str());
        CHECK(r.at("result") == NULL);
    }
}

TEST(ServerProtocol, LargeAndOddResultsPassThroughIntact)
{
    std::string big(300 * 1024, 'q');

    stubBehavior.resultJson = "{\"body\":\"" + big + "\"}";
    Json r(callTool("mail_get_message",
                    "{\"folder\":\"x\",\"uid\":1,\"uidvalidity\":1}"));
    Json inner(r.toolResult());

    CHECK(inner.ok());
    LONGS_EQUAL((int) big.size(), (int) inner.str("body").size());

    stubBehavior.resultJson = "{\"s\":\"caf\\u00e9 \\u2615 \\\"quoted\\\" \\\\ \\n tab\\t\"}";
    Json r2(callTool("mail_list_folders"));
    Json inner2(r2.toolResult());

    STRCMP_EQUAL("caf\xc3\xa9 \xe2\x98\x95 \"quoted\" \\ \n tab\t", inner2.str("s").c_str());
}

TEST(ServerProtocol, ManySequentialCallsKeepIdsAndOrder)
{
    for (int i = 0; i < 100; i++) {
        Json r(callTool("mail_list_folders", "{}", std::to_string(1000 + i)));

        LONGS_EQUAL(1000 + i, (int) r.at("id")->valuedouble);
    }

    LONGS_EQUAL(100, stubBehavior.runs);
}

TEST(ServerProtocol, TheLockRefusesEveryToolCallFirst)
{
    mcp_set_lock(1);

    Json known(callTool("mail_list_folders"));

    LONGS_EQUAL(-32001, known.errorCode());

    /* before validation and before the tool lookup */
    Json bad(ask(JsonLineClient::toolCall("2", "mail_list_folders", "{\"x\":1}")));

    LONGS_EQUAL(-32001, bad.errorCode());
    Json unknown(ask(JsonLineClient::toolCall("3", "no_such_tool", "{}")));

    LONGS_EQUAL(-32001, unknown.errorCode());
    LONGS_EQUAL(0, stubBehavior.runs);

    /* but the protocol itself still answers */
    Json init(ask(JsonLineClient::request("4", "initialize", "{}")));

    CHECK(init.at("result") != NULL);
    Json list(ask(JsonLineClient::request("5", "tools/list")));

    CHECK(list.at("result") != NULL);
    Json ping(ask(JsonLineClient::request("6", "ping")));

    CHECK(ping.at("result") != NULL);

    mcp_set_lock(0);
    Json after(callTool("mail_list_folders", "{}", "7"));

    CHECK(after.at("error") == NULL);
    LONGS_EQUAL(1, stubBehavior.runs);
}

TEST(ServerProtocol, LockEngagedWhileACallWaitsInTheQueueStopsIt)
{
    c->sendLine(JsonLineClient::toolCall("1", "mail_list_folders", "{}"));
    usleep(100 * 1000);                  /* queued, not yet run */
    mcp_set_lock(1);
    h->drain();
    Json r(reply());

    LONGS_EQUAL(-32001, r.errorCode());
    LONGS_EQUAL(0, stubBehavior.runs);
    mcp_set_lock(0);
}

TEST(ServerProtocol, HalfCloseStillGetsItsAnswersThenEndOfFile)
{
    c->sendLine(JsonLineClient::request("1", "ping"));
    c->sendLine(JsonLineClient::toolCall("2", "mail_list_folders", "{}"));
    c->shutdownWrite();
    h->drain();
    Json a(reply());
    Json b(reply());
    std::string more;

    LONGS_EQUAL(1, (int) a.at("id")->valuedouble);
    LONGS_EQUAL(2, (int) b.at("id")->valuedouble);
    CHECK(!c->readLine(more, 2000));
    CHECK(c->atEof());
}

TEST(ServerProtocol, ClientThatClosesMidLineLeavesTheServerHealthy)
{
    JsonLineClient rude;

    CHECK(rude.connectTo(h->socketPath()));
    rude.sendRaw("{\"jsonrpc\":\"2.0\",\"id\":1,\"met");
    rude.closeNow();
    usleep(100 * 1000);
    Json ok(ask(JsonLineClient::request("2", "ping")));

    CHECK(ok.at("result") != NULL);
}

TEST(ServerProtocol, ManyConnectionsAndDisconnectsLeakNoDescriptors)
{
    /* the listener accepts the fixture's connection asynchronously: a round
       trip makes sure its side is open before the descriptors are counted */
    Json sync(ask(JsonLineClient::request("0", "ping")));

    CHECK(sync.ok());
    int before = openDescriptors();
    std::string listBefore = descriptorList();

    for (int i = 0; i < 300; i++) {
        JsonLineClient t;

        CHECK(t.connectTo(h->socketPath()));
        t.sendLine(JsonLineClient::request("1", "ping"));
        if (i % 3 == 0) {
            Json r(t.readJson(2000));

            CHECK(r.ok());
        }
    }

    usleep(300 * 1000);
    std::string listAfter = descriptorList();
    int after = openDescriptors();

    STRCMP_EQUAL(listBefore.c_str(), listAfter.c_str());
    if (before != after) {
        printf("descriptors before %d after %d\n  before: %s\n  after:  %s\n",
               before, after, listBefore.c_str(), listAfter.c_str());
    }

    LONGS_EQUAL(before, after);
    Json ok(ask(JsonLineClient::request("2", "ping")));

    CHECK(ok.at("result") != NULL);
}

TEST(ServerProtocol, ConnectionLimitIsFifteenAtOnce)
{
    JsonLineClient many[20];
    int served = 0;

    for (int i = 0; i < 20; i++) {
        if (many[i].connectTo(h->socketPath())) {
            many[i].sendLine(JsonLineClient::request("1", "ping"));
        }
    }

    for (int i = 0; i < 20; i++) {
        Json r(many[i].readJson(500));

        if (r.ok() && r.at("result") != NULL) {
            served++;
        }
    }

    /* the fixture's own client is one of them */
    LONGS_EQUAL(14, served);

    for (int i = 0; i < 20; i++) {
        many[i].closeNow();
    }

    usleep(200 * 1000);
    Json ok(ask(JsonLineClient::request("2", "ping")));

    CHECK(ok.at("result") != NULL);
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
