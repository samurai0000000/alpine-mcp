/*
 * PolicyConfigTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */


#include <cerrno>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "FaultInjection.hxx"
#include "TestSupport.hxx"

extern "C" {
#include "mcpjson.h"
#include "mcpconf.h"
}

/* CppUTest after the standard headers: its new macros break them otherwise */
#include "CppUTest/TestHarness.h"
#include "CppUTest/TestHarness_c.h"

/* free() of memory the add-on's C code allocated with the tracked malloc */
#include "CppUTest/MemoryLeakDetectorMallocMacros.h"


/*
 * The policy file, $HOME/.config/alpine-mcp/config.json.  Everything that
 * is wrong with it must leave MCP off and the lock's meaning intact:
 * missing, unreadable, too open, not a regular file, malformed, wrongly
 * typed.  Only a well-formed, owner-only file turns anything on.
 */

TEST_GROUP(PolicyConfig)
{
    TempDir *tmp;
    std::string confDir;
    std::string confFile;

    void setup()
    {
        tmp = new TempDir();
        confDir = tmp->child(".config/alpine-mcp");
        confFile = confDir + "/config.json";
        Fault::reset();
    }

    void teardown()
    {
        Fault::reset();
        Fault::removeJsonHooks();
        Fault::failAllocNth(0);
        mcp_config_load(NULL);          /* back to defaults, frees the lists */
        delete tmp;
    }

    void put(const std::string &json, mode_t mode = 0600)
    {
        CHECK(makeDir(tmp->child(".config"), 0700));
        CHECK(makeDir(confDir, 0700));
        CHECK(writeFile(confFile, json, mode));
    }

    void load()
    {
        mcp_config_load((char *) tmp->path().c_str());
    }

    std::string lastError()
    {
        char buf[300];

        mcp_config_last_error(buf, sizeof(buf));
        return buf;
    }

    void expectOffAndRefused()
    {
        LONGS_EQUAL(0, mcp_enabled());
        LONGS_EQUAL(0, mcp_aimon_enabled());
        LONGS_EQUAL(0, mcp_allow_count());
        LONGS_EQUAL(0, mcp_recipient_allowed((char *) "a@example.org"));
    }
};

TEST(PolicyConfig, MissingFileMeansDefaultsAndNoError)
{
    load();
    expectOffAndRefused();
    LONGS_EQUAL(0, mcp_lock_engaged());
    STRCMP_EQUAL("", lastError().c_str());
}

TEST(PolicyConfig, NoHomeDirectoryMeansDefaults)
{
    mcp_config_load(NULL);
    expectOffAndRefused();
    mcp_config_load((char *) "");
    expectOffAndRefused();
    CHECK(lastError().find("no home") != std::string::npos);
}

TEST(PolicyConfig, WellFormedFileTurnsThingsOn)
{
    put("{\"mcp\":{\"enabled\":true},\"operator_lock\":true,"
        "\"allowed_recipients\":[\"Me@Example.org\",\"*@example.net\"],"
        "\"aimon\":{\"enabled\":true,\"host\":\"10.1.2.3\",\"port\":4000}}");
    load();
    LONGS_EQUAL(1, mcp_enabled());
    LONGS_EQUAL(1, mcp_lock_engaged());
    LONGS_EQUAL(2, mcp_allow_count());
    LONGS_EQUAL(1, mcp_aimon_enabled());

    char host[64];
    int port = 0;

    LONGS_EQUAL(0, mcp_aimon_target(host, sizeof(host), &port));
    STRCMP_EQUAL("10.1.2.3", host);
    LONGS_EQUAL(4000, port);
    STRCMP_EQUAL(confFile.c_str(), mcp_config_path());
}

TEST(PolicyConfig, DefaultsForEveryKeyThatIsAbsent)
{
    put("{}");
    load();
    expectOffAndRefused();
    LONGS_EQUAL(0, mcp_lock_engaged());

    char host[64];
    int port = 0;

    LONGS_EQUAL(0, mcp_aimon_target(host, sizeof(host), &port));
    STRCMP_EQUAL("127.0.0.1", host);
    LONGS_EQUAL(3885, port);
}

TEST(PolicyConfig, AimonStaysOffWhenMcpIsOff)
{
    put("{\"mcp\":{\"enabled\":false},\"aimon\":{\"enabled\":true}}");
    load();
    LONGS_EQUAL(0, mcp_enabled());
    LONGS_EQUAL(0, mcp_aimon_enabled());
}

