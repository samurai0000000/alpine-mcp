/*
 * IdempotencyTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */


#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "FaultInjection.hxx"
#include "TestSupport.hxx"

extern "C" {
#include "mcpjson.h"
#include "mcp.h"
#include "mcpint.h"
}

/* CppUTest after the standard headers: its new macros break them otherwise */
#include "CppUTest/TestHarness.h"
#include "CppUTest/TestHarness_c.h"

/* free() of memory the add-on's C code allocated with the tracked malloc */
#include "CppUTest/MemoryLeakDetectorMallocMacros.h"


/*
 * The idempotency store: one file per key under
 * $XDG_STATE_HOME/alpine-mcp/idempotency (or $HOME/.local/state/...).
 * A send or a draft writes "pending" before the effect and "done" after,
 * so a retry never repeats the effect.
 */

TEST_GROUP(Idempotency)
{
    TempDir *tmp;
    EnvGuard *xdg;
    EnvGuard *home;
    std::string dir;

    void setup()
    {
        tmp = new TempDir();
        xdg = new EnvGuard("XDG_STATE_HOME", tmp->child("state").c_str());
        home = new EnvGuard("HOME", tmp->path().c_str());
        dir = tmp->child("state/alpine-mcp/idempotency");
        Fault::reset();
    }

    void teardown()
    {
        Fault::reset();
        Fault::removeJsonHooks();
        Fault::failAllocNth(0);
        delete home;
        delete xdg;
        delete tmp;
    }

    int begin(const char *key, const char *hash, const char *op = "mail_send")
    {
        char *res = NULL;
        int rc = mcp_idem_begin(op, key, hash, &res);

        if (res != NULL) {
            free(res);
        }

        return rc;
    }

    int check(const char *key, const char *hash, std::string *result = NULL,
              const char *op = "mail_send")
    {
        char *res = NULL;
        int rc = mcp_idem_check(op, key, hash, &res);

        if (res != NULL) {
            if (result != NULL) {
                *result = res;
            }

            free(res);
        }

        return rc;
    }
};

TEST(Idempotency, FirstBeginWritesAPendingRecordWithTightPermissions)
{
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h1"));
    CHECK(fileExists(dir + "/key-aaaa-0001"));
    LONGS_EQUAL(0600, fileMode(dir + "/key-aaaa-0001"));
    LONGS_EQUAL(0700, fileMode(dir));
    CHECK(readFile(dir + "/key-aaaa-0001").find("\"pending\"")
          != std::string::npos);
}

TEST(Idempotency, SecondBeginWhilePendingIsRefused)
{
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h1"));
    LONGS_EQUAL(MCP_IDEM_PENDING, begin("key-aaaa-0001", "h1"));
    LONGS_EQUAL(MCP_IDEM_MISMATCH, begin("key-aaaa-0001", "h2"));
}

TEST(Idempotency, DoneRecordReturnsTheStoredResult)
{
    char *res = NULL;

    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h1"));
    LONGS_EQUAL(0, mcp_idem_commit("mail_send", "key-aaaa-0001", "h1",
                                   "{\"sent\":true}"));
    LONGS_EQUAL(MCP_IDEM_DONE,
                mcp_idem_begin("mail_send", "key-aaaa-0001", "h1", &res));
    CHECK(res != NULL);
    STRCMP_EQUAL("{\"sent\":true}", res);
    free(res);
    LONGS_EQUAL(0600, fileMode(dir + "/key-aaaa-0001"));
}

TEST(Idempotency, DoneRecordWithOtherArgumentsOrOtherToolIsAMismatch)
{
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h1"));
    LONGS_EQUAL(0, mcp_idem_commit("mail_send", "key-aaaa-0001", "h1", "{}"));
    LONGS_EQUAL(MCP_IDEM_MISMATCH, begin("key-aaaa-0001", "h2"));
    LONGS_EQUAL(MCP_IDEM_MISMATCH,
                begin("key-aaaa-0001", "h1", "mail_create_draft"));
}

