/*
 * ToolValidatorTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */


#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "mcpjson.h"
#include "mcp.h"
#include "mcpint.h"
}

/* CppUTest after the standard headers: its new macros break them otherwise */
#include "CppUTest/TestHarness.h"

/*
 * Every tool has a closed argument schema that is checked on the listener
 * thread before anything is queued.  These tests hold the validators to the
 * limits in the plan: ranges, lengths, characters, the dates c-client can
 * hold, \Deleted allowed, unknown fields (a raw "criteria" string above
 * all) refused.
 */

TEST_GROUP(ToolValidator)
{
    /* true if the tool accepts these arguments */
    bool accepts(const char *tool, const std::string &json,
                 std::string *why = NULL)
    {
        MCP_TOOL_S *t = mcp_tool_lookup(tool);

        CHECK_TEXT(t != NULL, tool);

        cJSON *args = NULL;

        if (!json.empty()) {
            args = mcp_json_parse(json.c_str(), json.size());
            CHECK_TEXT(args != NULL, json.c_str());
        }

        char err[300] = "";
        int rc = (*t->validate)(args, err, sizeof(err));

        if (args != NULL) {
            cJSON_Delete(args);
        }

        if (why != NULL) {
            *why = err;
        }

        return rc == 0;
    }

    std::string str(size_t n, char c = 'a')
    {
        return "\"" + std::string(n, c) + "\"";
    }
};

TEST(ToolValidator, TheTableHoldsTheTenToolsWithRunFunctions)
{
    static const char *names[] = {
        "mail_list_folders", "mail_folder_status", "mail_list_messages",
        "mail_search", "mail_get_message", "mail_search_addressbook",
        "mail_copy", "mail_set_flag", "mail_create_draft", "mail_send", NULL
    };

    for (int i = 0; names[i]; i++) {
        MCP_TOOL_S *t = mcp_tool_lookup(names[i]);

        CHECK_TEXT(t != NULL, names[i]);
        CHECK_TEXT(t->run != NULL, names[i]);
        CHECK_TEXT(t->validate != NULL, names[i]);
        CHECK_TEXT(t->description != NULL && strlen(t->description) > 20, names[i]);
        if (strcmp(names[i], "mail_send") == 0) {
            CHECK(t->run_confirmed != NULL);
        } else {
            CHECK_TEXT(t->run_confirmed == NULL, names[i]);
        }
    }
}

TEST(ToolValidator, NoToolForDeletingExpungingOrMoving)
{
    static const char *forbidden[] = {
        "mail_delete", "mail_expunge", "mail_move", "mail_create_folder",
        "mail_delete_folder", "mail_rename_folder", "mail_save_attachment",
        "mail_classify", "mail_summarize", "shell", NULL
    };

    for (int i = 0; forbidden[i]; i++) {
        CHECK_TEXT(mcp_tool_lookup(forbidden[i]) == NULL, forbidden[i]);
    }

    CHECK(mcp_tool_lookup(NULL) == NULL);
    CHECK(mcp_tool_lookup("") == NULL);
    CHECK(mcp_tool_lookup("MAIL_SEND") == NULL);
}

TEST(ToolValidator, ToolsListIsWellFormedWithClosedSchemas)
{
    cJSON *list = mcp_tools_list_json();

    CHECK(list != NULL);
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(list, "tools");

    CHECK(cJSON_IsArray(arr));
    LONGS_EQUAL(10, cJSON_GetArraySize(arr));

    std::set<std::string> seen;
    cJSON *tool;

    cJSON_ArrayForEach(tool, arr) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(tool, "description");
        cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");

        CHECK(cJSON_IsString(name));
        CHECK(cJSON_IsString(desc));
        CHECK(cJSON_IsObject(schema));
        CHECK(seen.insert(name->valuestring).second);      /* no duplicates */
        STRCMP_EQUAL("object",
                     cJSON_GetObjectItemCaseSensitive(schema, "type")->valuestring);
        cJSON *ap = cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties");

        CHECK_TEXT(cJSON_IsFalse(ap), name->valuestring);
    }

    cJSON_Delete(list);
}