TEST(PolicyConfig, FileOthersCanReadIsRejected)
{
    static const mode_t modes[] = { 0644, 0640, 0604, 0660, 0666, 0602, 0601 };

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        put("{\"mcp\":{\"enabled\":true},\"allowed_recipients\":[\"a@example.org\"]}",
            modes[i]);
        load();
        expectOffAndRefused();
        CHECK(lastError().find("must be 600") != std::string::npos);
    }
}

TEST(PolicyConfig, OwnerOnlyModesAreAccepted)
{
    static const mode_t modes[] = { 0600, 0400, 0200 | 0400 };

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        put("{\"mcp\":{\"enabled\":true}}", modes[i]);
        load();
        LONGS_EQUAL(1, mcp_enabled());
        chmod(confFile.c_str(), 0600);
    }
}

TEST(PolicyConfig, DirectoryInPlaceOfTheFileIsRejected)
{
    CHECK(makeDir(tmp->child(".config"), 0700));
    CHECK(makeDir(confDir, 0700));
    CHECK(makeDir(confFile, 0700));
    load();
    expectOffAndRefused();
    CHECK(lastError().find("not a regular file") != std::string::npos);
}

TEST(PolicyConfig, SymlinkInPlaceOfTheFileIsRejected)
{
    CHECK(makeDir(tmp->child(".config"), 0700));
    CHECK(makeDir(confDir, 0700));
    CHECK(writeFile(tmp->child("real.json"), "{\"mcp\":{\"enabled\":true}}", 0600));
    CHECK(symlink(tmp->child("real.json").c_str(), confFile.c_str()) == 0);
    load();
    expectOffAndRefused();
    CHECK(!lastError().empty());
}

TEST(PolicyConfig, FileSizeBoundaries)
{
    put("");
    load();
    expectOffAndRefused();
    CHECK(lastError().find("size") != std::string::npos);

    /* exactly 1 MiB of valid JSON (padded with spaces) is read ... */
    std::string ok = "{\"mcp\":{\"enabled\":true}}";

    ok.resize(1024 * 1024, ' ');
    put(ok);
    load();
    LONGS_EQUAL(1, mcp_enabled());

    /* ... one byte more is refused */
    put(ok + " ");
    load();
    expectOffAndRefused();
    CHECK(lastError().find("size") != std::string::npos);
}

TEST(PolicyConfig, MalformedJsonIsRejected)
{
    static const char *bad[] = {
        "not json", "{", "{\"mcp\":", "{} {}", "{} x", "[]", "null", "42",
        "\"string\"", "{\"mcp\":{\"enabled\":true}} trailing",
        "{\"mcp\":{\"enabled\":true},}", "\xff\xfe{}", NULL
    };

    for (int i = 0; bad[i]; i++) {
        put(bad[i]);
        load();
        expectOffAndRefused();
        CHECK_TEXT(!lastError().empty(), bad[i]);
    }
}

TEST(PolicyConfig, WrongTypesFailClosedAndKeepNothing)
{
    static const char *bad[] = {
        "{\"mcp\":true}",
        "{\"mcp\":{\"enabled\":\"yes\"}}",
        "{\"mcp\":{\"enabled\":1}}",
        "{\"mcp\":{\"enabled\":null}}",
        "{\"operator_lock\":\"true\"}",
        "{\"operator_lock\":0}",
        "{\"allowed_recipients\":\"a@example.org\"}",
        "{\"allowed_recipients\":{}}",
        "{\"aimon\":false}",
        "{\"aimon\":{\"enabled\":\"x\"}}",
        "{\"aimon\":{\"host\":5}}",
        "{\"aimon\":{\"host\":\"\"}}",
        "{\"aimon\":{\"host\":\"has space\"}}",
        "{\"aimon\":{\"port\":\"3885\"}}",
        "{\"aimon\":{\"port\":0}}",
        "{\"aimon\":{\"port\":65536}}",
        "{\"aimon\":{\"port\":1.5}}",
        "{\"aimon\":{\"port\":-1}}",
        NULL
    };

    for (int i = 0; bad[i]; i++) {
        /* with an enabling key in the same file: a wrong type elsewhere
           must not leave it half on */
        std::string text = bad[i];

        if (strncmp(bad[i], "{\"mcp\"", 6) != 0) {
            text = std::string("{\"mcp\":{\"enabled\":true},") + (bad[i] + 1);
        }

        put(text);
        load();
        expectOffAndRefused();
        CHECK_TEXT(!lastError().empty(), bad[i]);
    }
}

