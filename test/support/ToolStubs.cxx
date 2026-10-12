/*
 * ToolStubs.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ToolStubs.hxx"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>

extern "C" {
#include "mcpjson.h"
#include "mcp.h"
#include "mcpint.h"
}

StubBehavior stubBehavior;

StubBehavior::StubBehavior()
{
    reset();
}

void
StubBehavior::reset()
{
    /* swap with an empty string: clear() would keep the memory, and the
       leak check at the end of a test would count it */
    code = 0;
    std::string().swap(message);
    std::string().swap(resultJson);
    park = false;
    std::string().swap(prompt);
    delayMs = 0;
    streamReadOnly = false;
    runs = 0;
    confirmedRuns = 0;
    lastTool[0] = '\0';
    lastArgs[0] = '\0';
}

namespace
{
    void
    stubRun(const char *tool, cJSON *args, MCP_CALL_S *call, bool confirmed)
    {
        stubBehavior.runs++;
        snprintf(stubBehavior.lastTool, sizeof(stubBehavior.lastTool), "%s", tool);

        char *text = args != NULL ? cJSON_PrintUnformatted(args) : NULL;

        snprintf(stubBehavior.lastArgs, sizeof(stubBehavior.lastArgs), "%s",
                 text != NULL ? text : "");
        if (text != NULL) {
            cJSON_free(text);
        }

        if (stubBehavior.delayMs > 0) {
            struct timespec ts;

            ts.tv_sec = stubBehavior.delayMs / 1000;
            ts.tv_nsec = (long) (stubBehavior.delayMs % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }

        if (confirmed) {
            stubBehavior.confirmedRuns++;
        } else if (stubBehavior.park) {
            call->park = 1;
            snprintf(call->park_prompt, sizeof(call->park_prompt), "%s",
                     stubBehavior.prompt.c_str());
            return;
        }

        if (stubBehavior.code != 0) {
            mcp_call_error(call, stubBehavior.code, "%s",
                           stubBehavior.message.c_str());
            return;
        }

        if (!stubBehavior.resultJson.empty()) {
            call->result = mcp_json_parse(stubBehavior.resultJson.c_str(),
                                          stubBehavior.resultJson.size());
        }

        if (call->result == NULL) {
            call->result = cJSON_CreateObject();
            cJSON_AddStringToObject(call->result, "tool", tool);
            cJSON_AddBoolToObject(call->result, "ok", 1);
        }

        snprintf(call->summary, sizeof(call->summary), "stub");
    }
}

extern "C" {

void mcp_run_list_folders(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_list_folders", a, c, false);
}

void mcp_run_folder_status(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_folder_status", a, c, false);
}

void mcp_run_list_messages(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_list_messages", a, c, false);
}

void mcp_run_search(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_search", a, c, false);
}

void mcp_run_get_message(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_get_message", a, c, false);
}

void mcp_run_search_addressbook(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_search_addressbook", a, c, false);
}

void mcp_run_copy(cJSON *a, MCP_CALL_S *c)
{
    if (stubBehavior.streamReadOnly) {
        cJSON *mv = cJSON_GetObjectItemCaseSensitive(a, "move");

        if (mv != NULL && cJSON_IsTrue(mv)) {
            stubBehavior.runs++;
            snprintf(stubBehavior.lastTool, sizeof(stubBehavior.lastTool), "mail_copy");
            mcp_call_error(c, MCP_ERR_INTERNAL, "the folder is read-only");
            return;
        }
    }

    stubRun("mail_copy", a, c, false);
}

void mcp_run_set_flag(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_set_flag", a, c, false);
}

void mcp_run_create_draft(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_create_draft", a, c, false);
}

void mcp_run_send(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_send", a, c, false);
}

void mcp_run_send_confirmed(cJSON *a, MCP_CALL_S *c)
{
    stubRun("mail_send", a, c, true);
}

} /* extern "C" */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
