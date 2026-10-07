/*
 * ProtocolIntegrationTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "IntegrationRig.hxx"

#include "CppUTest/TestHarness.h"

/*
 * The protocol and the policy file as a real Alpine applies them: the
 * handshake, bad input, the lock, a policy file that must not be trusted,
 * a second Alpine, and that Alpine stays usable through all of it.
 */

namespace
{
    const char *initParams =
        "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"it\",\"version\":\"0\"}}";

    bool
    alpineStillAnswers(JsonLineClient &c)
    {
        c.sendLine(JsonLineClient::request("77", "ping"));
        Json r(c.readJson(10000));

        return r.ok() && r.at("error") == NULL;
    }
}

TEST_GROUP(ProtocolIntegration)
{
    ScopedThreadSafeTracking threadSafe;
    Rig *rig;

    void setup() { rig = new Rig(); }

    void teardown()
    {
        rig->c.closeNow();
        CHECK(rig->a.stop());
        delete rig;
    }
};

TEST(ProtocolIntegration, InitializeThenListsTheTenTools)
{
    CHECK(rig->up());
    rig->c.sendLine(JsonLineClient::request("1", "initialize", initParams));
    Json init(rig->c.readJson(10000));

    CHECK(init.ok());
    CHECK(init.at("error") == NULL);
    STRCMP_EQUAL("2025-11-25",
                 cJSON_GetObjectItemCaseSensitive(init.at("result"),
                                                  "protocolVersion")->valuestring);
    rig->c.sendLine("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    rig->c.sendLine(JsonLineClient::request("2", "tools/list"));
    Json list(rig->c.readJson(10000));

    CHECK(list.ok());
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(list.at("result"), "tools");

    LONGS_EQUAL(10, cJSON_GetArraySize(tools));
    cJSON *t = NULL;

    cJSON_ArrayForEach(t, tools) {
        CHECK(cJSON_GetObjectItemCaseSensitive(t, "name") != NULL);
        CHECK(cJSON_GetObjectItemCaseSensitive(t, "description") != NULL);
        CHECK(cJSON_GetObjectItemCaseSensitive(t, "inputSchema") != NULL);
    }
}

TEST(ProtocolIntegration, NoToolDeletesOrExpungesOrMoves)
{
    CHECK(rig->up());
    rig->c.sendLine(JsonLineClient::request("2", "tools/list"));
    Json list(rig->c.readJson(10000));
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(list.at("result"), "tools");
    cJSON *t = NULL;

    cJSON_ArrayForEach(t, tools) {
        std::string name = cJSON_GetObjectItemCaseSensitive(t, "name")->valuestring;

        CHECK_TEXT(name.find("delete") == std::string::npos, name.c_str());
        CHECK_TEXT(name.find("expunge") == std::string::npos, name.c_str());
        CHECK_TEXT(name.find("move") == std::string::npos, name.c_str());
    }
}

TEST(ProtocolIntegration, BadInputGetsAnErrorAndTheConnectionLives)
{
    CHECK(rig->up());
    rig->c.sendLine("this is not json");
    Json bad(rig->c.readJson(10000));

    CHECK(bad.ok());
    LONGS_EQUAL(-32700, bad.errorCode());

    rig->c.sendLine(JsonLineClient::request("5", "no/such/method"));
    Json unknown(rig->c.readJson(10000));

    LONGS_EQUAL(-32601, unknown.errorCode());

    Json noTool(rig->call("mail_expunge", "{}"));

    CHECK(noTool.ok());
    CHECK(noTool.errorCode() != 0);

    Json extra(rig->call("mail_list_folders", "{\"surprise\":1}"));

    LONGS_EQUAL(-32602, extra.errorCode());
    CHECK(alpineStillAnswers(rig->c));
}

TEST(ProtocolIntegration, AnOversizedLineEndsThatConnectionOnly)
{
    CHECK(rig->up());
    JsonLineClient other;

    CHECK(other.connectTo(rig->a.socketPath()));
    std::string big(5 * 1024 * 1024, 'x');

    rig->c.sendRaw(big);
    std::string line;

    rig->c.readLine(line, 10000);               /* an error line or the end */
    CHECK(rig->c.atEof() || !line.empty());
    CHECK(alpineStillAnswers(other));
}

TEST(ProtocolIntegration, ManyClientsAtOnceAreAllServed)
{
    CHECK(rig->up());
    JsonLineClient cs[6];

    for (int i = 0; i < 6; i++) {
        CHECK(cs[i].connectTo(rig->a.socketPath()));
        cs[i].sendLine(JsonLineClient::toolCall(std::to_string(100 + i),
                                                "mail_list_folders", "{}"));
    }

    for (int i = 0; i < 6; i++) {
        Json r(cs[i].readJson(15000));

        CHECK(r.ok());
        LONGS_EQUAL(100 + i, (int) r.at("id")->valuedouble);
        CHECK(r.at("error") == NULL);
    }
}

TEST(ProtocolIntegration, ClientsThatVanishMidRequestDoNoHarm)
{
    CHECK(rig->up());

    for (int i = 0; i < 5; i++) {
        JsonLineClient gone;

        CHECK(gone.connectTo(rig->a.socketPath()));
        gone.sendLine(JsonLineClient::toolCall("1", "mail_list_folders", "{}"));
        gone.closeNow();
        JsonLineClient half;

        CHECK(half.connectTo(rig->a.socketPath()));
        half.sendRaw("{\"jsonrpc\":\"2.0\",\"id\":1,\"met");
        half.closeNow();
    }

    CHECK(alpineStillAnswers(rig->c));
}

TEST(ProtocolIntegration, TheLockRefusesToolsButNotTheHandshake)
{
    rig->a.useConfig("{\"mcp\":{\"enabled\":true},\"operator_lock\":true,"
                     "\"allowed_recipients\":[],\"aimon\":{\"enabled\":false}}");
    CHECK(rig->up());
    Json r(rig->call("mail_list_folders", "{}"));

    CHECK(r.ok());
    LONGS_EQUAL(-32001, r.errorCode());
    rig->c.sendLine(JsonLineClient::request("3", "tools/list"));
    Json list(rig->c.readJson(10000));

    CHECK(list.ok());
    CHECK(list.at("error") == NULL);
    CHECK(rig->smtp.count() == 0);
}

TEST(ProtocolIntegration, NoSocketWhenMcpIsDisabled)
{
    rig->a.useConfig("{\"mcp\":{\"enabled\":false}}");
    CHECK(rig->smtp.start());
    rig->a.setSmtpPort(rig->smtp.port());
    CHECK(rig->a.start(20000, false));
    CHECK(!fileExists(rig->a.socketPath()));
    CHECK(rig->a.running());
}

TEST(ProtocolIntegration, NoSocketWhenThereIsNoPolicyFile)
{
    unlink((rig->a.home() + "/.config/alpine-mcp/config.json").c_str());
    CHECK(rig->a.start(20000, false));
    CHECK(!fileExists(rig->a.socketPath()));
    CHECK(rig->a.running());
}

TEST(ProtocolIntegration, ARecognisablyUnsafePolicyFileKeepsMcpOff)
{
    const int modes[] = { 0644, 0666, 0640, 0604 };

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        AlpineHarness a;

        a.useConfig("{\"mcp\":{\"enabled\":true},\"operator_lock\":false,"
                    "\"allowed_recipients\":[\"*@example.org\"]}", modes[i]);
        CHECK(a.start(20000, false));
        CHECK_TEXT(!fileExists(a.socketPath()), "socket exists");
        CHECK(a.stop());
    }
}