TEST(Idempotency, CheckNeverCreatesARecord)
{
    std::string result;

    LONGS_EQUAL(MCP_IDEM_NEW, check("key-aaaa-0001", "h1"));
    CHECK(!fileExists(dir + "/key-aaaa-0001"));
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h1"));
    LONGS_EQUAL(MCP_IDEM_PENDING, check("key-aaaa-0001", "h1"));
    LONGS_EQUAL(MCP_IDEM_MISMATCH, check("key-aaaa-0001", "h2"));
    LONGS_EQUAL(0, mcp_idem_commit("mail_send", "key-aaaa-0001", "h1", "{\"a\":1}"));
    LONGS_EQUAL(MCP_IDEM_DONE, check("key-aaaa-0001", "h1", &result));
    STRCMP_EQUAL("{\"a\":1}", result.c_str());
    LONGS_EQUAL(MCP_IDEM_MISMATCH, check("key-aaaa-0001", "h1", NULL,
                                         "mail_create_draft"));
}

TEST(Idempotency, AbortForgetsAPendingRecordSoARetryMayRun)
{
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h1"));
    mcp_idem_abort("key-aaaa-0001");
    CHECK(!fileExists(dir + "/key-aaaa-0001"));
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h1"));
}

TEST(Idempotency, PendingListHoldsOnlyPendingRecords)
{
    char **names = NULL;
    int n = -1;

    LONGS_EQUAL(MCP_IDEM_NEW, begin("pending-0001", "h"));
    LONGS_EQUAL(MCP_IDEM_NEW, begin("pending-0002", "h"));
    LONGS_EQUAL(MCP_IDEM_NEW, begin("finished-0001", "h"));
    LONGS_EQUAL(0, mcp_idem_commit("mail_send", "finished-0001", "h", "{}"));

    LONGS_EQUAL(0, mcp_idem_pending(&names, &n));
    LONGS_EQUAL(2, n);
    std::vector<std::string> got;

    for (int i = 0; i < n; i++) {
        got.push_back(names[i]);
        free(names[i]);
    }

    free(names);
    CHECK(got[0] == "pending-0001" || got[1] == "pending-0001");
    CHECK(got[0] == "pending-0002" || got[1] == "pending-0002");
}

TEST(Idempotency, DeletePendingRemovesOnlyPendingRecords)
{
    LONGS_EQUAL(MCP_IDEM_NEW, begin("pending-0001", "h"));
    LONGS_EQUAL(MCP_IDEM_NEW, begin("finished-0001", "h"));
    LONGS_EQUAL(0, mcp_idem_commit("mail_send", "finished-0001", "h", "{}"));

    LONGS_EQUAL(0, mcp_idem_delete_pending("pending-0001"));
    CHECK(!fileExists(dir + "/pending-0001"));
    LONGS_EQUAL(-1, mcp_idem_delete_pending("finished-0001"));
    CHECK(fileExists(dir + "/finished-0001"));
    LONGS_EQUAL(-1, mcp_idem_delete_pending("no-such-record"));
}

TEST(Idempotency, DeletePendingRefusesNamesThatLeaveTheDirectory)
{
    CHECK(makeDir(tmp->child("state"), 0700));
    CHECK(makeDir(tmp->child("state/alpine-mcp"), 0700));
    CHECK(writeFile(tmp->child("state/alpine-mcp/outside"),
                    "{\"op\":\"x\",\"hash\":\"h\",\"state\":\"pending\"}", 0600));
    LONGS_EQUAL(-1, mcp_idem_delete_pending("../outside"));
    LONGS_EQUAL(-1, mcp_idem_delete_pending(".."));
    LONGS_EQUAL(-1, mcp_idem_delete_pending("."));
    LONGS_EQUAL(-1, mcp_idem_delete_pending("a/b"));
    LONGS_EQUAL(-1, mcp_idem_delete_pending(""));
    LONGS_EQUAL(-1, mcp_idem_delete_pending(NULL));
    CHECK(fileExists(tmp->child("state/alpine-mcp/outside")));
}

TEST(Idempotency, KeysThatCouldLeaveTheDirectoryAreRefused)
{
    /* the tools validate keys, but the store must not rely on it */
    CHECK(makeDir(tmp->child("state"), 0700));
    CHECK(makeDir(tmp->child("state/alpine-mcp"), 0700));
    CHECK(writeFile(tmp->child("state/alpine-mcp/outside"), "x", 0600));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("../outside", "h"));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("../../escape", "h"));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("a/b", "h"));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("/etc/passwd", "h"));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("..", "h"));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin(".", "h"));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("", "h"));
    LONGS_EQUAL(MCP_IDEM_ERROR, check("../outside", "h"));
    LONGS_EQUAL(-1, mcp_idem_commit("mail_send", "../outside", "h", "{}"));
    mcp_idem_abort("../outside");
    STRCMP_EQUAL("x", readFile(tmp->child("state/alpine-mcp/outside")).c_str());
    CHECK(!fileExists(tmp->child("escape")));
}