TEST(ToolValidator, SchemaAndValidatorAgreeOnProperties)
{
    /*
     * For every property the schema lists, the validator accepts a typical
     * value for it; for every required property, leaving it out is refused;
     * and an extra property the schema does not list is refused.
     */
    cJSON *list = mcp_tools_list_json();
    cJSON *tool;

    cJSON_ArrayForEach(tool, cJSON_GetObjectItemCaseSensitive(list, "tools")) {
        const char *name = cJSON_GetObjectItemCaseSensitive(tool, "name")->valuestring;
        cJSON *schema = cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
        cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
        cJSON *req = cJSON_GetObjectItemCaseSensitive(schema, "required");
        std::vector<std::string> pnames;
        std::vector<std::string> samples;
        cJSON *p;

        cJSON_ArrayForEach(p, props) {
            const char *type = cJSON_GetObjectItemCaseSensitive(p, "type")->valuestring;
            std::string sample;

            if (!strcmp(type, "string")) {
                if (!strcmp(p->string, "flag")) {
                    sample = "\"\\\\Seen\"";
                } else if (!strcmp(p->string, "since") || !strcmp(p->string, "before")) {
                    sample = "\"2026-01-31\"";
                } else if (!strcmp(p->string, "idempotency_key")) {
                    sample = "\"abcdefgh\"";
                } else {
                    sample = "\"x\"";
                }
            } else if (!strcmp(type, "integer")) {
                sample = "5";
            } else if (!strcmp(type, "boolean")) {
                sample = "true";
            } else if (!strcmp(type, "array")) {
                if (!strcmp(p->string, "uids")) {
                    sample = "[1]";
                } else if (!strcmp(p->string, "folders")) {
                    sample = "[\"x\"]";
                } else {
                    sample = "[]";
                }
            } else {
                FAIL(std::string(std::string("unexpected schema type for ") + name).c_str());
            }

            pnames.push_back(p->string);
            samples.push_back(sample);
        }

        std::set<std::string> required;
        cJSON *r;

        if (req != NULL) {
            cJSON_ArrayForEach(r, req) {
                required.insert(r->valuestring);
            }
        }

        std::string full = "{";
        bool first = true;

        for (size_t i = 0; i < pnames.size(); i++) {
            if (pnames[i] == "uids" || pnames[i] == "folders") {
                continue;
            }
            full += (first ? "" : ",") + std::string("\"") + pnames[i] + "\":" + samples[i];
            first = false;
        }

        full += "}";
        CHECK_TEXT(accepts(name, full), name);

        if (std::find(pnames.begin(), pnames.end(), "uids") != pnames.end()) {
            std::string batch = "{";
            bool bfirst = true;
            for (size_t i = 0; i < pnames.size(); i++) {
                if (pnames[i] == "uid") continue;
                batch += (bfirst ? "" : ",") + std::string("\"") + pnames[i] + "\":" + samples[i];
                bfirst = false;
            }
            batch += "}";
            CHECK_TEXT(accepts(name, batch), (std::string(name) + " batch").c_str());
        } else if (std::find(pnames.begin(), pnames.end(), "folders") != pnames.end()) {
            std::string batch = "{";
            bool bfirst = true;
            for (size_t i = 0; i < pnames.size(); i++) {
                if (pnames[i] == "folder") continue;
                batch += (bfirst ? "" : ",") + std::string("\"") + pnames[i] + "\":" + samples[i];
                bfirst = false;
            }
            batch += "}";
            CHECK_TEXT(accepts(name, batch), (std::string(name) + " batch").c_str());
        }

        for (std::set<std::string>::iterator it = required.begin();
             it != required.end(); ++it) {
            std::string without = "{";
            bool wfirst = true;

            for (size_t i = 0; i < pnames.size(); i++) {
                if (pnames[i] == *it || pnames[i] == "uids" || pnames[i] == "folders") {
                    continue;
                }

                without += (wfirst ? "" : ",") + std::string("\"") + pnames[i]
                           + "\":" + samples[i];
                wfirst = false;
            }

            without += "}";
            CHECK_TEXT(!accepts(name, without), (std::string(name) + " without " + *it).c_str());
        }

        std::string extra = full.substr(0, full.size() - 1)
                            + (pnames.empty() ? "" : ",") + "\"zz_unknown\":1}";

        CHECK_TEXT(!accepts(name, extra), name);
    }

    cJSON_Delete(list);
}

TEST(ToolValidator, ListFoldersTakesNoArguments)
{
    CHECK(accepts("mail_list_folders", ""));            /* no arguments at all */
    CHECK(accepts("mail_list_folders", "{}"));
    CHECK(!accepts("mail_list_folders", "{\"x\":1}"));
    CHECK(!accepts("mail_list_folders", "[]"));
    CHECK(!accepts("mail_list_folders", "5"));
    CHECK(!accepts("mail_list_folders", "\"text\""));
}

TEST(ToolValidator, FolderArgumentLengthAndType)
{
    CHECK(accepts("mail_folder_status", "{\"folder\":\"x\"}"));
    CHECK(accepts("mail_folder_status", "{\"folder\":" + str(1024) + "}"));
    CHECK(!accepts("mail_folder_status", "{\"folder\":" + str(1025) + "}"));
    CHECK(!accepts("mail_folder_status", "{\"folder\":\"\"}"));
    CHECK(!accepts("mail_folder_status", "{\"folder\":5}"));
    CHECK(!accepts("mail_folder_status", "{\"folder\":null}"));
    CHECK(!accepts("mail_folder_status", "{\"folder\":[\"x\"]}"));
    CHECK(!accepts("mail_folder_status", "{}"));
    CHECK(!accepts("mail_folder_status", ""));
    CHECK(!accepts("mail_folder_status", "{\"folder\":\"x\",\"limit\":5}"));
}

