/*
 * SendIntegrationTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "IntegrationRig.hxx"

#include "CppUTest/TestHarness.h"

/*
 * mail_send against a real Alpine and a loopback SMTP host: what goes on
 * the wire, who is let through without asking, and the question the human
 * is asked on the Alpine screen for anyone else.
 */

namespace
{
    const char *askText = "Let the agent send mail to";

    std::string
    sendArgs(const std::string &to, const std::string &key,
             const std::string &more = "")
    {
        return "{\"to\":\"" + to + "\",\"idempotency_key\":\"" + key + "\""
            + more + "}";
    }

    std::string
    lower(std::string s)
    {
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] >= 'A' && s[i] <= 'Z') {
                s[i] = (char) (s[i] - 'A' + 'a');
            }
        }

        return s;
    }

    /* the value of a header; the name is matched without regard to case,
       as in RFC 5322 */
    std::string
    headerValue(const std::string &data, const std::string &name)
    {
        size_t end = data.find("\r\n\r\n");
        std::string head = data.substr(0, end == std::string::npos ? data.size() : end);
        std::string want = "\r\n" + lower(name) + ":";
        size_t at = lower("\r\n" + head).find(want);

        if (at == std::string::npos) {
            return "";
        }

        size_t from = at + want.size() - 2;         /* in head */
        size_t eol = head.find("\r\n", from);
        std::string v = head.substr(from, eol == std::string::npos ? std::string::npos
                                    : eol - from);

        while (!v.empty() && v[0] == ' ') {
            v.erase(0, 1);
        }

        return v;
    }
}

TEST_GROUP(SendIntegration)
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

    /* the question on screen, an answer key, and the reply to the call */
    cJSON *askAndAnswer(const std::string &args, const char *key)
    {
        size_t m = rig->a.mark();

        rig->c.sendLine(JsonLineClient::toolCall("9", "mail_send", args));
        CHECK(rig->a.waitForText(askText, 8000, m));
        rig->a.sendKeys(key);
        return rig->c.readJson(15000);
    }
};

TEST(SendIntegration, AnAllowedRecipientIsSentAtOnceWithoutAQuestion)
{
    CHECK(rig->up());
    size_t m = rig->a.mark();
    Json r(rig->callOk("mail_send", sendArgs("ok@example.org", "send-key-0001",
        ",\"subject\":\"hello\",\"body\":\"the body\"")));

    CHECK(r.ok());
    CHECK(cJSON_IsTrue(r.at("sent")));
    LONGS_EQUAL(1, (int) r.at("recipients")->valuedouble);
    CHECK(rig->smtp.waitForCount(1, 5000));
    CHECK(rig->a.staysAbsent(askText, 300, m));

    std::vector<SinkMail> mails = rig->smtp.mails();

    LONGS_EQUAL(1, (int) mails.size());
    LONGS_EQUAL(1, (int) mails[0].rcpts.size());
    STRCMP_EQUAL("ok@example.org", mails[0].rcpts[0].c_str());
    STRCMP_EQUAL("hello", headerValue(mails[0].data, "Subject").c_str());
    CHECK(mails[0].data.find("\r\n\r\nthe body\r\n") != std::string::npos);
}

TEST(SendIntegration, TheEnvelopeSenderIsTheFromAddress)
{
    CHECK(rig->up());
    Json r(rig->callOk("mail_send", sendArgs("ok@example.org", "send-key-0002")));

    CHECK(r.ok());
    CHECK(rig->smtp.waitForCount(1, 5000));
    SinkMail m = rig->smtp.mails()[0];
    std::string from = headerValue(m.data, "From");

    CHECK(!m.from.empty());                         /* never MAIL FROM:<> */
    CHECK_TEXT(from.find("<" + m.from + ">") != std::string::npos, from.c_str());
    CHECK(m.from.find("@example.test") != std::string::npos);
}

