/*
 * JsonLineClient.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_JSON_LINE_CLIENT_HXX
#define ALPINE_MCP_TEST_JSON_LINE_CLIENT_HXX

#include <string>

extern "C" {
#include "cJSON.h"
}

/*
 * A client of the add-on's Unix socket: one JSON-RPC message per line, in
 * both directions, as an agent or a stdio bridge would.
 */
class JsonLineClient
{
public:
    JsonLineClient();
    ~JsonLineClient();

    bool connectTo(const std::string &socketPath);
    void closeNow();
    void shutdownWrite();

    bool sendRaw(const std::string &bytes);
    bool sendLine(const std::string &line);

    /* the next line without its newline; false on timeout or end of file */
    bool readLine(std::string &out, int timeoutMs);

    /* the next line parsed; NULL on timeout, end of file or bad JSON.
       The caller frees it with cJSON_Delete. */
    cJSON *readJson(int timeoutMs);

    bool atEof() const { return _eof; }
    bool isOpen() const { return _fd >= 0; }

    /* a JSON-RPC request line */
    static std::string request(const std::string &id, const std::string &method,
                               const std::string &paramsJson = "");
    static std::string toolCall(const std::string &id, const std::string &tool,
                                const std::string &argsJson);

private:
    JsonLineClient(const JsonLineClient &);
    JsonLineClient &operator=(const JsonLineClient &);

    int _fd;
    bool _eof;
    std::string _buf;
};

#endif /* ALPINE_MCP_TEST_JSON_LINE_CLIENT_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