TEST(ToolValidator, ListMessagesLimitBoundaries)
{
    const std::string f = "\"folder\":\"x\"";

    CHECK(accepts("mail_list_messages", "{" + f + "}"));
    CHECK(accepts("mail_list_messages", "{" + f + ",\"limit\":1}"));
    CHECK(accepts("mail_list_messages", "{" + f + ",\"limit\":100}"));
    CHECK(!accepts("mail_list_messages", "{" + f + ",\"limit\":0}"));
    CHECK(!accepts("mail_list_messages", "{" + f + ",\"limit\":101}"));
    CHECK(!accepts("mail_list_messages", "{" + f + ",\"limit\":-1}"));
    CHECK(!accepts("mail_list_messages", "{" + f + ",\"limit\":1.5}"));
    CHECK(!accepts("mail_list_messages", "{" + f + ",\"limit\":\"5\"}"));
    CHECK(!accepts("mail_list_messages", "{" + f + ",\"limit\":null}"));
    CHECK(accepts("mail_list_messages", "{" + f + ",\"limit\":1e2}"));   /* the number 100 */
    CHECK(!accepts("mail_list_messages", "{" + f + ",\"limit\":1e400}"));
}

TEST(ToolValidator, SearchRefusesARawCriteriaString)
{
    const std::string f = "\"folder\":\"x\"";
    std::string why;

    CHECK(!accepts("mail_search", "{" + f + ",\"criteria\":\"ALL\"}", &why));
    CHECK(why.find("criteria") != std::string::npos);
    CHECK(!accepts("mail_search", "{" + f + ",\"query\":\"x\"}"));
    CHECK(!accepts("mail_search", "{" + f + ",\"raw\":\"x\"}"));
    CHECK(!accepts("mail_search", "{" + f + ",\"imap\":\"x\"}"));
    CHECK(!accepts("mail_search", "{" + f + ",\"From\":\"x\"}"));   /* case matters */
}

TEST(ToolValidator, SearchFieldsLengthsAndTypes)
{
    const std::string f = "\"folder\":\"x\"";

    CHECK(accepts("mail_search", "{" + f + "}"));
    for (const char *k : { "from", "to", "subject", "text" }) {
        CHECK(accepts("mail_search", "{" + f + ",\"" + k + "\":" + str(1) + "}"));
        CHECK(accepts("mail_search", "{" + f + ",\"" + k + "\":" + str(256) + "}"));
        CHECK(!accepts("mail_search", "{" + f + ",\"" + k + "\":" + str(257) + "}"));
        CHECK(!accepts("mail_search", "{" + f + ",\"" + k + "\":\"\"}"));
        CHECK(!accepts("mail_search", "{" + f + ",\"" + k + "\":5}"));
    }

    for (const char *k : { "seen", "flagged", "unanswered" }) {
        CHECK(accepts("mail_search", "{" + f + ",\"" + k + "\":true}"));
        CHECK(accepts("mail_search", "{" + f + ",\"" + k + "\":false}"));
        CHECK(!accepts("mail_search", "{" + f + ",\"" + k + "\":1}"));
        CHECK(!accepts("mail_search", "{" + f + ",\"" + k + "\":\"true\"}"));
    }

    CHECK(accepts("mail_search", "{" + f + ",\"limit\":100}"));
    CHECK(!accepts("mail_search", "{" + f + ",\"limit\":101}"));
}

TEST(ToolValidator, SearchDateBoundaries)
{
    const std::string f = "\"folder\":\"x\"";
    static const char *good[] = {
        "1970-01-01", "2097-12-31", "2026-02-31", "2000-01-01", NULL
    };
    static const char *bad[] = {
        "1969-12-31", "2098-01-01", "2026-00-10", "2026-13-10", "2026-01-00",
        "2026-01-32", "26-01-01", "2026/01/01", "2026-1-1", "20260101",
        "", "yesterday", "2026-01-01 ", " 2026-01-01", "2026-01-011",
        "abcd-ef-gh", NULL
    };

    for (int i = 0; good[i]; i++) {
        CHECK_TEXT(accepts("mail_search", "{" + f + ",\"since\":\"" + good[i] + "\"}"), good[i]);
        CHECK_TEXT(accepts("mail_search", "{" + f + ",\"before\":\"" + good[i] + "\"}"), good[i]);
    }

    for (int i = 0; bad[i]; i++) {
        CHECK_TEXT(!accepts("mail_search", "{" + f + ",\"since\":\"" + bad[i] + "\"}"), bad[i]);
        CHECK_TEXT(!accepts("mail_search", "{" + f + ",\"before\":\"" + bad[i] + "\"}"), bad[i]);
    }

    CHECK(!accepts("mail_search", "{" + f + ",\"since\":20260101}"));
}

