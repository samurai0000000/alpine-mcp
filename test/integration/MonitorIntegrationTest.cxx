/*
 * MonitorIntegrationTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <string>
#include <unistd.h>
#include <vector>

#include "FakeGateway.hxx"
#include "IntegrationRig.hxx"

#include "CppUTest/TestHarness.h"

/*
 * The MCP monitor screen (Setup, then i) in a real Alpine: what it shows,
 * the lock keys and that they reach config.json, and the aimon keys
 * against a stand-in gateway.  The aimon address here is always the
 * stand-in's, never the real gateway's port.
 */

namespace
{
    bool
    configHas(const std::string &home, const char *key, bool value)
    {
        Json cfg(cJSON_Parse(readFile(home + "/.config/alpine-mcp/config.json").c_str()));
        cJSON *v = cfg.at(key);

        return v != NULL && cJSON_IsBool(v) && cJSON_IsTrue(v) == (value ? 1 : 0);
    }

    std::string
    configAimonHost(const std::string &home)
    {
        Json cfg(cJSON_Parse(readFile(home + "/.config/alpine-mcp/config.json").c_str()));
        cJSON *aimon = cfg.at("aimon");

        if (aimon == NULL) {
            return "";
        }

        cJSON *host = cJSON_GetObjectItemCaseSensitive(aimon, "host");

        return (host != NULL && cJSON_IsString(host) && host->valuestring != NULL)
            ? host->valuestring : "";
    }

    int
    configAimonPort(const std::string &home)
    {
        Json cfg(cJSON_Parse(readFile(home + "/.config/alpine-mcp/config.json").c_str()));
        cJSON *aimon = cfg.at("aimon");

        if (aimon == NULL) {
            return -1;
        }

        cJSON *port = cJSON_GetObjectItemCaseSensitive(aimon, "port");

        return (port != NULL && cJSON_IsNumber(port)) ? port->valueint : -1;
    }

    std::vector<std::string>
    configAllowedRecipients(const std::string &home)
    {
        std::vector<std::string> res;
        Json cfg(cJSON_Parse(readFile(home + "/.config/alpine-mcp/config.json").c_str()));
        cJSON *arr = cfg.at("allowed_recipients");

        if (arr == NULL || !cJSON_IsArray(arr)) {
            return res;
        }

        cJSON *item = NULL;

        cJSON_ArrayForEach(item, arr) {
            if (cJSON_IsString(item) && item->valuestring != NULL) {
                res.push_back(item->valuestring);
            }
        }

        return res;
    }

    std::string
    stripAnsi(const std::string &in)
    {
        std::string out;
        bool inEsc = false;

        for (size_t i = 0; i < in.size(); i++) {
            if (in[i] == '\033') {
                inEsc = true;
            } else if (inEsc) {
                if ((in[i] >= 'A' && in[i] <= 'Z')
                    || (in[i] >= 'a' && in[i] <= 'z') || in[i] == '~') {
                    inEsc = false;
                }
            } else {
                out.push_back(in[i]);
            }
        }

        return out;
    }
}

TEST_GROUP(MonitorIntegration)
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

    size_t openMonitor()
    {
        size_t m = rig->a.mark();

        rig->a.sendKeys("si");
        CHECK(rig->a.waitForText("MCP MONITOR", 5000, m));
        CHECK(rig->a.waitForText("Last error:", 2000, m));
        return m;
    }

    /* e goes back to the screen the monitor was opened from */
    void leaveMonitor()
    {
        size_t m = rig->a.mark();

        rig->a.sendKeys("e");
        CHECK(rig->a.waitForText("INDEX", 5000, m));
    }
};

TEST(MonitorIntegration, ShowsTheLockTheListTheSocketAndTheAimonState)
{
    CHECK(rig->up());
    size_t m = openMonitor();
    std::string screen = rig->a.since(m);

    CHECK(screen.find("Operator lock:         OFF") != std::string::npos);
    CHECK(screen.find("1 entry") != std::string::npos);
    CHECK(screen.find("open at " + rig->a.socketPath()) != std::string::npos);
    CHECK(screen.find("aimon state:           off") != std::string::npos);
    CHECK(screen.find("Pending records:       0") != std::string::npos);
    CHECK(screen.find("Last error:            none") != std::string::npos);
    leaveMonitor();
}

