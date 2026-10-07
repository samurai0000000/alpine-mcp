/*
 * FakeGateway.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_FAKE_GATEWAY_HXX
#define ALPINE_MCP_TEST_FAKE_GATEWAY_HXX

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
 * A stand-in for aimon's TCP gateway: it listens on the loopback, answers
 * gateway/register the way the test says, records what it was sent, and can
 * push a request to the add-on or drop the connection.
 */
class FakeGateway
{
public:
    enum Mode {
        Accept,         /* {"status":"registered"} */
        Refuse,         /* {"status":"rejected"} */
        NoStatus,       /* an error object, no status */
        Silent,         /* never answers */
        CloseAtOnce     /* closes as soon as it has read the register line */
    };

    FakeGateway();
    ~FakeGateway();

    bool start(int port = 0);           /* 0: any free port */
    void stop();
    int port() const { return _port; }

    void setMode(Mode m);
    int connections();
    int registrations();
    std::vector<long long> registerTimesMs();
    std::string lastRegister();
    bool waitForRegistrations(int n, int timeoutMs);

    bool send(const std::string &line);                 /* to the add-on */
    bool nextLine(std::string &out, int timeoutMs);     /* from the add-on */
    bool waitForEof(int timeoutMs);                     /* the add-on closed */
    void dropClient();

    static long long nowMs();

private:
    FakeGateway(const FakeGateway &);
    FakeGateway &operator=(const FakeGateway &);

    void run();

    int _listenFd;
    int _port;
    int _clientFd;
    bool _stop;
    bool _eof;
    Mode _mode;
    int _connections;
    std::vector<std::string> _registers;
    std::vector<long long> _times;
    std::deque<std::string> _lines;
    std::mutex _mutex;
    std::condition_variable _cv;
    std::thread _thread;
};

#endif /* ALPINE_MCP_TEST_FAKE_GATEWAY_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
