/*
 * ServerTestUtil.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_SERVER_TEST_UTIL_HXX
#define ALPINE_MCP_TEST_SERVER_TEST_UTIL_HXX

#include <string>

extern "C" {
#include "cJSON.h"
}

/* Owns a parsed JSON value and frees it. */
class Json
{
public:
    explicit Json(cJSON *p = NULL) : _p(p) {}
    ~Json() { if (_p != NULL) { cJSON_Delete(_p); } }

    bool ok() const { return _p != NULL; }
    cJSON *get() const { return _p; }

    /* members, NULL when absent */
    cJSON *at(const char *key) const
    {
        return _p != NULL ? cJSON_GetObjectItemCaseSensitive(_p, key) : NULL;
    }

    std::string str(const char *key) const
    {
        cJSON *v = at(key);

        return (cJSON_IsString(v) && v->valuestring != NULL) ? v->valuestring : "";
    }

    /* a JSON-RPC error code, or 0 when the reply is not an error */
    int errorCode() const
    {
        cJSON *e = at("error");
        cJSON *c = e != NULL ? cJSON_GetObjectItemCaseSensitive(e, "code") : NULL;

        return cJSON_IsNumber(c) ? (int) c->valuedouble : 0;
    }

    std::string errorMessage() const
    {
        cJSON *e = at("error");
        cJSON *m = e != NULL ? cJSON_GetObjectItemCaseSensitive(e, "message") : NULL;

        return (cJSON_IsString(m) && m->valuestring != NULL) ? m->valuestring : "";
    }

    /* the tool result: the text of content[0], parsed; NULL if absent.
       The caller owns it: wrap it in a Json. */
    cJSON *toolResult() const
    {
        cJSON *r = at("result");
        cJSON *c = r != NULL ? cJSON_GetObjectItemCaseSensitive(r, "content") : NULL;
        cJSON *first = cJSON_IsArray(c) ? cJSON_GetArrayItem(c, 0) : NULL;
        cJSON *text = first != NULL ? cJSON_GetObjectItemCaseSensitive(first, "text") : NULL;

        if (!cJSON_IsString(text)) {
            return NULL;
        }

        return cJSON_Parse(text->valuestring);
    }

    bool isErrorFlag() const
    {
        cJSON *r = at("result");
        cJSON *f = r != NULL ? cJSON_GetObjectItemCaseSensitive(r, "isError") : NULL;

        return cJSON_IsTrue(f);
    }

private:
    Json(const Json &);
    Json &operator=(const Json &);

    cJSON *_p;
};

#endif /* ALPINE_MCP_TEST_SERVER_TEST_UTIL_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