TEST(SendIntegration, BccIsDeliveredButNeverWrittenInTheHeaders)
{
    rig->a.useConfig("{\"mcp\":{\"enabled\":true},\"operator_lock\":false,"
                     "\"allowed_recipients\":[\"*@example.org\"]}");
    CHECK(rig->up());
    Json r(rig->callOk("mail_send", sendArgs("to@example.org", "send-key-0003",
        ",\"cc\":\"cc@example.org\",\"bcc\":\"hidden@example.org\","
        "\"subject\":\"s\",\"body\":\"b\"")));

    CHECK(r.ok());
    LONGS_EQUAL(3, (int) r.at("recipients")->valuedouble);
    CHECK(rig->smtp.waitForCount(1, 5000));
    SinkMail m = rig->smtp.mails()[0];

    LONGS_EQUAL(3, (int) m.rcpts.size());
    CHECK(m.data.find("hidden@example.org") == std::string::npos);
    CHECK(m.data.find("Bcc:") == std::string::npos);
    CHECK(m.data.find("bcc:") == std::string::npos);
    STRCMP_EQUAL("to@example.org", headerValue(m.data, "To").c_str());
    STRCMP_EQUAL("cc@example.org", headerValue(m.data, "Cc").c_str());
}

TEST(SendIntegration, NonAsciiTextIsLabelledUtf8EvenInTheCLocale)
{
    CHECK(rig->up());
    Json r(rig->callOk("mail_send", sendArgs("ok@example.org", "send-key-0004",
        ",\"subject\":\"caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac\","
        "\"body\":\"caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac body\"")));

    CHECK(r.ok());
    CHECK(rig->smtp.waitForCount(1, 5000));
    SinkMail m = rig->smtp.mails()[0];

    CHECK(headerValue(m.data, "Content-Type").find("charset=UTF-8") != std::string::npos);
    CHECK(headerValue(m.data, "Subject").find("=?UTF-8?") == 0);
    CHECK(m.data.find("caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac body") != std::string::npos);
    STRCMP_EQUAL("1.0", headerValue(m.data, "MIME-Version").c_str());
}

TEST(SendIntegration, TheHeadersAlpineAddsAreThere)
{
    CHECK(rig->up());
    CHECK(rig->callOk("mail_send", sendArgs("ok@example.org", "send-key-0005")) != NULL);
    CHECK(rig->smtp.waitForCount(1, 5000));
    SinkMail m = rig->smtp.mails()[0];

    CHECK(!headerValue(m.data, "Date").empty());
    CHECK(headerValue(m.data, "Message-ID").find("@example.test>") != std::string::npos);
    CHECK(headerValue(m.data, "From").find("Test User") == 0);
}

TEST(SendIntegration, BodiesThatLookLikeSmtpOrMboxSyntaxArriveIntact)
{
    const char *bodies[] = {
        "before\\n.\\nafter",
        ".\\n",
        "..two dots\\n.\\n..",
        "From someone Mon Jan  1 00:00:00 2026\\nnot a new message",
        "a\\r\\n.\\r\\nb",
        "",
    };
    const char *want[] = {
        "before\r\n.\r\nafter",
        ".\r\n",
        "..two dots\r\n.\r\n..",
        "From someone Mon Jan  1 00:00:00 2026\r\nnot a new message",
        "a\r\n.\r\nb",
        "",
    };

    CHECK(rig->up());

    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        std::string key = "send-body-" + std::to_string(i) + "0000";
        Json r(rig->callOk("mail_send", sendArgs("ok@example.org", key,
            std::string(",\"body\":\"") + bodies[i] + "\"")));

        CHECK_TEXT(r.ok(), bodies[i]);
        CHECK(rig->smtp.waitForCount(i + 1, 5000));
        std::string data = rig->smtp.mails()[i].data;
        size_t at = data.find("\r\n\r\n");

        CHECK(at != std::string::npos);
        std::string body = data.substr(at + 4);

        /* the sink has dot-unstuffed; what is left is the body and its
           final line end */
        CHECK_TEXT(body.compare(0, strlen(want[i]), want[i]) == 0, bodies[i]);
    }

    LONGS_EQUAL(6, (int) rig->smtp.count());
}

TEST(SendIntegration, ALargeBodyArrivesWhole)
{
    CHECK(rig->up());
    std::string body;

    for (int i = 0; i < 4000; i++) {
        body += "line " + std::to_string(i) + " of a long body\\n";
    }

    Json r(rig->callOk("mail_send", sendArgs("ok@example.org", "send-key-0006",
        ",\"body\":\"" + body + "\""), 60000));

    CHECK(r.ok());
    CHECK(rig->smtp.waitForCount(1, 10000));
    std::string data = rig->smtp.mails()[0].data;

    CHECK(data.find("line 0 of a long body\r\n") != std::string::npos);
    CHECK(data.find("line 3999 of a long body\r\n") != std::string::npos);
}

