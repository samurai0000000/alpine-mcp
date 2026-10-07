/*
 * MonitorIntegrationTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <string>
#include <unistd.h>

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

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