TEST(Idempotency, KeyLengthBoundaries)
{
    LONGS_EQUAL(MCP_IDEM_NEW, begin("k", "h"));
    LONGS_EQUAL(MCP_IDEM_NEW, begin(std::string(128, 'a').c_str(), "h"));
    LONGS_EQUAL(MCP_IDEM_NEW, begin(std::string(255, 'b').c_str(), "h"));
    /* NAME_MAX is 255: one more cannot be created */
    LONGS_EQUAL(MCP_IDEM_ERROR, begin(std::string(256, 'c').c_str(), "h"));
}

TEST(Idempotency, StateDirectoryFallsBackToHome)
{
    EnvGuard noXdg("XDG_STATE_HOME", NULL);

    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h"));
    CHECK(fileExists(tmp->child(".local/state/alpine-mcp/idempotency/key-aaaa-0001")));
}

TEST(Idempotency, RelativeXdgStateHomeIsIgnored)
{
    EnvGuard rel("XDG_STATE_HOME", "relative/path");

    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h"));
    CHECK(fileExists(tmp->child(".local/state/alpine-mcp/idempotency/key-aaaa-0001")));
    CHECK(!fileExists("relative/path"));
}

TEST(Idempotency, NoStateDirectoryAtAllIsAnError)
{
    EnvGuard noXdg("XDG_STATE_HOME", NULL);
    EnvGuard noHome("HOME", NULL);

    LONGS_EQUAL(MCP_IDEM_ERROR, begin("key-aaaa-0001", "h"));
}

TEST(Idempotency, CorruptRecordsAreAnErrorNotACrash)
{
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h"));

    CHECK(writeFile(dir + "/key-aaaa-0001", "not json at all", 0600));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("key-aaaa-0001", "h"));
    LONGS_EQUAL(MCP_IDEM_ERROR, check("key-aaaa-0001", "h"));

    CHECK(writeFile(dir + "/key-aaaa-0001", "", 0600));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("key-aaaa-0001", "h"));

    CHECK(writeFile(dir + "/key-aaaa-0001", "[1,2,3]", 0600));
    CHECK(begin("key-aaaa-0001", "h") != MCP_IDEM_NEW);

    CHECK(writeFile(dir + "/key-aaaa-0001", "{\"state\":\"done\"}", 0600));
    CHECK(begin("key-aaaa-0001", "h") != MCP_IDEM_DONE);
}

TEST(Idempotency, RecordLargerThanOneMegabyteIsRefused)
{
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h"));
    CHECK(writeFile(dir + "/key-aaaa-0001",
                    std::string(1024 * 1024 + 1, ' '), 0600));
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("key-aaaa-0001", "h"));
}

/* ---- fault injection ---- */

TEST(Idempotency, FailedCreateLeavesNoRecord)
{
    Fault::onlyPathsContaining("idempotency");
    Fault::failNth(Fault::Open, 1, EACCES);
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("key-aaaa-0001", "h"));
    Fault::reset();
    LONGS_EQUAL(0, countFiles(dir));
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h"));
}

TEST(Idempotency, FailedWriteLeavesNoRecord)
{
    Fault::failNth(Fault::Write, 1, EIO);
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("key-aaaa-0001", "h"));
    Fault::reset();
    LONGS_EQUAL(0, countFiles(dir));
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h"));
}

TEST(Idempotency, FailedFsyncBeforeTheEffectLeavesNoRecord)
{
    /* the pending record must be on disk before the effect: if it cannot
       be made durable, nothing may proceed and nothing may be left behind */
    Fault::failNth(Fault::Fsync, 1, EIO);
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("key-aaaa-0001", "h"));
    Fault::reset();
    LONGS_EQUAL(0, countFiles(dir));
}

TEST(Idempotency, FailedStateDirectoryCreationIsAnError)
{
    Fault::onlyPathsContaining("state");
    Fault::failAll(Fault::Mkdir, EACCES);
    LONGS_EQUAL(MCP_IDEM_ERROR, begin("key-aaaa-0001", "h"));
}

