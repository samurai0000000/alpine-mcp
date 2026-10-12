/*
 * WriteIntegrationTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <dirent.h>
#include <string>
#include <sys/stat.h>

#include "IntegrationRig.hxx"

#include "CppUTest/TestHarness.h"

/*
 * The mutation tools against a real Alpine and real mbox files: flags,
 * copy and drafts.  Nothing here may delete, move or send.
 */

namespace
{
    int
    countOccurrences(const std::string &hay, const std::string &needle)
    {
        int n = 0;

        for (size_t at = hay.find(needle); at != std::string::npos;
             at = hay.find(needle, at + needle.size())) {
            n++;
        }

        return n;
    }
}

TEST_GROUP(WriteIntegration)
{
    ScopedThreadSafeTracking threadSafe;
    Rig *rig;
    long long v;
    std::string saved, postponed;

    void setup()
    {
        rig = new Rig();
        CHECK(rig->up());
        Json folders(rig->callOk("mail_list_folders", "{}"));

        CHECK(folders.ok());
        saved = rig->mailbox("/saved-messages");
        postponed = rig->mailbox("/postponed-msgs");
        CHECK(!saved.empty());
        CHECK(!postponed.empty());

        /* nothing may point outside the test's own home */
        CHECK(saved.compare(0, rig->a.home().size(), rig->a.home()) == 0);
        CHECK(postponed.compare(0, rig->a.home().size(), rig->a.home()) == 0);
        v = rig->uidvalidity(rig->inbox());
        CHECK(v > 0);
    }

    void teardown()
    {
        rig->c.closeNow();
        CHECK(rig->a.stop());
        delete rig;
    }

    std::string at(long uid)
    {
        return ",\"uid\":" + std::to_string(uid) + ",\"uidvalidity\":"
            + std::to_string(v);
    }

    /* a flag of one message as mail_list_messages shows it */
    bool flagOf(long uid, const char *key)
    {
        Json l(rig->callOk("mail_list_messages",
                           Rig::folderArgs(rig->inbox(), ",\"limit\":100")));
        cJSON *row = NULL;

        cJSON_ArrayForEach(row, l.at("messages")) {
            if ((long) cJSON_GetObjectItemCaseSensitive(row, "uid")->valuedouble == uid) {
                return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, key));
            }
        }

        FAIL("uid not listed");
        return false;
    }

    std::string setFlag(long uid, const std::string &flag, bool on)
    {
        Json r(rig->call("mail_set_flag", Rig::folderArgs(rig->inbox(),
            at(uid) + ",\"flag\":\"" + flag + "\",\"set\":" + (on ? "true" : "false"))));

        return r.ok() && r.at("error") == NULL ? "" : r.errorMessage();
    }
};

TEST(WriteIntegration, SetsAndClearsEachAllowedFlag)
{
    struct { const char *flag; const char *key; } flags[] = {
        { "\\\\Seen", "seen" },
        { "\\\\Flagged", "flagged" },
        { "\\\\Answered", "answered" },
        { "\\\\Deleted", "deleted" },
    };

    for (size_t i = 0; i < 4; i++) {
        CHECK(!flagOf(2, flags[i].key));
        STRCMP_EQUAL("", setFlag(2, flags[i].flag, true).c_str());
        CHECK_TEXT(flagOf(2, flags[i].key), flags[i].key);
        CHECK(!flagOf(1, flags[i].key));                  /* only that one */
        STRCMP_EQUAL("", setFlag(2, flags[i].flag, false).c_str());
        CHECK_TEXT(!flagOf(2, flags[i].key), flags[i].key);
    }
}

TEST(WriteIntegration, ASetFlagReachesTheMailboxFile)
{
    STRCMP_EQUAL("", setFlag(3, "\\\\Flagged", true).c_str());
    rig->c.closeNow();
    CHECK(rig->a.stop());

    /* Alpine wrote its flags back on the way out */
    std::string text = readFile(rig->a.mailPath("INBOX"));

    CHECK(text.find("X-Status: F") != std::string::npos);
    LONGS_EQUAL(4, countOccurrences(text, "\nFrom: ") + 1 - 1);
}