TEST(MonitorIntegration, LockAndUnlockTakeEffectAtOnceAndReachConfigJson)
{
    CHECK(rig->up());
    size_t m = openMonitor();

    rig->a.sendKeys("l");
    CHECK(rig->a.waitForText("ON ", 5000, m));
    CHECK(configHas(rig->a.home(), "operator_lock", true));
    Json locked(rig->call("mail_list_folders", "{}"));

    LONGS_EQUAL(-32001, locked.errorCode());

    m = rig->a.mark();
    rig->a.sendKeys("u");
    CHECK(rig->a.waitForText("OFF", 5000, m));
    CHECK(configHas(rig->a.home(), "operator_lock", false));
    Json open(rig->call("mail_list_folders", "{}"));

    CHECK(open.ok());
    CHECK(open.at("error") == NULL);
    LONGS_EQUAL(0600, fileMode(rig->a.home() + "/.config/alpine-mcp/config.json"));
    leaveMonitor();
}

TEST(MonitorIntegration, ALockedStartIsShownAsOn)
{
    rig->a.useConfig("{\"mcp\":{\"enabled\":true},\"operator_lock\":true,"
                     "\"allowed_recipients\":[]}");
    CHECK(rig->up());
    size_t m = openMonitor();
    std::string screen = rig->a.since(m);

    CHECK(screen.find("Operator lock:         ON") != std::string::npos);
    CHECK(screen.find("0 entries") != std::string::npos);
    leaveMonitor();
}

TEST(MonitorIntegration, TheAimonKeysConnectReconnectAndDisconnect)
{
    FakeGateway gw;

    CHECK(gw.start());
    rig->a.useConfig("{\"mcp\":{\"enabled\":true},\"operator_lock\":false,"
                     "\"allowed_recipients\":[],\"aimon\":{\"enabled\":false,"
                     "\"host\":\"127.0.0.1\",\"port\":" + std::to_string(gw.port()) + "}}");
    CHECK(rig->up());
    openMonitor();
    LONGS_EQUAL(0, gw.registrations());

    rig->a.sendKeys("a");                           /* on */
    CHECK(gw.waitForRegistrations(1, 8000));
    usleep(500 * 1000);

    rig->a.sendKeys("r");                           /* reconnect, at once */
    CHECK(gw.waitForRegistrations(2, 8000));

    size_t m = rig->a.mark();

    rig->a.sendKeys("a");                           /* off */
    CHECK(gw.waitForEof(8000));
    usleep(1500 * 1000);
    int regs = gw.registrations();

    usleep(1500 * 1000);
    LONGS_EQUAL(regs, gw.registrations());
    (void) m;
    leaveMonitor();
}

TEST(MonitorIntegration, LeavingTheMonitorGivesBackTheScreenItCameFrom)
{
    CHECK(rig->up());
    openMonitor();
    leaveMonitor();

    /* and the tools still work */
    Json l(rig->callOk("mail_list_folders", "{}"));

    CHECK(l.ok());
}

TEST(MonitorIntegration, TopHeaderDisplaysMcpBadgeOnIdle)
{
    CHECK(rig->up());
    CHECK(rig->a.waitForText("ALPINE(MCP)", 5000));
}

TEST(MonitorIntegration, TopHeaderDisplaysQuestionWhenConfigMisconfigured)
{
    rig->a.useConfig("{invalid-json");
    CHECK(rig->a.start(20000, false));
    CHECK(rig->a.waitForText("ALPINE?MCP?", 5000));
}

