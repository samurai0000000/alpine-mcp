/*
 * JsonLineClient.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "JsonLineClient.hxx"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

JsonLineClient::JsonLineClient()
    : _fd(-1), _eof(false)
{
}

JsonLineClient::~JsonLineClient()
{
    closeNow();
}

bool
JsonLineClient::connectTo(const std::string &socketPath)
{
    closeNow();
    _eof = false;
    _buf.clear();

    struct sockaddr_un sun;

    if (socketPath.size() >= sizeof(sun.sun_path)) {
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        return false;
    }

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    memcpy(sun.sun_path, socketPath.c_str(), socketPath.size() + 1);
    if (connect(fd, (struct sockaddr *) &sun, sizeof(sun)) < 0) {
        close(fd);
        return false;
    }

    _fd = fd;
    return true;
}

void
JsonLineClient::closeNow()
{
    if (_fd >= 0) {
        close(_fd);
        _fd = -1;
    }
}

void
JsonLineClient::shutdownWrite()
{
    if (_fd >= 0) {
        shutdown(_fd, SHUT_WR);
    }
}

bool
JsonLineClient::sendRaw(const std::string &bytes)
{
    size_t done = 0;

    while (_fd >= 0 && done < bytes.size()) {
        ssize_t n = send(_fd, bytes.data() + done, bytes.size() - done,
                         MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN) {
                struct pollfd p = { _fd, POLLOUT, 0 };

                poll(&p, 1, 1000);
                continue;
            }

            return false;
        }

        done += (size_t) n;
    }

    return done == bytes.size();
}

bool
JsonLineClient::sendLine(const std::string &line)
{
    return sendRaw(line + "\n");
}

bool
JsonLineClient::readLine(std::string &out, int timeoutMs)
{
    for (;;) {
        size_t nl = _buf.find('\n');

        if (nl != std::string::npos) {
            out = _buf.substr(0, nl);
            _buf.erase(0, nl + 1);
            return true;
        }

        if (_fd < 0 || _eof) {
            return false;
        }

        struct pollfd p = { _fd, POLLIN, 0 };
        int rc = poll(&p, 1, timeoutMs);

        if (rc < 0 && errno == EINTR) {
            continue;
        }

        if (rc <= 0) {
            return false;
        }

        char buf[65536];
        ssize_t n = read(_fd, buf, sizeof(buf));

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            _eof = true;
            return false;
        }

        if (n == 0) {
            _eof = true;
            continue;
        }

        _buf.append(buf, (size_t) n);
    }
}

cJSON *
JsonLineClient::readJson(int timeoutMs)
{
    std::string line;

    if (!readLine(line, timeoutMs)) {
        return NULL;
    }

    return cJSON_ParseWithLength(line.c_str(), line.size());
}

std::string
JsonLineClient::request(const std::string &id, const std::string &method,
                        const std::string &paramsJson)
{
    std::string s = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"method\":\""
                    + method + "\"";

    if (!paramsJson.empty()) {
        s += ",\"params\":" + paramsJson;
    }

    return s + "}";
}

std::string
JsonLineClient::toolCall(const std::string &id, const std::string &tool,
                         const std::string &argsJson)
{
    return request(id, "tools/call",
                   "{\"name\":\"" + tool + "\",\"arguments\":" + argsJson + "}");
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