TEST(ProtocolIntegration, ABrokenPolicyFileKeepsMcpOff)
{
    const char *texts[] = { "", "{", "[]", "null", "{\"mcp\":\"yes\"}",
                            "{\"mcp\":{\"enabled\":1}}" };

    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        AlpineHarness a;

        a.useConfig(texts[i]);
        CHECK(a.start(20000, false));
        CHECK_TEXT(!fileExists(a.socketPath()), texts[i]);
        CHECK(a.stop());
    }
}

TEST(ProtocolIntegration, ASecondAlpineDoesNotTakeTheSocketAndSaysSo)
{
    CHECK(rig->up());
    AlpineHarness second;

    second.shareRuntimeWith(rig->a);
    CHECK(second.start(20000, false));
    CHECK(second.waitForText("another Alpine is serving", 5000));
    CHECK(second.stop());

    /* the first one still has its socket */
    CHECK(fileExists(rig->a.socketPath()));
    Json r(rig->callOk("mail_list_folders", "{}"));

    CHECK(r.ok());
}

TEST(ProtocolIntegration, TheSocketIsPrivateToTheUser)
{
    CHECK(rig->up());
    struct stat st;

    LONGS_EQUAL(0, stat(rig->a.socketPath().c_str(), &st));
    CHECK(S_ISSOCK(st.st_mode));
    LONGS_EQUAL(0, st.st_mode & 077);
}

TEST(ProtocolIntegration, TheSocketGoesAwayWhenAlpineQuits)
{
    CHECK(rig->up());
    rig->c.closeNow();
    CHECK(rig->a.stop());
    CHECK(!fileExists(rig->a.socketPath()));
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