TEST(MonitorIntegration, MonitorAppearsOnFirstPageOfSetupBetweenExitAndPrinter)
{
    CHECK(rig->up());
    size_t m = rig->a.mark();

    rig->a.sendKeys("s");
    CHECK(rig->a.waitForText("(E) Exit Setup:", 5000, m));
    CHECK(rig->a.waitForText("(I) monItor:", 5000, m));
    CHECK(rig->a.waitForText("(P) Printer:", 5000, m));

    std::string screen = rig->a.since(m);
    size_t posE = screen.find("(E) Exit Setup:");
    size_t posI = screen.find("(I) monItor:");
    size_t posP = screen.find("(P) Printer:");

    CHECK(posE != std::string::npos);
    CHECK(posI != std::string::npos);
    CHECK(posP != std::string::npos);
    CHECK(posE < posI);
    CHECK(posI < posP);

    std::string plain = stripAnsi(screen);
    CHECK(plain.find("I monItor") != std::string::npos);

    rig->a.sendKeys("e");
    CHECK(rig->a.waitForText("INDEX", 5000, m));
}

TEST(MonitorIntegration, ArrowKeysAndInPlaceFieldEditing)
{
    CHECK(rig->up());
    size_t m = openMonitor();

    /* Toggle Operator lock on with Enter (\r) */
    rig->a.sendKeys("\r");
    CHECK(rig->a.waitForText("ON ", 5000, m));
    CHECK(configHas(rig->a.home(), "operator_lock", true));

    /* Toggle Operator lock back off with 'x' */
    m = rig->a.mark();
    rig->a.sendKeys("x");
    CHECK(rig->a.waitForText("OFF", 5000, m));
    CHECK(configHas(rig->a.home(), "operator_lock", false));

    /* Navigate down twice to aimon host row and change host */
    rig->a.sendKeys("\x1b[B\x1b[B");
    usleep(100 * 1000);
    m = rig->a.mark();
    rig->a.sendKeys("c");
    CHECK(rig->a.waitForText("Enter aimon host:", 3000, m));
    rig->a.sendKeys("\x01\x0b" "127.0.0.2\r");
    CHECK(rig->a.waitForText("127.0.0.2", 3000, m));
    CHECK(configAimonHost(rig->a.home()) == "127.0.0.2");

    /* Navigate down once to aimon port row and change port */
    rig->a.sendKeys("\x1b[B");
    usleep(100 * 1000);
    m = rig->a.mark();
    rig->a.sendKeys("c");
    CHECK(rig->a.waitForText("Enter aimon port:", 3000, m));
    rig->a.sendKeys("\x01\x0b" "4999\r");
    CHECK(rig->a.waitForText("4999", 3000, m));
    LONGS_EQUAL(4999, configAimonPort(rig->a.home()));

    leaveMonitor();
}

TEST(MonitorIntegration, AllowListAddAndDeleteItems)
{
    rig->a.useConfig("{\"mcp\":{\"enabled\":true},\"operator_lock\":false,"
                     "\"allowed_recipients\":[],\"aimon\":{\"enabled\":false,"
                     "\"host\":\"127.0.0.1\",\"port\":3885}}");
    CHECK(rig->up());
    size_t m = openMonitor();

    /* Navigate down 4 times to Recipient allow-list row */
    rig->a.sendKeys("\x1b[B\x1b[B\x1b[B\x1b[B");
    usleep(100 * 1000);

    m = rig->a.mark();
    rig->a.sendKeys("a");
    CHECK(rig->a.waitForText("Add allowed recipient:", 3000, m));
    rig->a.sendKeys("alice@example.org\r");
    CHECK(rig->a.waitForText("1 entry", 3000, m));
    CHECK(rig->a.waitForText("alice@example.org", 3000, m));

    std::vector<std::string> recipients = configAllowedRecipients(rig->a.home());
    LONGS_EQUAL(1, recipients.size());
    CHECK(recipients[0] == "alice@example.org");

    /* Delete the entry (already focused on alice@example.org) */
    m = rig->a.mark();
    rig->a.sendKeys("d");
    CHECK(rig->a.waitForText("0 entries", 3000, m));

    recipients = configAllowedRecipients(rig->a.home());
    CHECK(recipients.empty());

    leaveMonitor();
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