TEST(ToolValidator, UidAndUidvalidityBoundaries)
{
    const std::string f = "\"folder\":\"x\"";
    static const char *tools[] = { "mail_get_message", NULL };
    static const char *good[] = { "1", "2", "4294967295", NULL };
    static const char *bad[] = {
        "0", "-1", "4294967296", "1.5", "\"5\"", "null", "true", "[]", "1e10",
        NULL
    };

    for (int t = 0; tools[t]; t++) {
        for (int i = 0; good[i]; i++) {
            CHECK_TEXT(accepts(tools[t], "{" + f + ",\"uid\":" + good[i]
                               + ",\"uidvalidity\":1}"), good[i]);
            CHECK_TEXT(accepts(tools[t], "{" + f + ",\"uid\":1,\"uidvalidity\":"
                               + good[i] + "}"), good[i]);
        }

        for (int i = 0; bad[i]; i++) {
            CHECK_TEXT(!accepts(tools[t], "{" + f + ",\"uid\":" + bad[i]
                                + ",\"uidvalidity\":1}"), bad[i]);
            CHECK_TEXT(!accepts(tools[t], "{" + f + ",\"uid\":1,\"uidvalidity\":"
                                + bad[i] + "}"), bad[i]);
        }

        CHECK(!accepts(tools[t], "{" + f + ",\"uid\":1}"));
        CHECK(!accepts(tools[t], "{" + f + ",\"uidvalidity\":1}"));
    }
}

TEST(ToolValidator, CopyNeedsBothMailboxStrings)
{
    const std::string base = "\"folder\":\"x\",\"uid\":1,\"uidvalidity\":1";

    CHECK(accepts("mail_copy", "{" + base + ",\"destination\":\"y\"}"));
    CHECK(accepts("mail_copy", "{" + base + ",\"destination\":" + str(1024) + "}"));
    CHECK(!accepts("mail_copy", "{" + base + ",\"destination\":" + str(1025) + "}"));
    CHECK(!accepts("mail_copy", "{" + base + "}"));
    CHECK(!accepts("mail_copy", "{" + base + ",\"destination\":\"\"}"));
    CHECK(accepts("mail_copy", "{" + base + ",\"destination\":\"y\",\"move\":true}"));
    CHECK(accepts("mail_copy", "{" + base + ",\"destination\":\"y\",\"move\":false}"));
    CHECK(!accepts("mail_copy", "{" + base + ",\"destination\":\"y\",\"move\":\"true\"}"));
    CHECK(!accepts("mail_copy", "{" + base + ",\"destination\":\"y\",\"move\":1}"));
    CHECK(!accepts("mail_copy", "{" + base + ",\"destination\":\"y\",\"move\":null}"));
    CHECK(!accepts("mail_copy", "{" + base + ",\"destination\":\"y\",\"delete\":true}"));
}

TEST(ToolValidator, SetFlagAllowsFourFlags)
{
    const std::string base = "\"folder\":\"x\",\"uid\":1,\"uidvalidity\":1";

    CHECK(accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Seen\",\"set\":true}"));
    CHECK(accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Flagged\",\"set\":false}"));
    CHECK(accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Answered\",\"set\":true}"));
    CHECK(accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Deleted\",\"set\":true}"));
    CHECK(accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Deleted\",\"set\":false}"));

    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\deleted\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\DELETED\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"Deleted\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"$Deleted\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Seen \\\\Deleted\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Draft\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Recent\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"Seen\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\seen\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Seen \",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"$Junk\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\",\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":5,\"set\":true}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Seen\"}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Seen\",\"set\":1}"));
    CHECK(!accepts("mail_set_flag", "{" + base + ",\"flag\":\"\\\\Seen\",\"set\":\"true\"}"));
}

