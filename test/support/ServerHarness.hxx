/*
 * ServerHarness.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_SERVER_HARNESS_HXX
#define ALPINE_MCP_TEST_SERVER_HARNESS_HXX

#include <string>

#include "TestSupport.hxx"

/*
 * The add-on's server running inside the test process, with the test
 * thread standing in for Alpine's main thread: it waits on the wake
 * descriptor and runs mcp_drain(), exactly as input_ready does.
 * The policy file, the runtime directory and the state directory are all
 * under a temporary directory.
 */
class ServerHarness
{
public:
    ServerHarness();
    ~ServerHarness();

    /* config.json text; written with mode 0600 and loaded */
    void useConfig(const std::string &json);

    /* a standard policy: MCP on, the given lock and allow list, aimon off */
    void usePolicy(bool lock = false,
                   const std::string &allowedJson = "[\"ok@example.org\"]");

    int start();                    /* mcp_server_start(); 0 on success */
    void stop();
    bool running() const;

    std::string socketPath() const;
    std::string home() const { return _tmp.path(); }
    const TempDir &dir() const { return _tmp; }

    /* wait up to timeoutMs for queued work, run it; true if any ran */
    bool drain(int timeoutMs = 2000);

    /* drain repeatedly until nothing is queued for quietMs */
    int drainAll(int timeoutMs = 2000, int quietMs = 150);

private:
    ServerHarness(const ServerHarness &);
    ServerHarness &operator=(const ServerHarness &);

    TempDir _tmp;
    EnvGuard _runtime;
    EnvGuard _state;
    EnvGuard _home;
    bool _started;
};

/* the number of open file descriptors of this process, and what they are */
int openDescriptors();
std::string descriptorList();

#endif /* ALPINE_MCP_TEST_SERVER_HARNESS_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