TEST(SendIntegration, ARetryWithTheSameKeySendsNothingMoreAndAChangeIsRefused)
{
    CHECK(rig->up());
    const std::string args = sendArgs("ok@example.org", "send-key-0007",
                                      ",\"subject\":\"once\",\"body\":\"b\"");
    Json first(rig->callOk("mail_send", args));
    Json again(rig->callOk("mail_send", args));

    CHECK(first.ok());
    CHECK(again.ok());
    CHECK(!cJSON_IsTrue(first.at("replayed")));
    CHECK(cJSON_IsTrue(again.at("replayed")));

    Json changed(rig->call("mail_send", sendArgs("ok@example.org", "send-key-0007",
                                                 ",\"subject\":\"twice\",\"body\":\"b\"")));

    LONGS_EQUAL(-32006, changed.errorCode());
    CHECK(rig->smtp.waitForCount(1, 5000));
    usleep(500 * 1000);                             /* time for a wrong second mail */
    LONGS_EQUAL(1, (int) rig->smtp.count());
}

TEST(SendIntegration, AReplayStillWorksAfterAlpineRestarts)
{
    CHECK(rig->up());
    const std::string args = sendArgs("ok@example.org", "send-key-0008",
                                      ",\"subject\":\"once\",\"body\":\"b\"");

    CHECK(rig->callOk("mail_send", args) != NULL);
    CHECK(rig->smtp.waitForCount(1, 5000));
    rig->c.closeNow();
    CHECK(rig->a.stop());
    CHECK(!fileExists(rig->a.socketPath()));

    /* the same home, a new process: the record on disk is what remembers */
    CHECK(rig->a.start());
    CHECK(rig->c.connectTo(rig->a.socketPath()));
    Json again(rig->callOk("mail_send", args));

    CHECK(again.ok());
    CHECK(cJSON_IsTrue(again.at("replayed")));
    usleep(500 * 1000);
    LONGS_EQUAL(1, (int) rig->smtp.count());
}

TEST(SendIntegration, ARecipientTheHostRefusesIsAnErrorAndTheKeyCanBeRetried)
{
    CHECK(rig->up());
    const std::string args = sendArgs("ok@example.org", "send-key-0009",
                                      ",\"subject\":\"s\",\"body\":\"b\"");

    rig->smtp.rejectRecipients("550 5.1.1 no such user");
    Json bad(rig->call("mail_send", args));

    CHECK(bad.ok());
    CHECK(bad.errorCode() != 0);
    LONGS_EQUAL(0, (int) rig->smtp.count());

    rig->smtp.rejectRecipients("");
    Json retry(rig->callOk("mail_send", args));

    CHECK(retry.ok());
    CHECK(!cJSON_IsTrue(retry.at("replayed")));
    CHECK(rig->smtp.waitForCount(1, 5000));
    LONGS_EQUAL(1, (int) rig->smtp.count());
}

TEST(SendIntegration, AMailHostThatIsDownIsAnErrorAndAlpineCarriesOn)
{
    CHECK(rig->up());
    rig->smtp.stop();
    Json bad(rig->call("mail_send", sendArgs("ok@example.org", "send-key-0010"), 60000));

    CHECK(bad.ok());
    CHECK(bad.errorCode() != 0);
    Json l(rig->callOk("mail_list_folders", "{}"));

    CHECK(l.ok());
}

TEST(SendIntegration, MatchingTheAllowListIgnoresCaseAndDisplayNames)
{
    const char *ok[] = { "OK@EXAMPLE.ORG", "Ok@Example.Org",
                         "Someone <ok@example.org>" };

    CHECK(rig->up());

    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        size_t m = rig->a.mark();
        std::string key = "send-case-" + std::to_string(i) + "0000";
        Json r(rig->callOk("mail_send", sendArgs(ok[i], key)));

        CHECK_TEXT(r.ok(), ok[i]);
        CHECK_TEXT(rig->a.staysAbsent(askText, 200, m), ok[i]);
    }

    CHECK(rig->smtp.waitForCount(3, 5000));
}