TEST(PolicyConfig, AimonPortAndHostBoundaries)
{
    char host[300];
    int port = 0;

    put("{\"aimon\":{\"port\":1}}");
    load();
    LONGS_EQUAL(0, mcp_aimon_target(host, sizeof(host), &port));
    LONGS_EQUAL(1, port);

    put("{\"aimon\":{\"port\":65535}}");
    load();
    mcp_aimon_target(host, sizeof(host), &port);
    LONGS_EQUAL(65535, port);

    put(std::string("{\"aimon\":{\"host\":\"") + std::string(255, 'h') + "\"}}");
    load();
    mcp_aimon_target(host, sizeof(host), &port);
    LONGS_EQUAL(255, strlen(host));

    put(std::string("{\"aimon\":{\"host\":\"") + std::string(256, 'h') + "\"}}");
    load();
    mcp_aimon_target(host, sizeof(host), &port);
    STRCMP_EQUAL("127.0.0.1", host);            /* rejected: the default stays */

    /* a too-small caller buffer is reported */
    char small[4];

    LONGS_EQUAL(-1, mcp_aimon_target(small, sizeof(small), &port));
}

TEST(PolicyConfig, UnknownKeysAreIgnored)
{
    put("{\"mcp\":{\"enabled\":true,\"future\":1},\"other\":[1,2],"
        "\"allowed_recipients\":[]}");
    load();
    LONGS_EQUAL(1, mcp_enabled());
}

TEST(PolicyConfig, BadAllowListEntriesAreSkippedNotTrusted)
{
    put("{\"mcp\":{\"enabled\":true},\"allowed_recipients\":["
        "\"good@example.org\",\"\",\"nodomain\",\"a@b@c\",42,null,"
        "\"two words@example.org\",\"  Spaced@Example.ORG  \"]}");
    load();
    LONGS_EQUAL(1, mcp_enabled());
    LONGS_EQUAL(2, mcp_allow_count());
    CHECK(lastError().find("ignored") != std::string::npos);
    LONGS_EQUAL(1, mcp_recipient_allowed((char *) "good@example.org"));
    LONGS_EQUAL(1, mcp_recipient_allowed((char *) "spaced@example.org"));
    LONGS_EQUAL(0, mcp_recipient_allowed((char *) "nodomain"));
}

TEST(PolicyConfig, AllowListSizeBoundary)
{
    std::string list = "[";

    for (int i = 0; i < 1025; i++) {
        list += (i ? "," : "") + std::string("\"u") + std::to_string(i)
                + "@example.org\"";
    }

    list += "]";
    put("{\"mcp\":{\"enabled\":true},\"allowed_recipients\":" + list + "}");
    load();
    LONGS_EQUAL(1024, mcp_allow_count());
    LONGS_EQUAL(1, mcp_recipient_allowed((char *) "u0@example.org"));
    LONGS_EQUAL(1, mcp_recipient_allowed((char *) "u1023@example.org"));
    LONGS_EQUAL(0, mcp_recipient_allowed((char *) "u1024@example.org"));
}

TEST(PolicyConfig, RecipientMatchingRules)
{
    put("{\"allowed_recipients\":[\"me@example.org\",\"*@example.net\","
        "\"*@sub.example.com\"]}");
    load();

    CHECK(mcp_recipient_allowed((char *) "me@example.org"));
    CHECK(mcp_recipient_allowed((char *) "ME@EXAMPLE.ORG"));
    CHECK(mcp_recipient_allowed((char *) "  me@example.org "));
    CHECK(!mcp_recipient_allowed((char *) "you@example.org"));
    CHECK(!mcp_recipient_allowed((char *) "me@example.org.evil.net"));
    CHECK(!mcp_recipient_allowed((char *) "xme@example.org"));
    CHECK(mcp_recipient_allowed((char *) "anyone@example.net"));
    CHECK(mcp_recipient_allowed((char *) "a.b+c@example.net"));
    CHECK(!mcp_recipient_allowed((char *) "anyone@sub.example.net"));
    CHECK(mcp_recipient_allowed((char *) "x@sub.example.com"));
    CHECK(!mcp_recipient_allowed((char *) "x@example.com"));

    /* things that are not a plain address never match */
    CHECK(!mcp_recipient_allowed((char *) "*@example.net"));
    CHECK(!mcp_recipient_allowed((char *) "Me <me@example.org>"));
    CHECK(!mcp_recipient_allowed((char *) "me@example.org, you@example.org"));
    CHECK(!mcp_recipient_allowed((char *) "me@@example.org"));
    CHECK(!mcp_recipient_allowed((char *) "@example.org"));
    CHECK(!mcp_recipient_allowed((char *) "me@"));
    CHECK(!mcp_recipient_allowed((char *) ""));
    CHECK(!mcp_recipient_allowed(NULL));
}

