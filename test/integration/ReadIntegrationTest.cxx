/*
 * ReadIntegrationTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <string>

#include "IntegrationRig.hxx"

#include "CppUTest/TestHarness.h"

/*
 * The read tools against a real Alpine and real mbox folders: what is
 * listed, what a message looks like in each of the four shapes (plain,
 * with an attachment, UTF-8, HTML only), and that reading changes
 * nothing.
 */

TEST_GROUP(ReadIntegration)
{
    ScopedThreadSafeTracking threadSafe;
    Rig *rig;

    void setup()
    {
        rig = new Rig();
        rig->a.addContact("ann", "Ann Example", "a@example.org");
        rig->a.addContact("bob", "Bob Example", "b@example.org");
        rig->a.addMbox("work",
            "From w@example.org Mon Jan  5 10:00:00 2026\n"
            "From: W <w@example.org>\n"
            "Subject: work item\n"
            "Date: Mon, 05 Jan 2026 10:00:00 +0000\n"
            "Message-ID: <w1@example.org>\n"
            "\n"
            "work body\n"
            "\n");
        CHECK(rig->up());

        /* the other tools accept only folders that this was asked for */
        Json folders(rig->callOk("mail_list_folders", "{}"));

        CHECK(folders.ok());
    }

    void teardown()
    {
        rig->c.closeNow();
        CHECK(rig->a.stop());
        delete rig;
    }

    long long uv() { return rig->uidvalidity(rig->inbox()); }
};

TEST(ReadIntegration, ListsIncomingFoldersAndTheCollection)
{
    Json r(rig->callOk("mail_list_folders", "{}"));

    CHECK(r.ok());
    cJSON *rows = r.at("folders");
    bool inbox = false, work = false, sent = false, postponed = false;
    cJSON *f = NULL;

    cJSON_ArrayForEach(f, rows) {
        std::string box = cJSON_GetObjectItemCaseSensitive(f, "mailbox")->valuestring;
        bool incoming = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "incoming"));

        CHECK(cJSON_GetObjectItemCaseSensitive(f, "name") != NULL);

        if (box == rig->inbox()) {
            inbox = inbox || incoming;
        } else if (box == rig->a.mailPath("work")) {
            work = work || incoming;
        } else if (box == rig->a.mailPath("sent-mail")) {
            sent = sent || !incoming;
        } else if (box == rig->a.mailPath("postponed-msgs")) {
            postponed = postponed || !incoming;
        }
    }

    CHECK(inbox);
    CHECK(work);
    CHECK(sent);
    CHECK(postponed);
    CHECK(cJSON_GetArraySize(r.at("collections")) == 2);
}

TEST(ReadIntegration, FolderStatusCountsTheMessages)
{
    Json r(rig->callOk("mail_folder_status", Rig::folderArgs(rig->inbox())));

    CHECK(r.ok());
    LONGS_EQUAL(4, (int) r.at("messages")->valuedouble);
    LONGS_EQUAL(4, (int) r.at("unseen")->valuedouble);
    CHECK(r.at("uidvalidity")->valuedouble >= 1);
    LONGS_EQUAL(5, (int) r.at("uidnext")->valuedouble);

    Json w(rig->callOk("mail_folder_status", Rig::folderArgs(rig->a.mailPath("work"))));

    CHECK(w.ok());
    LONGS_EQUAL(1, (int) w.at("messages")->valuedouble);
}

TEST(ReadIntegration, ListsNewestFirstAndHonoursTheLimit)
{
    Json r(rig->callOk("mail_list_messages",
                       Rig::folderArgs(rig->inbox(), ",\"limit\":2")));

    CHECK(r.ok());
    LONGS_EQUAL(4, (int) r.at("total")->valuedouble);
    cJSON *rows = r.at("messages");

    LONGS_EQUAL(2, cJSON_GetArraySize(rows));
    LONGS_EQUAL(4, (int) cJSON_GetObjectItemCaseSensitive(
                    cJSON_GetArrayItem(rows, 0), "uid")->valuedouble);
    LONGS_EQUAL(3, (int) cJSON_GetObjectItemCaseSensitive(
                    cJSON_GetArrayItem(rows, 1), "uid")->valuedouble);

    Json all(rig->callOk("mail_list_messages", Rig::folderArgs(rig->inbox())));

    LONGS_EQUAL(4, cJSON_GetArraySize(all.at("messages")));
}

