/*
 * ServerLifecycleTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
 * Starting and stopping the server: the socket it makes, what it refuses
 * to take over, what a failure of each system call on the way leaves
 * behind, and that a server can be started again in the same process.
 */

TEST_GROUP(ServerLifecycle)
{
    ServerHarness *h;

    void setup()
    {
        h = new ServerHarness();
        stubBehavior.reset();
        Fault::reset();
    }

    void teardown()
    {
        Fault::reset();
        delete h;
        stubBehavior.reset();
    }

    /* a unix socket that listens at path (and is not unlinked afterwards) */
    int listenAt(const std::string &path)
    {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sun;

        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strncpy(sun.sun_path, path.c_str(), sizeof(sun.sun_path) - 1);
        CHECK(bind(fd, (struct sockaddr *) &sun, sizeof(sun)) == 0);
        CHECK(listen(fd, 4) == 0);
        return fd;
    }

    bool pingWorks()
    {
        JsonLineClient c;

        if (!c.connectTo(h->socketPath())) {
            return false;
        }

        c.sendLine(JsonLineClient::request("1", "ping"));
        Json r(c.readJson(2000));

        return r.ok() && r.at("result") != NULL;
    }

    std::string lastError()
    {
        char buf[300];

        mcp_server_last_error(buf, sizeof(buf));
        return buf;
    }
};

TEST(ServerLifecycle, StartMakesAnOwnerOnlySocketAndStopRemovesIt)
{
    h->usePolicy();
    LONGS_EQUAL(0, h->start());
    CHECK(h->running());

    struct stat sb;

    LONGS_EQUAL(0, lstat(h->socketPath().c_str(), &sb));
    CHECK(S_ISSOCK(sb.st_mode));
    LONGS_EQUAL(0600, sb.st_mode & 0777);
    CHECK(pingWorks());

    h->stop();
    CHECK(!h->running());
    CHECK(!fileExists(h->socketPath()));
}

TEST(ServerLifecycle, PolicyThatIsOffStartsNothing)
{
    h->useConfig("{\"mcp\":{\"enabled\":false}}");
    LONGS_EQUAL(0, h->start());
    CHECK(!h->running());
    CHECK(!fileExists(h->dir().child("alpine-mcp.sock")));
    LONGS_EQUAL(-1, mcp_wake_fd());
}

TEST(ServerLifecycle, MissingPolicyStartsNothing)
{
    mcp_config_load((char *) h->home().c_str());
    LONGS_EQUAL(0, h->start());
    CHECK(!h->running());
    CHECK(!fileExists(h->dir().child("alpine-mcp.sock")));
}

TEST(ServerLifecycle, UnsafePolicyFileStartsNothing)
{
    writeFile(h->dir().child(".config/alpine-mcp/config.json"),
              "{\"mcp\":{\"enabled\":true}}", 0644);
    mcp_config_load((char *) h->home().c_str());
    LONGS_EQUAL(0, h->start());
    CHECK(!h->running());
}

TEST(ServerLifecycle, StartingTwiceIsHarmless)
{
    h->usePolicy();
    LONGS_EQUAL(0, h->start());
    LONGS_EQUAL(0, mcp_server_start());
    CHECK(h->running());
    CHECK(pingWorks());
}

TEST(ServerLifecycle, StopWithoutStartIsHarmless)
{
    mcp_server_stop();
    mcp_server_stop();
    CHECK(!h->running());
}

TEST(ServerLifecycle, ServerCanBeStartedAgainInTheSameProcess)
{
    h->usePolicy();
    for (int i = 0; i < 4; i++) {
        LONGS_EQUAL(0, h->start());
        CHECK_TEXT(pingWorks(), "after a restart");
        h->stop();
        CHECK(!fileExists(h->socketPath()));
    }
}

TEST(ServerLifecycle, LiveSocketOfAnotherInstanceIsNotTaken)
{
    h->usePolicy();
    int other = listenAt(h->socketPath());

    LONGS_EQUAL(-1, h->start());
    CHECK(!h->running());
    CHECK(lastError().find("another Alpine") != std::string::npos);

    /* the other instance still owns its socket */
    JsonLineClient c;

    CHECK(c.connectTo(h->socketPath()));
    close(other);
}

TEST(ServerLifecycle, StaleSocketFileIsReplaced)
{
    h->usePolicy();
    int dead = listenAt(h->socketPath());

    close(dead);                        /* the file stays, nobody listens */
    CHECK(fileExists(h->socketPath()));
    LONGS_EQUAL(0, h->start());
    CHECK(pingWorks());
}