TEST(PolicyConfig, EmptyAllowListAllowsNothing)
{
    put("{\"allowed_recipients\":[]}");
    load();
    CHECK(!mcp_recipient_allowed((char *) "me@example.org"));
    CHECK(!mcp_recipient_allowed((char *) "*@example.org"));
}

TEST(PolicyConfig, AddressEntryValidity)
{
    static const char *good[] = {
        "a@b.c", "a@b", "me@example.org", "*@example.org", "a.b@c.d",
        "a+tag@c.d", "a_b-c@c.d", "A@B.C", "  x@y.z  ", "a@x-y.z",
        "a@" "1.2", NULL
    };
    static const char *bad[] = {
        "", " ", "@", "a@", "@b.c", "a", "a b@c.d", "a@b c", "a@@b.c",
        "a@b@c.d", ".a@b.c", "a.@b.c", "a..b@c.d", "a@-b.c", "a@b-.c",
        "a@b..c", "a@b.c.", "a@.b.c", "**@b.c", "*a@b.c", "a*@b.c",
        "*@", "*@*", "a@*.c", "<a@b.c>", "a@b.c,d@e.f", "a;b@c.d", "a@b/c",
        "a@b_c.d", "a\"b@c.d", "a(b@c.d", NULL
    };

    for (int i = 0; good[i]; i++) {
        CHECK_TEXT(mcp_allow_entry_valid((char *) good[i]), good[i]);
    }

    for (int i = 0; bad[i]; i++) {
        CHECK_TEXT(!mcp_allow_entry_valid((char *) bad[i]), bad[i]);
    }

    /* local part 64 characters is the limit; label 63 */
    CHECK(mcp_allow_entry_valid((char *) (std::string(64, 'a') + "@b.c").c_str()));
    CHECK(!mcp_allow_entry_valid((char *) (std::string(65, 'a') + "@b.c").c_str()));
    CHECK(mcp_allow_entry_valid((char *) ("a@" + std::string(63, 'b') + ".c").c_str()));
    CHECK(!mcp_allow_entry_valid((char *) ("a@" + std::string(64, 'b') + ".c").c_str()));

    /* whole entry over 320 characters */
    std::string host;

    for (int i = 0; i < 6; i++) {
        host += std::string(60, 'h') + ".";
    }

    CHECK(!mcp_allow_entry_valid((char *) ("a@" + host + "org").c_str()));
}

TEST(PolicyConfig, SetAllowListNormalizesDeduplicatesAndRejects)
{
    char err[200];
    char *good[] = { (char *) "A@Example.org", (char *) "a@example.org",
                     (char *) "*@example.net", NULL };

    put("{}");
    load();
    LONGS_EQUAL(0, mcp_set_allow_list(good, err, sizeof(err)));
    LONGS_EQUAL(2, mcp_allow_count());
    STRCMP_EQUAL("", err);

    char **copy = mcp_allow_list_copy();

    CHECK(copy != NULL);
    STRCMP_EQUAL("a@example.org", copy[0]);
    STRCMP_EQUAL("*@example.net", copy[1]);
    CHECK(copy[2] == NULL);
    mcp_free_list(&copy);
    CHECK(copy == NULL);

    char *bad[] = { (char *) "ok@example.org", (char *) "not valid", NULL };

    LONGS_EQUAL(-1, mcp_set_allow_list(bad, err, sizeof(err)));
    CHECK(strstr(err, "not valid") != NULL);
    LONGS_EQUAL(2, mcp_allow_count());              /* unchanged by a failure */

    LONGS_EQUAL(0, mcp_set_allow_list(NULL, err, sizeof(err)));
    LONGS_EQUAL(0, mcp_allow_count());
}

TEST(PolicyConfig, SaveWritesAnOwnerOnlyFileThatLoadsBack)
{
    put("{\"mcp\":{\"enabled\":true},\"operator_lock\":false,"
        "\"allowed_recipients\":[\"a@example.org\"],"
        "\"aimon\":{\"enabled\":true,\"host\":\"h.example\",\"port\":1234}}");
    load();
    mcp_set_lock(1);
    LONGS_EQUAL(0, mcp_config_save());
    LONGS_EQUAL(0600, fileMode(confFile));
    LONGS_EQUAL(0700, fileMode(confDir));
    LONGS_EQUAL(1, countFiles(confDir));            /* no temporary file left */

    load();
    LONGS_EQUAL(1, mcp_enabled());
    LONGS_EQUAL(1, mcp_lock_engaged());
    LONGS_EQUAL(1, mcp_allow_count());
    LONGS_EQUAL(1, mcp_aimon_enabled());

    char host[64];
    int port = 0;

    mcp_aimon_target(host, sizeof(host), &port);
    STRCMP_EQUAL("h.example", host);
    LONGS_EQUAL(1234, port);
}