TEST(SendIntegration, AWildcardDomainEntryLetsTheWholeDomainThrough)
{
    rig->a.useConfig("{\"mcp\":{\"enabled\":true},\"operator_lock\":false,"
                     "\"allowed_recipients\":[\"*@example.org\"]}");
    CHECK(rig->up());
    size_t m = rig->a.mark();
    Json r(rig->callOk("mail_send", sendArgs("anyone@example.org", "send-key-0011")));

    CHECK(r.ok());
    CHECK(rig->a.staysAbsent(askText, 200, m));

    /* a lookalike domain is not the domain */
    Json off(askAndAnswer(sendArgs("anyone@notexample.org", "send-key-0012"), "n"));

    LONGS_EQUAL(-32007, off.errorCode());
    Json sub(askAndAnswer(sendArgs("anyone@mail.example.org", "send-key-0013"), "n"));

    LONGS_EQUAL(-32007, sub.errorCode());
    LONGS_EQUAL(1, (int) rig->smtp.count());
}

TEST(SendIntegration, AnOperatorLockRefusesEvenAnAllowedRecipient)
{
    rig->a.useConfig("{\"mcp\":{\"enabled\":true},\"operator_lock\":true,"
                     "\"allowed_recipients\":[\"ok@example.org\"]}");
    CHECK(rig->up());
    Json r(rig->call("mail_send", sendArgs("ok@example.org", "send-key-0014")));

    LONGS_EQUAL(-32001, r.errorCode());
    LONGS_EQUAL(0, (int) rig->smtp.count());
}

TEST(SendIntegration, SendRefusesBadArgumentsAndHeaderInjection)
{
    const char *bad[] = {
        "{\"to\":\"\",\"idempotency_key\":\"send-bad-0001\"}",
        "{\"to\":\"ok@example.org\"}",
        "{\"idempotency_key\":\"send-bad-0002\"}",
        "{\"to\":\"ok@example.org\",\"idempotency_key\":\"x\"}",
        "{\"to\":\"ok@example.org\",\"idempotency_key\":\"send-bad-0003\","
        "\"subject\":\"hi\\r\\nBcc: evil@example.net\"}",
        "{\"to\":\"ok@example.org\\nBcc: evil@example.net\","
        "\"idempotency_key\":\"send-bad-0004\"}",
        "{\"to\":\"ok@example.org\",\"idempotency_key\":\"send-bad-0005\","
        "\"attachments\":[\"/etc/passwd\"]}",
        "{\"to\":\"ok@example.org\",\"idempotency_key\":\"send-bad-0006\","
        "\"from\":\"nobody-role\"}",
        "{\"to\":[\"ok@example.org\"],\"idempotency_key\":\"send-bad-0007\"}",
    };

    CHECK(rig->up());

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        Json r(rig->call("mail_send", bad[i]));

        CHECK_TEXT(r.ok(), bad[i]);
        CHECK_TEXT(r.errorCode() != 0, bad[i]);
    }

    LONGS_EQUAL(0, (int) rig->smtp.count());
}

TEST(SendIntegration, TheQuestionAppearsWithoutAnyKeyBeingPressed)
{
    CHECK(rig->up());
    size_t m = rig->a.mark();

    rig->c.sendLine(JsonLineClient::toolCall("9", "mail_send",
                    sendArgs("other@example.net", "send-ask-0001")));
    CHECK(rig->a.waitForText(askText, 8000, m));
    CHECK(rig->a.waitForText("other@example.net", 1000, m));
    CHECK(rig->a.waitForText("not on the allow list", 1000, m));
    LONGS_EQUAL(0, (int) rig->smtp.count());
    rig->a.sendKeys("n");
    Json r(rig->c.readJson(15000));

    LONGS_EQUAL(-32007, r.errorCode());
}

TEST(SendIntegration, NoSendsNothingAndTheAnswerIsNotRemembered)
{
    CHECK(rig->up());
    Json no(askAndAnswer(sendArgs("other@example.net", "send-ask-0002"), "n"));

    LONGS_EQUAL(-32007, no.errorCode());
    LONGS_EQUAL(0, (int) rig->smtp.count());

    /* the same recipient again is asked again */
    Json again(askAndAnswer(sendArgs("other@example.net", "send-ask-0003"), "n"));

    LONGS_EQUAL(-32007, again.errorCode());
    LONGS_EQUAL(0, (int) rig->smtp.count());
}