TEST(ToolValidator, IdempotencyKeyBoundariesAndCharacters)
{
    for (const char *tool : { "mail_create_draft", "mail_send" }) {
        std::string to = std::string(tool) == "mail_send" ? "\"to\":\"a@example.org\"," : "";

        CHECK(accepts(tool, "{" + to + "\"idempotency_key\":" + str(8) + "}"));
        CHECK(accepts(tool, "{" + to + "\"idempotency_key\":" + str(128) + "}"));
        CHECK(!accepts(tool, "{" + to + "\"idempotency_key\":" + str(7) + "}"));
        CHECK(!accepts(tool, "{" + to + "\"idempotency_key\":" + str(129) + "}"));
        CHECK(!accepts(tool, "{" + to + "\"idempotency_key\":\"\"}"));
        CHECK(!accepts(tool, "{" + to + "\"idempotency_key\":12345678}"));
        CHECK(!accepts(tool, to.empty() ? "{}" : "{\"to\":\"a@example.org\"}"));

        CHECK(accepts(tool, "{" + to + "\"idempotency_key\":\"Az09._-xx\"}"));
        for (const char *bad : { "abcd efgh", "abcd/efgh", "abcd\\\\efgh",
                                 "abcd\\\"efgh", "abcd\\nefgh", "abcd:efgh",
                                 "abcd*efgh", "abcd?efgh", "abcd;efgh",
                                 "abcd\xc3\xa9" "efgh", "abcd\\u0000efg",
                                 "../../etc/passwd", "abcdefg\\t" }) {
            CHECK_TEXT(!accepts(tool, "{" + to + "\"idempotency_key\":\"" + bad + "\"}"), bad);
        }
    }
}

TEST(ToolValidator, MessageFieldsRefuseControlCharactersButBodyMayHaveNewlines)
{
    const std::string key = "\"idempotency_key\":\"abcdefgh\"";

    for (const char *tool : { "mail_create_draft", "mail_send" }) {
        std::string to = std::string(tool) == "mail_send" ? "\"to\":\"a@example.org\"," : "";

        for (const char *field : { "to", "cc", "bcc", "subject", "from" }) {
            if (std::string(field) == "to" && !to.empty()) {
                continue;
            }

            std::string pre = (std::string(field) == "to") ? "" : to;

            CHECK(accepts(tool, "{" + pre + "\"" + field + "\":\"fine text\"," + key + "}"));
            for (const char *bad : { "a\\nb", "a\\rb", "a\\r\\nBcc: x@y.z", "a\\tb",
                                     "a\\u0001b", "a\\u007fb", "\\n" }) {
                CHECK_TEXT(!accepts(tool, "{" + pre + "\"" + field + "\":\"" + bad + "\","
                                    + key + "}"),
                           (std::string(tool) + " " + field + " " + bad).c_str());
            }
        }

        CHECK(accepts(tool, "{" + to + "\"body\":\"line one\\r\\nline two\\n\\ttabbed\"," + key + "}"));
    }
}

TEST(ToolValidator, MessageFieldLengthBoundaries)
{
    const std::string key = "\"idempotency_key\":\"abcdefgh\"";

    for (const char *tool : { "mail_create_draft", "mail_send" }) {
        std::string to = std::string(tool) == "mail_send" ? "\"to\":\"a@example.org\"," : "";

        CHECK(accepts(tool, "{" + to + "\"subject\":" + str(998) + "," + key + "}"));
        CHECK(!accepts(tool, "{" + to + "\"subject\":" + str(999) + "," + key + "}"));
        CHECK(accepts(tool, "{" + to + "\"body\":" + str(262144) + "," + key + "}"));
        CHECK(!accepts(tool, "{" + to + "\"body\":" + str(262145) + "," + key + "}"));
        CHECK(accepts(tool, "{" + to + "\"from\":" + str(128) + "," + key + "}"));
        CHECK(!accepts(tool, "{" + to + "\"from\":" + str(129) + "," + key + "}"));
        CHECK(!accepts(tool, "{" + to + "\"from\":\"\"," + key + "}"));
        CHECK(accepts(tool, "{" + to + "\"cc\":" + str(4096) + "," + key + "}"));
        CHECK(!accepts(tool, "{" + to + "\"cc\":" + str(4097) + "," + key + "}"));
        CHECK(accepts(tool, "{" + to + "\"bcc\":\"\"," + key + "}"));    /* may be empty */
        CHECK(accepts(tool, "{" + to + "\"subject\":\"\"," + key + "}"));
        CHECK(accepts(tool, "{" + to + "\"body\":\"\"," + key + "}"));
        CHECK(!accepts(tool, "{" + to + "\"attachments\":[\"/etc/passwd\"]," + key + "}"));
        CHECK(!accepts(tool, "{" + to + "\"attachment\":\"/etc/passwd\"," + key + "}"));
        CHECK(!accepts(tool, "{" + to + "\"role\":\"x\"," + key + "}"));
    }
}