TEST(WriteIntegration, InvalidDeletedSpellingsAreRefusedAndNothingIsMarked)
{
    const char *spellings[] = { "\\\\deleted", "\\\\DELETED",
                                "Deleted", "$Deleted", "\\\\Seen \\\\Deleted" };

    for (size_t i = 0; i < sizeof(spellings) / sizeof(spellings[0]); i++) {
        Json r(rig->call("mail_set_flag", Rig::folderArgs(rig->inbox(),
            at(2) + ",\"flag\":\"" + spellings[i] + "\",\"set\":true")));

        CHECK_TEXT(r.ok(), spellings[i]);
        LONGS_EQUAL_TEXT(-32602, r.errorCode(), spellings[i]);
    }

    CHECK(!flagOf(2, "deleted"));
    CHECK(!flagOf(2, "seen"));
}

TEST(WriteIntegration, SetFlagRefusesAStaleUidvalidityAndWrongTypes)
{
    Json stale(rig->call("mail_set_flag", Rig::folderArgs(rig->inbox(),
        ",\"uid\":2,\"uidvalidity\":" + std::to_string(v + 5)
        + ",\"flag\":\"\\\\Seen\",\"set\":true")));

    LONGS_EQUAL(-32602, stale.errorCode());
    Json str(rig->call("mail_set_flag", Rig::folderArgs(rig->inbox(),
        at(2) + ",\"flag\":\"\\\\Seen\",\"set\":\"true\"")));

    LONGS_EQUAL(-32602, str.errorCode());
    Json zero(rig->call("mail_set_flag", Rig::folderArgs(rig->inbox(),
        ",\"uid\":0,\"uidvalidity\":" + std::to_string(v)
        + ",\"flag\":\"\\\\Seen\",\"set\":true")));

    LONGS_EQUAL(-32602, zero.errorCode());
    CHECK(!flagOf(2, "seen"));
}

TEST(WriteIntegration, CopiesToAnotherFolderAndLeavesTheOriginal)
{
    Json r(rig->callOk("mail_copy", Rig::folderArgs(rig->inbox(),
        at(2) + ",\"destination\":\"" + saved + "\"")));

    CHECK(r.ok());
    CHECK(cJSON_IsTrue(r.at("copied")));
    CHECK(!cJSON_IsTrue(r.at("source_removed")));
    CHECK(!flagOf(2, "deleted"));

    Json listing(rig->callOk("mail_list_messages",
                             Rig::folderArgs(saved)));

    CHECK(listing.ok());
    LONGS_EQUAL(1, cJSON_GetArraySize(listing.at("messages")));
    STRCMP_EQUAL("with attachment",
                 cJSON_GetObjectItemCaseSensitive(
                     cJSON_GetArrayItem(listing.at("messages"), 0), "subject")->valuestring);

    /* the original is where it was, with its attachment */
    Json st(rig->callOk("mail_folder_status", Rig::folderArgs(rig->inbox())));

    LONGS_EQUAL(4, (int) st.at("messages")->valuedouble);

    /* the copy keeps the attachment */
    long long sv = rig->uidvalidity(saved);
    Json m(rig->callOk("mail_get_message", Rig::folderArgs(saved,
        ",\"uid\":1,\"uidvalidity\":" + std::to_string(sv))));

    CHECK(m.ok());
    LONGS_EQUAL(1, cJSON_GetArraySize(m.at("attachments")));
}