TEST(ServerLifecycle, FileThatIsNotASocketIsNeverRemoved)
{
    h->usePolicy();
    CHECK(writeFile(h->socketPath(), "precious", 0600));
    LONGS_EQUAL(-1, h->start());
    CHECK(!h->running());
    STRCMP_EQUAL("precious", readFile(h->socketPath()).c_str());
    CHECK(lastError().find("not our socket") != std::string::npos);
}

TEST(ServerLifecycle, SymlinkAtThePathIsNotFollowed)
{
    h->usePolicy();
    CHECK(writeFile(h->dir().child("target"), "precious", 0600));
    CHECK(symlink(h->dir().child("target").c_str(), h->socketPath().c_str()) == 0);
    LONGS_EQUAL(-1, h->start());
    STRCMP_EQUAL("precious", readFile(h->dir().child("target")).c_str());
}

TEST(ServerLifecycle, PathThatDoesNotFitSunPathFails)
{
    std::string deep = h->dir().child("d");

    CHECK(makeDir(deep, 0700));
    while (deep.size() < 120) {
        deep += "/xxxxxxxxxx";
        CHECK(makeDir(deep, 0700));
    }

    EnvGuard xdg("XDG_RUNTIME_DIR", deep.c_str());

    h->usePolicy();
    LONGS_EQUAL(-1, h->start());
    CHECK(!h->running());
    CHECK(lastError().find("too long") != std::string::npos);
}

TEST(ServerLifecycle, EachSystemCallThatFailsLeavesNothingBehind)
{
    static const struct {
        Fault::Call call;
        int nth;
        const char *what;
    } plans[] = {
        { Fault::Pipe2, 1, "first pipe" },
        { Fault::Pipe2, 2, "second pipe" },
        { Fault::Socket, 1, "listening socket" },
        { Fault::Bind, 1, "bind" },
        { Fault::Listen, 1, "listen" },
        { Fault::PthreadCreate, 1, "thread" },
    };

    h->usePolicy();
    int before = openDescriptors();

    for (size_t i = 0; i < sizeof(plans) / sizeof(plans[0]); i++) {
        Fault::reset();
        Fault::failNth(plans[i].call, plans[i].nth, EMFILE);
        LONGS_EQUAL_TEXT(-1, h->start(), plans[i].what);
        CHECK_TEXT(!h->running(), plans[i].what);
        CHECK_TEXT(!fileExists(h->socketPath()), plans[i].what);
        LONGS_EQUAL_TEXT(before, openDescriptors(), plans[i].what);
        CHECK_TEXT(!lastError().empty(), plans[i].what);
    }

    Fault::reset();
    LONGS_EQUAL(0, h->start());                 /* and it still starts cleanly */
    CHECK(pingWorks());
}

TEST(ServerLifecycle, StopDropsQueuedAndAnsweredWorkWithoutLeakingDescriptors)
{
    h->usePolicy();
    LONGS_EQUAL(0, h->start());
    int before = openDescriptors();

    for (int round = 0; round < 3; round++) {
        JsonLineClient c;

        CHECK(c.connectTo(h->socketPath()));
        for (int i = 0; i < 5; i++) {
            c.sendLine(JsonLineClient::toolCall(std::to_string(i), "mail_list_folders", "{}"));
        }

        usleep(100 * 1000);         /* let the listener queue them; no drain */
        h->stop();
        CHECK(!h->running());
        LONGS_EQUAL(0, h->start());
    }

    usleep(200 * 1000);
    LONGS_EQUAL(before, openDescriptors());
    CHECK(pingWorks());
}

TEST(ServerLifecycle, StopWithAParkedCallWaitingFreesItAndTheServerRestarts)
{
    h->usePolicy();
    LONGS_EQUAL(0, h->start());
    stubBehavior.park = true;
    stubBehavior.prompt = "may I?";

    JsonLineClient c;

    CHECK(c.connectTo(h->socketPath()));
    c.sendLine(JsonLineClient::toolCall("1", "mail_send",
               "{\"to\":\"x@example.net\",\"idempotency_key\":\"abcdefgh\"}"));
    CHECK(h->drain());
    LONGS_EQUAL(1, mcp_pending_confirms());

    h->stop();
    LONGS_EQUAL(0, mcp_pending_confirms());
    stubBehavior.park = false;
    LONGS_EQUAL(0, h->start());
    CHECK(pingWorks());
    LONGS_EQUAL(0, mcp_pending_confirms());
}

TEST(ServerLifecycle, SocketPathAccessorMatchesTheListeningPath)
{
    h->usePolicy();
    LONGS_EQUAL(0, h->start());
    char buf[256];

    CHECK(mcp_socket_path(buf, sizeof(buf)) != NULL);
    STRCMP_EQUAL(h->dir().child("alpine-mcp.sock").c_str(), buf);
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