TEST(ToolValidator, SendNeedsARecipientDraftDoesNot)
{
    const std::string key = "\"idempotency_key\":\"abcdefgh\"";

    CHECK(accepts("mail_create_draft", "{" + key + "}"));
    CHECK(accepts("mail_create_draft", "{\"to\":\"\"," + key + "}"));
    CHECK(!accepts("mail_send", "{" + key + "}"));
    CHECK(!accepts("mail_send", "{\"to\":\"\"," + key + "}"));
    CHECK(accepts("mail_send", "{\"to\":\"a@example.org\"," + key + "}"));
    CHECK(accepts("mail_send", "{\"to\":" + str(4096) + "," + key + "}"));
    CHECK(!accepts("mail_send", "{\"to\":" + str(4097) + "," + key + "}"));
    CHECK(!accepts("mail_send", "{\"to\":5," + key + "}"));
    CHECK(!accepts("mail_send", "{\"to\":[\"a@example.org\"]," + key + "}"));
}

TEST(ToolValidator, AddressBookQueryBoundaries)
{
    CHECK(accepts("mail_search_addressbook", "{\"query\":\"x\"}"));
    CHECK(accepts("mail_search_addressbook", "{\"query\":" + str(200) + "}"));
    CHECK(!accepts("mail_search_addressbook", "{\"query\":" + str(201) + "}"));
    CHECK(!accepts("mail_search_addressbook", "{\"query\":\"\"}"));
    CHECK(!accepts("mail_search_addressbook", "{}"));
    CHECK(!accepts("mail_search_addressbook", "{\"query\":\"x\",\"book\":\"y\"}"));
}

TEST(ToolValidator, ErrorMessagesNameTheProblemAndStayInTheBuffer)
{
    MCP_TOOL_S *t = mcp_tool_lookup("mail_search");
    const char *text = "{\"folder\":\"x\",\"limit\":0}";
    cJSON *args = mcp_json_parse(text, strlen(text));
    char small[12];

    memset(small, 'z', sizeof(small));
    CHECK((*t->validate)(args, small, sizeof(small)) < 0);
    CHECK(memchr(small, '\0', sizeof(small)) != NULL);      /* terminated */
    cJSON_Delete(args);
}

TEST(ToolValidator, UidsArrayBoundariesAndTypes)
{
    auto makeUids = [](int n) {
        std::string s = "[";
        for (int i = 1; i <= n; i++) {
            if (i > 1) s += ",";
            s += std::to_string(i);
        }
        s += "]";
        return s;
    };

    const std::string flagBase = "\"folder\":\"x\",\"uidvalidity\":1,\"flag\":\"\\\\Seen\",\"set\":true";
    const std::string copyBase = "\"folder\":\"x\",\"uidvalidity\":1,\"destination\":\"y\"";

    for (const auto &base : { flagBase, copyBase }) {
        const char *tool = (base == flagBase) ? "mail_set_flag" : "mail_copy";

        /* accepted at 1 and 250 */
        CHECK(accepts(tool, "{" + base + ",\"uids\":[1]}"));
        CHECK(accepts(tool, "{" + base + ",\"uids\":" + makeUids(250) + "}"));

        /* rejected at 0 and 251 */
        CHECK(!accepts(tool, "{" + base + ",\"uids\":[]}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":" + makeUids(251) + "}"));

        /* non-array types */
        CHECK(!accepts(tool, "{" + base + ",\"uids\":1}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":\"1\"}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":null}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":true}"));

        /* element value constraints: 1..4294967295 */
        CHECK(!accepts(tool, "{" + base + ",\"uids\":[0]}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":[-1]}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":[1.5]}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":[4294967296]}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":[\"1\"]}"));

        /* duplicates */
        CHECK(!accepts(tool, "{" + base + ",\"uids\":[1, 1]}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":[1, 2, 3, 2]}"));
        CHECK(!accepts(tool, "{" + base + ",\"uids\":[10, 20, 30, 10]}"));
    }
}

TEST(ToolValidator, FoldersArrayBoundariesAndTypes)
{
    auto makeFolders = [](int n) {
        std::string s = "[";
        for (int i = 1; i <= n; i++) {
            if (i > 1) s += ",";
            s += "\"f" + std::to_string(i) + "\"";
        }
        s += "]";
        return s;
    };

    /* accepted at 1 and 32 */
    CHECK(accepts("mail_folder_status", "{\"folders\":[\"INBOX\"]}"));
    CHECK(accepts("mail_folder_status", "{\"folders\":" + makeFolders(32) + "}"));

    /* rejected at 0 and 33 */
    CHECK(!accepts("mail_folder_status", "{\"folders\":[]}"));
    CHECK(!accepts("mail_folder_status", "{\"folders\":" + makeFolders(33) + "}"));

    /* non-array */
    CHECK(!accepts("mail_folder_status", "{\"folders\":\"INBOX\"}"));
    CHECK(!accepts("mail_folder_status", "{\"folders\":1}"));

    /* element constraints: 1..1024 bytes, strings only */
    CHECK(!accepts("mail_folder_status", "{\"folders\":[\"\"]}"));
    CHECK(!accepts("mail_folder_status", "{\"folders\":[1]}"));
    CHECK(!accepts("mail_folder_status", "{\"folders\":[null]}"));
    CHECK(accepts("mail_folder_status", "{\"folders\":[" + str(1024) + "]}"));
    CHECK(!accepts("mail_folder_status", "{\"folders\":[" + str(1025) + "]}"));

    /* duplicates */
    CHECK(!accepts("mail_folder_status", "{\"folders\":[\"INBOX\", \"INBOX\"]}"));
    CHECK(!accepts("mail_folder_status", "{\"folders\":[\"a\", \"b\", \"a\"]}"));
}

