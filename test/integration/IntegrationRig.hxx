/*
 * IntegrationRig.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_INTEGRATION_RIG_HXX
#define ALPINE_MCP_TEST_INTEGRATION_RIG_HXX

#include <cstdio>
#include <string>

#include "AlpineHarness.hxx"
#include "JsonLineClient.hxx"
#include "SmtpSink.hxx"
#include "../unit/ServerTestUtil.hxx"

/*
 * What every integration test starts from: a real Alpine on a pty with a
 * home of its own, a loopback SMTP sink as its mail host, and one client
 * on its MCP socket.
 */
class Rig
{
public:
    Rig() : _next(1) {}

    /* sink, Alpine and a connected client; false if any step failed */
    bool up()
    {
        if (!smtp.start()) {
            return false;
        }

        a.setSmtpPort(smtp.port());

        if (!a.start()) {
            return false;
        }

        return c.connectTo(a.socketPath());
    }

    std::string inbox() const { return a.home() + "/mail/INBOX"; }

    /* the whole JSON-RPC reply to a tools/call; the caller wraps it in a
       Json.  NULL on timeout. */
    cJSON *call(const std::string &tool, const std::string &argsJson,
                int timeoutMs = 20000)
    {
        return call(c, tool, argsJson, timeoutMs);
    }

    cJSON *call(JsonLineClient &on, const std::string &tool,
                const std::string &argsJson, int timeoutMs = 20000)
    {
        on.sendLine(JsonLineClient::toolCall(std::to_string(_next++), tool,
                                             argsJson));
        return on.readJson(timeoutMs);
    }

    /* the parsed tool result of a successful call, else NULL */
    cJSON *callOk(const std::string &tool, const std::string &argsJson,
                  int timeoutMs = 20000)
    {
        Json r(call(tool, argsJson, timeoutMs));

        if (r.ok() && r.at("error") == NULL) {
            return r.toolResult();
        }

        /* say why, so a failing test is not a silent NULL */
        fprintf(stderr, "[rig] %s %s: %s\n", tool.c_str(), argsJson.c_str(),
                r.ok() ? r.errorMessage().c_str() : "no reply");
        return NULL;
    }

    /* {"folder":"<mailbox>" followed by the extra members } */
    static std::string folderArgs(const std::string &mailbox,
                                  const std::string &more = "")
    {
        return "{\"folder\":\"" + mailbox + "\"" + more + "}";
    }

    /* the mailbox string, from mail_list_folders, that ends with this
       text (e.g. "/saved-messages"); empty if there is none */
    std::string mailbox(const std::string &suffix)
    {
        Json l(callOk("mail_list_folders", "{}"));
        cJSON *f = NULL;

        cJSON_ArrayForEach(f, l.at("folders")) {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(f, "mailbox");

            if (cJSON_IsString(m)) {
                std::string box = m->valuestring;

                if (box.size() >= suffix.size()
                    && box.compare(box.size() - suffix.size(), suffix.size(),
                                   suffix) == 0) {
                    return box;
                }
            }
        }

        return "";
    }

    /* the UIDVALIDITY of a folder, 0 if it cannot be had */
    long long uidvalidity(const std::string &mailbox)
    {
        Json st(callOk("mail_folder_status", folderArgs(mailbox)));
        cJSON *v = st.at("uidvalidity");

        return cJSON_IsNumber(v) ? (long long) v->valuedouble : 0;
    }

    /* uid of the message with this subject in the folder, 0 if none */
    long uidOfSubject(const std::string &mailbox, const std::string &subject)
    {
        Json l(callOk("mail_list_messages", folderArgs(mailbox, ",\"limit\":100")));
        cJSON *m = l.at("messages");
        cJSON *row = NULL;

        cJSON_ArrayForEach(row, m) {
            cJSON *s = cJSON_GetObjectItemCaseSensitive(row, "subject");
            cJSON *u = cJSON_GetObjectItemCaseSensitive(row, "uid");

            if (cJSON_IsString(s) && subject == s->valuestring
                && cJSON_IsNumber(u)) {
                return (long) u->valuedouble;
            }
        }

        return 0;
    }

    AlpineHarness a;
    SmtpSink smtp;
    JsonLineClient c;

private:
    int _next;
};

#endif /* ALPINE_MCP_TEST_INTEGRATION_RIG_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