TEST(PolicyConfig, SaveCreatesTheDirectoryWhenMissing)
{
    mcp_config_load((char *) tmp->path().c_str());
    LONGS_EQUAL(0, mcp_config_save());
    LONGS_EQUAL(0700, fileMode(confDir));
    LONGS_EQUAL(0600, fileMode(confFile));
}

TEST(PolicyConfig, SaveRefusesADirectoryThatIsNotOurs)
{
    CHECK(makeDir(tmp->child(".config"), 0700));
    CHECK(writeFile(confDir, "a file where the directory should be", 0600));
    load();
    LONGS_EQUAL(-1, mcp_config_save());
    CHECK(!lastError().empty());
}

TEST(PolicyConfig, SaveWithoutALoadedPathFails)
{
    mcp_config_load(NULL);
    LONGS_EQUAL(-1, mcp_config_save());
}

TEST(PolicyConfig, FailedSaveLeavesTheOldFileIntact)
{
    const std::string original = "{\"mcp\":{\"enabled\":true},\"operator_lock\":false}";

    put(original);
    load();
    mcp_set_lock(1);

    Fault::onlyPathsContaining("config.json");
    Fault::failNth(Fault::Open, 1, EACCES);
    LONGS_EQUAL(-1, mcp_config_save());
    Fault::reset();
    STRCMP_EQUAL(original.c_str(), readFile(confFile).c_str());

    Fault::failNth(Fault::Write, 1, ENOSPC);
    LONGS_EQUAL(-1, mcp_config_save());
    Fault::reset();
    STRCMP_EQUAL(original.c_str(), readFile(confFile).c_str());
    LONGS_EQUAL(1, countFiles(confDir));            /* the temp file is gone */

    Fault::failNth(Fault::Fsync, 1, EIO);
    LONGS_EQUAL(-1, mcp_config_save());
    Fault::reset();
    STRCMP_EQUAL(original.c_str(), readFile(confFile).c_str());
    LONGS_EQUAL(1, countFiles(confDir));

    Fault::onlyPathsContaining("config.json");
    Fault::failNth(Fault::Rename, 1, EXDEV);
    LONGS_EQUAL(-1, mcp_config_save());
    Fault::reset();
    STRCMP_EQUAL(original.c_str(), readFile(confFile).c_str());
    LONGS_EQUAL(1, countFiles(confDir));

    LONGS_EQUAL(0, mcp_config_save());              /* and a retry works */
    load();
    LONGS_EQUAL(1, mcp_lock_engaged());
}

TEST(PolicyConfig, AllocationFailuresWhileLoadingFailClosed)
{
    put("{\"mcp\":{\"enabled\":true},\"operator_lock\":true,"
        "\"allowed_recipients\":[\"a@example.org\",\"b@example.org\"],"
        "\"aimon\":{\"enabled\":true,\"host\":\"h.example\"}}");

    for (int n = 1; n <= 24; n++) {
        Fault::failAllocNth(n);
        load();
        Fault::failAllocNth(0);
        if (mcp_enabled()) {
            /*
             * If it came up, the lock the file asked for is on, and the
             * allow list is never longer than the file's: an entry lost to
             * a failed allocation means one more question for the human,
             * never a recipient nobody allowed.
             */
            LONGS_EQUAL(1, mcp_lock_engaged());
            CHECK(mcp_allow_count() <= 2);
            LONGS_EQUAL(0, mcp_recipient_allowed((char *) "c@example.org"));
        } else {
            expectOffAndRefused();
        }
    }

    Fault::installJsonHooks();
    Fault::failJsonAllocNth(0);
    load();
    int allocs = Fault::jsonAllocs();

    for (int n = 1; n <= allocs; n++) {
        Fault::failJsonAllocNth(n);
        load();
        Fault::failJsonAllocNth(0);
        if (!mcp_enabled()) {
            expectOffAndRefused();
        }
    }
}

TEST(PolicyConfig, LockIsHeldInMemoryEvenWhenTheSaveFails)
{
    put("{\"mcp\":{\"enabled\":true}}");
    load();
    mcp_set_lock(1);
    Fault::failAll(Fault::Open, EACCES);
    LONGS_EQUAL(-1, mcp_config_save());
    Fault::reset();
    LONGS_EQUAL(1, mcp_lock_engaged());             /* the kill switch holds */
    mcp_set_lock(0);
    LONGS_EQUAL(0, mcp_lock_engaged());
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