TEST(ToolValidator, ExactlyOneUidOrUidsEnforced)
{
    const std::string flagBase = "\"folder\":\"x\",\"uidvalidity\":1,\"flag\":\"\\\\Seen\",\"set\":true";
    const std::string copyBase = "\"folder\":\"x\",\"uidvalidity\":1,\"destination\":\"y\"";

    for (const auto &base : { flagBase, copyBase }) {
        const char *tool = (base == flagBase) ? "mail_set_flag" : "mail_copy";

        /* neither uid nor uids */
        std::string why;
        CHECK(!accepts(tool, "{" + base + "}", &why));
        CHECK(why.find("uid") != std::string::npos);

        /* both uid and uids */
        CHECK(!accepts(tool, "{" + base + ",\"uid\":1,\"uids\":[1]}", &why));
        CHECK(why.find("uid") != std::string::npos);

        /* single uid alone still works */
        CHECK(accepts(tool, "{" + base + ",\"uid\":1}"));

        /* uids alone works */
        CHECK(accepts(tool, "{" + base + ",\"uids\":[1]}"));
    }
}

TEST(ToolValidator, ExactlyOneFolderOrFoldersEnforced)
{
    /* neither */
    std::string why;
    CHECK(!accepts("mail_folder_status", "{}", &why));
    CHECK(why.find("folder") != std::string::npos);

    /* both */
    CHECK(!accepts("mail_folder_status", "{\"folder\":\"x\",\"folders\":[\"x\"]}", &why));
    CHECK(why.find("folder") != std::string::npos);

    /* single folder */
    CHECK(accepts("mail_folder_status", "{\"folder\":\"x\"}"));

    /* folders */
    CHECK(accepts("mail_folder_status", "{\"folders\":[\"x\"]}"));
}