TEST(ReadIntegration, DecodesUtf8NamesAndSubjectsEvenInTheCLocale)
{
    Json r(rig->callOk("mail_list_messages", Rig::folderArgs(rig->inbox())));
    cJSON *row = NULL;
    bool found = false;

    cJSON_ArrayForEach(row, r.at("messages")) {
        cJSON *s = cJSON_GetObjectItemCaseSensitive(row, "subject");

        if (std::string(s->valuestring) == "caf\xc3\xa9 menu") {
            cJSON *from = cJSON_GetObjectItemCaseSensitive(row, "from");

            STRCMP_EQUAL("Caf\xc3\xa9 Owner",
                         cJSON_GetObjectItemCaseSensitive(from, "name")->valuestring);
            found = true;
        }
    }

    CHECK(found);
}

TEST(ReadIntegration, GetsAPlainMessageWithLfLineEnds)
{
    long uid = rig->uidOfSubject(rig->inbox(), "first plain");

    CHECK(uid > 0);
    Json m(rig->callOk("mail_get_message", Rig::folderArgs(rig->inbox(),
        ",\"uid\":" + std::to_string(uid) + ",\"uidvalidity\":" + std::to_string(uv()))));

    CHECK(m.ok());
    STRCMP_EQUAL("Hello one.\nSecond line.\n", m.str("body").c_str());
    STRCMP_EQUAL("text/plain", m.str("content_is").c_str());
    CHECK(m.str("warning").find("untrusted") != std::string::npos);
    LONGS_EQUAL(0, cJSON_GetArraySize(m.at("attachments")));
    cJSON *h = m.at("headers");

    STRCMP_EQUAL("<m1@example.org>",
                 cJSON_GetObjectItemCaseSensitive(h, "message_id")->valuestring);
}

TEST(ReadIntegration, ListsAttachmentsByNameAndSizeAndNeverTheirContent)
{
    long uid = rig->uidOfSubject(rig->inbox(), "with attachment");
    Json m(rig->callOk("mail_get_message", Rig::folderArgs(rig->inbox(),
        ",\"uid\":" + std::to_string(uid) + ",\"uidvalidity\":" + std::to_string(uv()))));

    CHECK(m.ok());
    STRCMP_EQUAL("Body with an attachment.", m.str("body").c_str());
    cJSON *at = m.at("attachments");

    LONGS_EQUAL(1, cJSON_GetArraySize(at));
    cJSON *one = cJSON_GetArrayItem(at, 0);

    STRCMP_EQUAL("report.txt",
                 cJSON_GetObjectItemCaseSensitive(one, "filename")->valuestring);
    LONGS_EQUAL(31, (int) cJSON_GetObjectItemCaseSensitive(one, "size")->valuedouble);
    CHECK(m.str("body").find("attached text") == std::string::npos);
}

TEST(ReadIntegration, GetsUtf8BodyDecodedFromQuotedPrintable)
{
    long uid = rig->uidOfSubject(rig->inbox(), "caf\xc3\xa9 menu");
    Json m(rig->callOk("mail_get_message", Rig::folderArgs(rig->inbox(),
        ",\"uid\":" + std::to_string(uid) + ",\"uidvalidity\":" + std::to_string(uv()))));

    CHECK(m.ok());
    STRCMP_EQUAL("caf\xc3\xa9 \xc3\xbc" "n\xc3\xaf" "code\n", m.str("body").c_str());
}

TEST(ReadIntegration, HtmlOnlyMessageReturnsTheHtmlAndSaysSo)
{
    long uid = rig->uidOfSubject(rig->inbox(), "html only");
    Json m(rig->callOk("mail_get_message", Rig::folderArgs(rig->inbox(),
        ",\"uid\":" + std::to_string(uid) + ",\"uidvalidity\":" + std::to_string(uv()))));

    CHECK(m.ok());
    STRCMP_EQUAL("text/html", m.str("content_is").c_str());
    CHECK(m.str("body").find("Only html here.") != std::string::npos);
}

TEST(ReadIntegration, ReadingMessagesNeverMarksThemSeen)
{
    long long v = uv();

    for (long uid = 1; uid <= 4; uid++) {
        Json m(rig->callOk("mail_get_message", Rig::folderArgs(rig->inbox(),
            ",\"uid\":" + std::to_string(uid) + ",\"uidvalidity\":" + std::to_string(v))));

        CHECK(m.ok());
        CHECK(!cJSON_IsTrue(m.at("seen")));
    }

    Json st(rig->callOk("mail_folder_status", Rig::folderArgs(rig->inbox())));

    LONGS_EQUAL(4, (int) st.at("unseen")->valuedouble);
}