TEST(WriteIntegration, CopiesWithMoveMarksSourceDeletedWithoutExpunging)
{
    Json r(rig->callOk("mail_copy", Rig::folderArgs(rig->inbox(),
        at(2) + ",\"destination\":\"" + saved + "\",\"move\":true")));

    CHECK(r.ok());
    CHECK(cJSON_IsTrue(r.at("copied")));
    CHECK(cJSON_IsTrue(r.at("source_removed")));

    /* the destination has the message */
    Json listing(rig->callOk("mail_list_messages",
                             Rig::folderArgs(saved)));

    CHECK(listing.ok());
    LONGS_EQUAL(1, cJSON_GetArraySize(listing.at("messages")));

    /* the copy in the destination is clean (not \Deleted) */
    cJSON *destMsg = cJSON_GetArrayItem(listing.at("messages"), 0);
    CHECK(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(destMsg, "deleted")));

    /* the original is marked \Deleted, but NOT expunged */
    CHECK(flagOf(2, "deleted"));
    Json st(rig->callOk("mail_folder_status", Rig::folderArgs(rig->inbox())));
    LONGS_EQUAL(4, (int) st.at("messages")->valuedouble);

    /* moving an already \Deleted message still succeeds and keeps source_removed true */
    Json r2(rig->callOk("mail_copy", Rig::folderArgs(rig->inbox(),
        at(2) + ",\"destination\":\"" + saved + "\",\"move\":true")));
    CHECK(r2.ok());
    CHECK(cJSON_IsTrue(r2.at("copied")));
    CHECK(cJSON_IsTrue(r2.at("source_removed")));
}

TEST(WriteIntegration, MoveToSameFolderDuplicatesAndMarksOriginalDeleted)
{
    /* move a message to its own folder: it creates a new copy and marks original \Deleted */
    Json r(rig->callOk("mail_copy", Rig::folderArgs(rig->inbox(),
        at(1) + ",\"destination\":\"" + rig->inbox() + "\",\"move\":true")));
    CHECK(r.ok());
    CHECK(cJSON_IsTrue(r.at("copied")));
    CHECK(cJSON_IsTrue(r.at("source_removed")));

    /* the original message (UID 1) is now \Deleted */
    CHECK(flagOf(1, "deleted"));

    /* total message count increased by 1 (4 -> 5) */
    Json st(rig->callOk("mail_folder_status", Rig::folderArgs(rig->inbox())));
    LONGS_EQUAL(5, (int) st.at("messages")->valuedouble);
}

TEST(WriteIntegration, CopyingTwiceMakesTwoCopies)
{
    for (int i = 0; i < 2; i++) {
        Json r(rig->callOk("mail_copy", Rig::folderArgs(rig->inbox(),
            at(1) + ",\"destination\":\"" + saved + "\"")));

        CHECK(r.ok());
    }

    Json st(rig->callOk("mail_folder_status", Rig::folderArgs(saved)));

    LONGS_EQUAL(2, (int) st.at("messages")->valuedouble);
}

TEST(WriteIntegration, CopyRefusesDestinationsThatWereNotListed)
{
    const char *dests[] = { "mail/nonexistent", "/etc/passwd", "INBOX",
                            "{evil.example.net}INBOX", "mail/../../x", "" };

    for (size_t i = 0; i < sizeof(dests) / sizeof(dests[0]); i++) {
        Json r(rig->call("mail_copy", Rig::folderArgs(rig->inbox(),
            at(1) + ",\"destination\":\"" + dests[i] + "\"")));

        CHECK_TEXT(r.ok(), dests[i]);
        LONGS_EQUAL_TEXT(-32602, r.errorCode(), dests[i]);
    }

    CHECK(!fileExists(rig->a.mailPath("nonexistent")));
}

TEST(WriteIntegration, CopyRefusesAStaleUidvalidity)
{
    Json r(rig->call("mail_copy", Rig::folderArgs(rig->inbox(),
        ",\"uid\":1,\"uidvalidity\":" + std::to_string(v + 1)
        + ",\"destination\":\"" + saved + "\"")));

    LONGS_EQUAL(-32602, r.errorCode());
    Json st(rig->callOk("mail_folder_status", Rig::folderArgs(saved)));

    LONGS_EQUAL(0, (int) st.at("messages")->valuedouble);
}