TEST(Idempotency, FailedCommitKeepsThePendingRecord)
{
    /* the effect may have happened: the record must stay pending so a
       retry cannot repeat it */
    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h"));

    Fault::onlyPathsContaining("idempotency");
    Fault::failNth(Fault::Open, 1, EACCES);
    LONGS_EQUAL(-1, mcp_idem_commit("mail_send", "key-aaaa-0001", "h", "{}"));
    Fault::reset();
    LONGS_EQUAL(MCP_IDEM_PENDING, begin("key-aaaa-0001", "h"));

    Fault::failNth(Fault::Fsync, 1, EIO);
    LONGS_EQUAL(-1, mcp_idem_commit("mail_send", "key-aaaa-0001", "h", "{}"));
    Fault::reset();
    LONGS_EQUAL(MCP_IDEM_PENDING, begin("key-aaaa-0001", "h"));
    LONGS_EQUAL(1, countFiles(dir));         /* no temporary file left */

    Fault::onlyPathsContaining("idempotency");
    Fault::failNth(Fault::Rename, 1, EXDEV);
    LONGS_EQUAL(-1, mcp_idem_commit("mail_send", "key-aaaa-0001", "h", "{}"));
    Fault::reset();
    LONGS_EQUAL(MCP_IDEM_PENDING, begin("key-aaaa-0001", "h"));
    LONGS_EQUAL(1, countFiles(dir));

    LONGS_EQUAL(0, mcp_idem_commit("mail_send", "key-aaaa-0001", "h", "{}"));
    LONGS_EQUAL(MCP_IDEM_DONE, check("key-aaaa-0001", "h"));
}

TEST(Idempotency, JsonAllocationFailuresAreErrorsNotCrashes)
{
    Fault::installJsonHooks();

    /* learn how many allocations a begin makes, then fail each in turn */
    Fault::failJsonAllocNth(0);
    LONGS_EQUAL(MCP_IDEM_NEW, begin("probe-0000", "h"));
    int allocs = Fault::jsonAllocs();

    CHECK(allocs > 0);
    mcp_idem_abort("probe-0000");

    for (int n = 1; n <= allocs; n++) {
        Fault::failJsonAllocNth(n);
        int rc = begin("key-aaaa-0001", "h");

        Fault::failJsonAllocNth(0);
        CHECK(rc == MCP_IDEM_ERROR || rc == MCP_IDEM_NEW);
        if (rc == MCP_IDEM_ERROR) {
            /* a failed begin must not leave a half-made record */
            CHECK(!fileExists(dir + "/key-aaaa-0001"));
        }

        mcp_idem_abort("key-aaaa-0001");
    }

    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h"));
    for (int n = 1; n <= 8; n++) {
        Fault::failJsonAllocNth(n);
        int rc = mcp_idem_commit("mail_send", "key-aaaa-0001", "h", "{\"r\":1}");

        Fault::failJsonAllocNth(0);
        if (rc == -1) {
            /* still pending, never half done */
            LONGS_EQUAL(MCP_IDEM_PENDING, check("key-aaaa-0001", "h"));
        } else {
            LONGS_EQUAL(MCP_IDEM_DONE, check("key-aaaa-0001", "h"));
            break;
        }
    }
}

TEST(Idempotency, MallocFailuresAreErrorsNotCrashes)
{
    for (int n = 1; n <= 12; n++) {
        Fault::failAllocNth(n);
        char *res = NULL;
        int rc = mcp_idem_begin("mail_send", "key-aaaa-0001", "h", &res);

        Fault::failAllocNth(0);
        if (res != NULL) {
            free(res);
        }

        CHECK(rc == MCP_IDEM_ERROR || rc == MCP_IDEM_NEW
              || rc == MCP_IDEM_PENDING || rc == MCP_IDEM_DONE);
        mcp_idem_abort("key-aaaa-0001");
    }

    LONGS_EQUAL(MCP_IDEM_NEW, begin("key-aaaa-0001", "h"));
    for (int n = 1; n <= 12; n++) {
        Fault::failAllocNth(n);
        char **names = NULL;
        int count = 0;
        int rc = mcp_idem_pending(&names, &count);

        Fault::failAllocNth(0);
        CHECK(rc == 0 || rc == -1);
        for (int i = 0; i < count; i++) {
            free(names[i]);
        }

        free(names);
    }
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