TEST(SendIntegration, YesSendsOnceAndTheAddressIsNotAddedToTheList)
{
    CHECK(rig->up());
    Json yes(askAndAnswer(sendArgs("other@example.net", "send-ask-0004",
                                     ",\"subject\":\"approved\",\"body\":\"b\""), "y"));

    CHECK(yes.ok());
    CHECK(yes.at("error") == NULL);
    CHECK(rig->smtp.waitForCount(1, 5000));
    STRCMP_EQUAL("other@example.net", rig->smtp.mails()[0].rcpts[0].c_str());

    Json next(askAndAnswer(sendArgs("other@example.net", "send-ask-0005"), "n"));

    LONGS_EQUAL(-32007, next.errorCode());
    LONGS_EQUAL(1, (int) rig->smtp.count());
    CHECK(readFile(rig->a.home() + "/.config/alpine-mcp/config.json")
          .find("other@example.net") == std::string::npos);
}

TEST(SendIntegration, EnterAndControlCAndNAreNo)
{
    const char *keys[] = { "\r", "\x03", "n", "N" };

    CHECK(rig->up());

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        std::string key = "send-ask-k" + std::to_string(i) + "0000";
        Json r(askAndAnswer(sendArgs("other@example.net", key), keys[i]));

        CHECK_TEXT(r.ok(), keys[i]);
        LONGS_EQUAL_TEXT(-32007, r.errorCode(), keys[i]);
    }

    LONGS_EQUAL(0, (int) rig->smtp.count());
}

TEST(SendIntegration, OtherKeysDoNotAnswerTheQuestion)
{
    CHECK(rig->up());
    size_t m = rig->a.mark();

    rig->c.sendLine(JsonLineClient::toolCall("9", "mail_send",
                    sendArgs("other@example.net", "send-ask-0007")));
    CHECK(rig->a.waitForText(askText, 8000, m));

    /* none of these is y, n, Enter or ^C: the question stays */
    rig->a.sendKeys("xz q 1?");
    std::string line;

    CHECK(!rig->c.readLine(line, 1500));
    LONGS_EQUAL(0, (int) rig->smtp.count());
    rig->a.sendKeys("y");
    Json r(rig->c.readJson(15000));

    CHECK(r.ok());
    CHECK(r.at("error") == NULL);
    CHECK(rig->smtp.waitForCount(1, 5000));
}

TEST(SendIntegration, EveryWayOfNamingAnOutsiderIsAsked)
{
    const char *off[] = {
        "{\"to\":\"other@example.net\",\"idempotency_key\":\"send-off-a0000\"}",
        "{\"to\":\"ok@example.org.evil.net\",\"idempotency_key\":\"send-off-b0000\"}",
        "{\"to\":\"xok@example.org\",\"idempotency_key\":\"send-off-c0000\"}",
        "{\"to\":\"\\\"ok@example.org\\\" <evil@example.net>\","
        "\"idempotency_key\":\"send-off-d0000\"}",
        "{\"to\":\"ok@example.org, evil@example.net\",\"idempotency_key\":\"send-off-e0000\"}",
        "{\"to\":\"ok@example.org\",\"cc\":\"evil@example.net\","
        "\"idempotency_key\":\"send-off-f0000\"}",
        "{\"to\":\"ok@example.org\",\"bcc\":\"evil@example.net\","
        "\"idempotency_key\":\"send-off-g0000\"}",
    };

    CHECK(rig->up());

    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
        Json r(askAndAnswer(off[i], "n"));

        CHECK_TEXT(r.ok(), off[i]);
        LONGS_EQUAL_TEXT(-32007, r.errorCode(), off[i]);
    }

    LONGS_EQUAL(0, (int) rig->smtp.count());
}

TEST(SendIntegration, ManyOutsidersAreSummarisedInOneQuestion)
{
    CHECK(rig->up());
    size_t m = rig->a.mark();

    rig->c.sendLine(JsonLineClient::toolCall("9", "mail_send",
        "{\"to\":\"a@example.net, b@example.net, c@example.net, d@example.net\","
        "\"idempotency_key\":\"send-many-0001\"}"));
    CHECK(rig->a.waitForText(askText, 8000, m));
    CHECK(rig->a.waitForText("and 2 more", 1000, m));
    rig->a.sendKeys("n");
    Json r(rig->c.readJson(15000));

    LONGS_EQUAL(-32007, r.errorCode());
}