TEST(WriteIntegration, CreatesADraftInThePostponedFolderAndSendsNothing)
{
    Json r(rig->callOk("mail_create_draft",
        "{\"to\":\"someone@example.net\",\"cc\":\"copy@example.net\","
        "\"subject\":\"draft subject\",\"body\":\"draft body\","
        "\"idempotency_key\":\"draft-key-0001\"}"));

    CHECK(r.ok());
    CHECK(cJSON_IsTrue(r.at("drafted")));
    LONGS_EQUAL(0, (int) rig->smtp.count());

    Json l(rig->callOk("mail_list_messages", Rig::folderArgs(postponed)));

    CHECK(l.ok());
    LONGS_EQUAL(1, cJSON_GetArraySize(l.at("messages")));
    long long pv = rig->uidvalidity(postponed);
    Json m(rig->callOk("mail_get_message", Rig::folderArgs(postponed,
        ",\"uid\":1,\"uidvalidity\":" + std::to_string(pv))));

    CHECK(m.ok());
    STRCMP_EQUAL("draft subject",
                 cJSON_GetObjectItemCaseSensitive(m.at("headers"), "subject")->valuestring);
    CHECK(m.str("body").find("draft body") != std::string::npos);
}

TEST(WriteIntegration, ADraftRetryWithTheSameKeyMakesNoSecondDraft)
{
    const std::string args =
        "{\"to\":\"someone@example.net\",\"subject\":\"once\",\"body\":\"b\","
        "\"idempotency_key\":\"draft-key-0002\"}";
    Json first(rig->callOk("mail_create_draft", args));
    Json again(rig->callOk("mail_create_draft", args));

    CHECK(first.ok());
    CHECK(again.ok());
    CHECK(!cJSON_IsTrue(first.at("replayed")));
    CHECK(cJSON_IsTrue(again.at("replayed")));
    Json st(rig->callOk("mail_folder_status", Rig::folderArgs(postponed)));

    LONGS_EQUAL(1, (int) st.at("messages")->valuedouble);

    Json other(rig->call("mail_create_draft",
        "{\"to\":\"someone@example.net\",\"subject\":\"changed\",\"body\":\"b\","
        "\"idempotency_key\":\"draft-key-0002\"}"));

    LONGS_EQUAL(-32006, other.errorCode());
    Json st2(rig->callOk("mail_folder_status", Rig::folderArgs(postponed)));

    LONGS_EQUAL(1, (int) st2.at("messages")->valuedouble);
}

TEST(WriteIntegration, DraftRefusesBadKeysAndBadHeaders)
{
    const char *bad[] = {
        "{\"to\":\"a@example.net\",\"idempotency_key\":\"short\"}",
        "{\"to\":\"a@example.net\",\"idempotency_key\":\"../../etc/passwd\"}",
        "{\"to\":\"a@example.net\",\"idempotency_key\":\"has space in it\"}",
        "{\"to\":\"a@example.net\"}",
        "{\"to\":\"a@example.net\",\"subject\":\"x\\r\\nBcc: evil@example.net\","
        "\"idempotency_key\":\"draft-key-0003\"}",
        "{\"to\":\"a@example.net\\r\\nBcc: evil@example.net\","
        "\"idempotency_key\":\"draft-key-0004\"}",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        Json r(rig->call("mail_create_draft", bad[i]));

        CHECK_TEXT(r.ok(), bad[i]);
        LONGS_EQUAL_TEXT(-32602, r.errorCode(), bad[i]);
    }

    Json st(rig->callOk("mail_folder_status", Rig::folderArgs(postponed)));

    LONGS_EQUAL(0, (int) st.at("messages")->valuedouble);
}

TEST(WriteIntegration, IdempotencyRecordsAreKeptPrivate)
{
    Json r(rig->callOk("mail_create_draft",
        "{\"to\":\"a@example.net\",\"idempotency_key\":\"draft-key-0005\"}"));

    CHECK(r.ok());
    LONGS_EQUAL(0700, fileMode(rig->a.idempotencyDir()));
    DIR *d = opendir(rig->a.idempotencyDir().c_str());
    int files = 0;

    CHECK(d != NULL);

    for (struct dirent *e; (e = readdir(d)) != NULL; ) {
        if (e->d_name[0] != '.') {
            files++;
            LONGS_EQUAL(0600, fileMode(rig->a.idempotencyDir() + "/" + e->d_name));
        }
    }

    closedir(d);
    LONGS_EQUAL(1, files);
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