TEST(ToolValidator, McpUidSetFormattingAndLimits)
{
    char buf[4096];

    /* empty */
    LONGS_EQUAL(0, mcp_uid_set(NULL, 0, buf, sizeof(buf)));
    STRCMP_EQUAL("", buf);

    /* single */
    unsigned long s1[] = { 5 };
    LONGS_EQUAL(1, mcp_uid_set(s1, 1, buf, sizeof(buf)));
    STRCMP_EQUAL("5", buf);

    /* consecutive run */
    unsigned long s2[] = { 7, 8, 9 };
    LONGS_EQUAL(3, mcp_uid_set(s2, 3, buf, sizeof(buf)));
    STRCMP_EQUAL("7:9", buf);

    /* mixed */
    unsigned long s3[] = { 5, 7, 8, 9, 12 };
    LONGS_EQUAL(8, mcp_uid_set(s3, 5, buf, sizeof(buf)));
    STRCMP_EQUAL("5,7:9,12", buf);

    /* unsorted */
    unsigned long s4[] = { 12, 5, 9, 7, 8 };
    LONGS_EQUAL(8, mcp_uid_set(s4, 5, buf, sizeof(buf)));
    STRCMP_EQUAL("5,7:9,12", buf);

    /* exact fit buffer for "5,7:9,12" (len 8 -> buffer 9) */
    char exact[9];
    LONGS_EQUAL(8, mcp_uid_set(s3, 5, exact, sizeof(exact)));
    STRCMP_EQUAL("5,7:9,12", exact);

    /* one byte short (buffer 8) */
    char shortBuf[8];
    memset(shortBuf, 'X', sizeof(shortBuf));
    LONGS_EQUAL(-1, mcp_uid_set(s3, 5, shortBuf, sizeof(shortBuf)));
    BYTES_EQUAL('X', shortBuf[0]);

    /* worst case 250 elements */
    unsigned long worst[250];
    for (int i = 0; i < 250; i++) worst[i] = (i + 1) * 2;
    long len = mcp_uid_set(worst, 250, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(len < (long) sizeof(buf));
}

TEST(ToolValidator, McpBatchClassifyRacesAndPasses)
{
    size_t ngone = 0, nmatch = 0, ndiffer = 0;

    /* all MATCH on pass 1 */
    int s1[] = { MCP_UID_MATCH, MCP_UID_MATCH, MCP_UID_MATCH };
    LONGS_EQUAL(MCP_ACT_DONE, mcp_batch_classify(s1, 3, 1, &ngone, &nmatch, &ndiffer));
    LONGS_EQUAL(0, ngone);
    LONGS_EQUAL(3, nmatch);
    LONGS_EQUAL(0, ndiffer);

    /* only GONE on pass 1 */
    int s2[] = { MCP_UID_GONE, MCP_UID_GONE };
    LONGS_EQUAL(MCP_ACT_DONE, mcp_batch_classify(s2, 2, 1, &ngone, &nmatch, &ndiffer));
    LONGS_EQUAL(2, ngone);
    LONGS_EQUAL(0, nmatch);
    LONGS_EQUAL(0, ndiffer);

    /* GONE + MATCH on pass 1 */
    int s3[] = { MCP_UID_GONE, MCP_UID_MATCH };
    LONGS_EQUAL(MCP_ACT_DONE, mcp_batch_classify(s3, 2, 1, &ngone, &nmatch, &ndiffer));
    LONGS_EQUAL(1, ngone);
    LONGS_EQUAL(1, nmatch);
    LONGS_EQUAL(0, ndiffer);

    /* DIFFER on pass 1 -> REISSUE */
    int s4[] = { MCP_UID_MATCH, MCP_UID_DIFFER, MCP_UID_GONE };
    LONGS_EQUAL(MCP_ACT_REISSUE, mcp_batch_classify(s4, 3, 1, &ngone, &nmatch, &ndiffer));
    LONGS_EQUAL(1, ngone);
    LONGS_EQUAL(1, nmatch);
    LONGS_EQUAL(1, ndiffer);

    /* DIFFER persisting on pass 2 -> FAIL */
    int s5[] = { MCP_UID_DIFFER };
    LONGS_EQUAL(MCP_ACT_FAIL, mcp_batch_classify(s5, 1, 2, &ngone, &nmatch, &ndiffer));
    LONGS_EQUAL(0, ngone);
    LONGS_EQUAL(0, nmatch);
    LONGS_EQUAL(1, ndiffer);

    /* DIFFER resolved to MATCH on pass 2 -> DONE */
    int s6[] = { MCP_UID_MATCH };
    LONGS_EQUAL(MCP_ACT_DONE, mcp_batch_classify(s6, 1, 2, &ngone, &nmatch, &ndiffer));
    LONGS_EQUAL(0, ngone);
    LONGS_EQUAL(1, nmatch);
    LONGS_EQUAL(0, ndiffer);
}

TEST(ToolValidator, PublishedSchemasContainBatchFieldsAndOneOf)
{
    cJSON *toolsObj = mcp_tools_list_json();
    CHECK(toolsObj != NULL);
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(toolsObj, "tools");
    CHECK(arr != NULL);

    for (cJSON *t = arr->child; t; t = t->next) {
        const char *name = cJSON_GetObjectItemCaseSensitive(t, "name")->valuestring;
        cJSON *schema = cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
        CHECK(schema != NULL);

        if (strcmp(name, "mail_set_flag") == 0 || strcmp(name, "mail_copy") == 0) {
            cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
            cJSON *uids = cJSON_GetObjectItemCaseSensitive(props, "uids");
            CHECK(uids != NULL);
            STRCMP_EQUAL("array", cJSON_GetObjectItemCaseSensitive(uids, "type")->valuestring);
            LONGS_EQUAL(1, (int) cJSON_GetObjectItemCaseSensitive(uids, "minItems")->valuedouble);
            LONGS_EQUAL(250, (int) cJSON_GetObjectItemCaseSensitive(uids, "maxItems")->valuedouble);
            CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(uids, "uniqueItems")));

            cJSON *oneOf = cJSON_GetObjectItemCaseSensitive(schema, "oneOf");
            CHECK(oneOf != NULL);
            LONGS_EQUAL(2, cJSON_GetArraySize(oneOf));
        } else if (strcmp(name, "mail_folder_status") == 0) {
            cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
            cJSON *folders = cJSON_GetObjectItemCaseSensitive(props, "folders");
            CHECK(folders != NULL);
            STRCMP_EQUAL("array", cJSON_GetObjectItemCaseSensitive(folders, "type")->valuestring);
            LONGS_EQUAL(1, (int) cJSON_GetObjectItemCaseSensitive(folders, "minItems")->valuedouble);
            LONGS_EQUAL(32, (int) cJSON_GetObjectItemCaseSensitive(folders, "maxItems")->valuedouble);

            cJSON *oneOf = cJSON_GetObjectItemCaseSensitive(schema, "oneOf");
            CHECK(oneOf != NULL);
            LONGS_EQUAL(2, cJSON_GetArraySize(oneOf));
        }
    }

    cJSON_Delete(toolsObj);
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
