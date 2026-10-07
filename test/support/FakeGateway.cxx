/*
 * FakeGateway.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "FakeGateway.hxx"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

FakeGateway::FakeGateway()
    : _listenFd(-1), _port(0), _clientFd(-1), _stop(false), _eof(false),
      _mode(Accept), _connections(0)
{
}

FakeGateway::~FakeGateway()
{
    stop();
}

long long
FakeGateway::nowMs()
{
    using namespace std::chrono;

    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool
FakeGateway::start(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return false;
    }

    int one = 1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sin;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons((unsigned short) port);
    if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0
        || listen(fd, 4) < 0) {
        close(fd);
        return false;
    }

    socklen_t len = sizeof(sin);

    getsockname(fd, (struct sockaddr *) &sin, &len);
    _port = ntohs(sin.sin_port);
    _listenFd = fd;
    _stop = false;
    _thread = std::thread(&FakeGateway::run, this);
    return true;
}

void
FakeGateway::stop()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stop = true;
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    if (_listenFd >= 0) {
        close(_listenFd);
        _listenFd = -1;
    }

    if (_clientFd >= 0) {
        close(_clientFd);
        _clientFd = -1;
    }
}

void
FakeGateway::setMode(Mode m)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _mode = m;
}

int
FakeGateway::connections()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _connections;
}

int
FakeGateway::registrations()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return (int) _registers.size();
}

std::vector<long long>
FakeGateway::registerTimesMs()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _times;
}

std::string
FakeGateway::lastRegister()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _registers.empty() ? "" : _registers.back();
}

bool
FakeGateway::waitForRegistrations(int n, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(_mutex);

    return _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this, n] { return (int) _registers.size() >= n; });
}

bool
FakeGateway::send(const std::string &line)
{
    int fd;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        fd = _clientFd;
    }

    if (fd < 0) {
        return false;
    }

    std::string out = line + "\n";
    size_t done = 0;

    while (done < out.size()) {
        ssize_t n = ::send(fd, out.data() + done, out.size() - done, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        done += (size_t) n;
    }

    return true;
}

bool
FakeGateway::nextLine(std::string &out, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(_mutex);

    if (!_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                      [this] { return !_lines.empty(); })) {
        return false;
    }

    out = _lines.front();
    _lines.pop_front();
    return true;
}

bool
FakeGateway::waitForEof(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(_mutex);

    return _cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this] { return _eof; });
}

void
FakeGateway::dropClient()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_clientFd >= 0) {
        shutdown(_clientFd, SHUT_RDWR);
    }
}

void
FakeGateway::run()
{
    std::string buf;

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_stop) {
                break;
            }
        }

        struct pollfd p[2];
        int n = 0;

        p[n].fd = _listenFd;
        p[n].events = POLLIN;
        p[n++].revents = 0;
        int cfd;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            cfd = _clientFd;
        }

        if (cfd >= 0) {
            p[n].fd = cfd;
            p[n].events = POLLIN;
            p[n++].revents = 0;
        }

        if (poll(p, (nfds_t) n, 50) <= 0) {
            continue;
        }

        if (p[0].revents & POLLIN) {
            int fd = accept(_listenFd, NULL, NULL);

            if (fd >= 0) {
                std::lock_guard<std::mutex> lock(_mutex);

                if (_clientFd >= 0) {
                    close(_clientFd);
                }

                _clientFd = fd;
                _eof = false;
                _connections++;
                buf.clear();
                _cv.notify_all();
            }
        }

        if (n == 2 && p[1].revents != 0) {
            char tmp[65536];
            ssize_t got = read(cfd, tmp, sizeof(tmp));

            if (got <= 0) {
                std::lock_guard<std::mutex> lock(_mutex);

                close(_clientFd);
                _clientFd = -1;
                _eof = true;
                buf.clear();
                _cv.notify_all();
                continue;
            }

            buf.append(tmp, (size_t) got);
            size_t nl;

            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);

                buf.erase(0, nl + 1);
                if (line.find("\"gateway/register\"") != std::string::npos) {
                    Mode m;

                    {
                        std::lock_guard<std::mutex> lock(_mutex);
                        _registers.push_back(line);
                        _times.push_back(nowMs());
                        m = _mode;
                        _cv.notify_all();
                    }

                    std::string reply;

                    switch (m) {
                    case Accept:
                        reply = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"status\":\"registered\"}}";
                        break;
                    case Refuse:
                        reply = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"status\":\"rejected\"}}";
                        break;
                    case NoStatus:
                        reply = "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-1,\"message\":\"no\"}}";
                        break;
                    case Silent:
                        break;
                    case CloseAtOnce:
                        dropClient();
                        break;
                    }

                    if (!reply.empty()) {
                        send(reply);
                    }
                } else {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _lines.push_back(line);
                    _cv.notify_all();
                }
            }
        }
    }
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