TEST(SendIntegration, OtherClientsAreServedWhileAQuestionIsOnScreen)
{
    CHECK(rig->up());
    size_t m = rig->a.mark();

    rig->c.sendLine(JsonLineClient::toolCall("9", "mail_send",
                    sendArgs("other@example.net", "send-ask-0006")));
    CHECK(rig->a.waitForText(askText, 8000, m));

    JsonLineClient other;

    CHECK(other.connectTo(rig->a.socketPath()));
    Json l(rig->call(other, "mail_folder_status", Rig::folderArgs(rig->inbox()), 8000));

    /* the folder is not listed in this client's view yet, so the answer
       may be a refusal; what matters is that it is an answer */
    CHECK(l.ok());
    rig->a.sendKeys("n");
    Json r(rig->c.readJson(15000));

    LONGS_EQUAL(-32007, r.errorCode());
}

TEST(SendIntegration, TwoQuestionsAreAskedOneAfterTheOther)
{
    CHECK(rig->up());
    JsonLineClient second;

    CHECK(second.connectTo(rig->a.socketPath()));
    size_t m = rig->a.mark();

    rig->c.sendLine(JsonLineClient::toolCall("1", "mail_send",
                    sendArgs("first@example.net", "send-two-0001")));
    CHECK(rig->a.waitForText("first@example.net", 8000, m));
    second.sendLine(JsonLineClient::toolCall("2", "mail_send",
                    sendArgs("second@example.net", "send-two-0002")));
    rig->a.sendKeys("n");
    size_t m2 = rig->a.mark();

    CHECK(rig->a.waitForText("second@example.net", 8000, m));
    (void) m2;
    rig->a.sendKeys("y");

    Json r1(rig->c.readJson(15000));
    Json r2(second.readJson(15000));

    LONGS_EQUAL(-32007, r1.errorCode());
    CHECK(r2.ok());
    CHECK(r2.at("error") == NULL);
    CHECK(rig->smtp.waitForCount(1, 5000));
    STRCMP_EQUAL("second@example.net", rig->smtp.mails()[0].rcpts[0].c_str());
}

TEST(SendIntegration, AClientThatLeavesCancelsTheCallEvenIfTheHumanSaysYes)
{
    CHECK(rig->up());
    size_t m = rig->a.mark();

    rig->c.sendLine(JsonLineClient::toolCall("9", "mail_send",
                    sendArgs("other@example.net", "send-gone-0001")));
    CHECK(rig->a.waitForText(askText, 8000, m));
    rig->c.closeNow();

    /* the question stays up until the human answers it; the answer no
       longer has a caller, so a yes sends nothing */
    usleep(500 * 1000);
    rig->a.sendKeys("y");

    /* then Alpine takes ordinary commands again: the quit prompt only
       appears when the question is gone */
    size_t after = rig->a.mark();

    rig->a.sendKeys("q");
    CHECK(rig->a.waitForText("Really quit", 8000, after));
    rig->a.sendKeys("n");
    usleep(500 * 1000);
    LONGS_EQUAL(0, (int) rig->smtp.count());

    /* the call never ran, so the same key can be used by a new call */
    JsonLineClient fresh;

    CHECK(fresh.connectTo(rig->a.socketPath()));
    Json r(rig->call(fresh, "mail_send", sendArgs("ok@example.org", "send-gone-0001"), 15000));

    CHECK(r.ok());
    CHECK(r.at("error") == NULL);
    Json result(r.toolResult());

    CHECK(result.ok());
    CHECK(!cJSON_IsTrue(result.at("replayed")));
    CHECK(rig->smtp.waitForCount(1, 5000));
    STRCMP_EQUAL("ok@example.org", rig->smtp.mails()[0].rcpts[0].c_str());
}

TEST(SendIntegration, AClientThatLeavesBeforeTheQuestionIsReachedCostsNothing)
{
    CHECK(rig->up());

    for (int i = 0; i < 3; i++) {
        JsonLineClient gone;

        CHECK(gone.connectTo(rig->a.socketPath()));
        gone.sendLine(JsonLineClient::toolCall("9", "mail_send",
            sendArgs("other@example.net", "send-gone-b" + std::to_string(i) + "000")));
        gone.closeNow();
    }

    /* calls whose client has left are dropped without a question; one
       that was already on screen when its client left waits for a key,
       like any question, and its answer has no caller to go to */
    if (rig->a.waitForText(askText, 2000, 0)) {
        rig->a.sendKeys("n");
    }

    usleep(800 * 1000);
    LONGS_EQUAL(0, (int) rig->smtp.count());
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