TEST(ReadIntegration, SearchesByEachCriterionAndByTheirCombination)
{
    struct Case { const char *more; int matches; };
    static const Case cases[] = {
        { ",\"from\":\"bob\"", 1 },
        { ",\"from\":\"example.org\"", 4 },
        { ",\"subject\":\"attachment\"", 1 },
        { ",\"text\":\"Hello one\"", 1 },
        { ",\"since\":\"2026-01-07\"", 2 },
        { ",\"before\":\"2026-01-06\"", 1 },
        { ",\"seen\":false", 4 },
        { ",\"seen\":true", 0 },
        { ",\"flagged\":true", 0 },
        { ",\"unanswered\":true", 4 },
        { ",\"from\":\"bob\",\"subject\":\"first\"", 0 },
        { ",\"from\":\"nobody-at-all\"", 0 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Json r(rig->callOk("mail_search", Rig::folderArgs(rig->inbox(), cases[i].more)));

        CHECK_TEXT(r.ok(), cases[i].more);
        LONGS_EQUAL_TEXT(cases[i].matches, (int) r.at("matches")->valuedouble, cases[i].more);
        LONGS_EQUAL_TEXT(cases[i].matches, cJSON_GetArraySize(r.at("messages")), cases[i].more);
    }
}

TEST(ReadIntegration, SearchRefusesRawCriteriaAndBadDates)
{
    Json raw(rig->call("mail_search", Rig::folderArgs(rig->inbox(),
                       ",\"criteria\":\"ALL\"")));

    LONGS_EQUAL(-32602, raw.errorCode());
    Json bad(rig->call("mail_search", Rig::folderArgs(rig->inbox(),
                       ",\"since\":\"yesterday\"")));

    LONGS_EQUAL(-32602, bad.errorCode());
    Json badLimit(rig->call("mail_list_messages", Rig::folderArgs(rig->inbox(),
                            ",\"limit\":101")));

    LONGS_EQUAL(-32602, badLimit.errorCode());
}

TEST(ReadIntegration, RefusesFoldersThatAreNotFromTheListing)
{
    const char *bad[] = { "INBOX", "/etc/passwd", "mail/../../etc/passwd",
                          "mail/nonexistent", "{evil.example.net}INBOX", "" };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        Json r(rig->call("mail_folder_status", Rig::folderArgs(bad[i])));

        CHECK_TEXT(r.ok(), bad[i]);
        LONGS_EQUAL_TEXT(-32602, r.errorCode(), bad[i]);
    }
}

TEST(ReadIntegration, RefusesAStaleUidvalidityAndAnUnknownUid)
{
    long long v = uv();
    Json stale(rig->call("mail_get_message", Rig::folderArgs(rig->inbox(),
        ",\"uid\":1,\"uidvalidity\":" + std::to_string(v + 1))));

    LONGS_EQUAL(-32602, stale.errorCode());
    CHECK(stale.errorMessage().find("uidvalidity") != std::string::npos);

    Json unknown(rig->call("mail_get_message", Rig::folderArgs(rig->inbox(),
        ",\"uid\":999,\"uidvalidity\":" + std::to_string(v))));

    CHECK(unknown.ok());
    CHECK(unknown.errorCode() != 0);
}

TEST(ReadIntegration, ReadsAnotherIncomingFolder)
{
    Json r(rig->callOk("mail_list_messages", Rig::folderArgs(rig->a.mailPath("work"))));

    CHECK(r.ok());
    LONGS_EQUAL(1, cJSON_GetArraySize(r.at("messages")));
    STRCMP_EQUAL("work item",
                 cJSON_GetObjectItemCaseSensitive(
                     cJSON_GetArrayItem(r.at("messages"), 0), "subject")->valuestring);
}

TEST(ReadIntegration, SearchesTheAddressBook)
{
    Json r(rig->callOk("mail_search_addressbook", "{\"query\":\"ann\"}"));

    CHECK(r.ok());
    LONGS_EQUAL(1, cJSON_GetArraySize(r.at("entries")));
    cJSON *e = cJSON_GetArrayItem(r.at("entries"), 0);

    STRCMP_EQUAL("Ann Example",
                 cJSON_GetObjectItemCaseSensitive(e, "fullname")->valuestring);

    Json byAddr(rig->callOk("mail_search_addressbook", "{\"query\":\"b@example\"}"));

    LONGS_EQUAL(1, cJSON_GetArraySize(byAddr.at("entries")));
    Json none(rig->callOk("mail_search_addressbook", "{\"query\":\"zzzz\"}"));

    LONGS_EQUAL(0, cJSON_GetArraySize(none.at("entries")));
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
