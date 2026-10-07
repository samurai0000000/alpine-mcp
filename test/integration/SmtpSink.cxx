/*
 * SmtpSink.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "SmtpSink.hxx"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace
{
    bool
    sendAll(int fd, const std::string &s)
    {
        size_t off = 0;

        while (off < s.size()) {
            ssize_t n = write(fd, s.data() + off, s.size() - off);

            if (n < 0 && errno == EINTR) {
                continue;
            }

            if (n <= 0) {
                return false;
            }

            off += (size_t) n;
        }

        return true;
    }

    /* the next line, without CRLF; false on end, error or after 10 s idle */
    bool
    readLine(int fd, std::string &buf, std::string &line, volatile bool &stop)
    {
        for (;;) {
            size_t nl = buf.find('\n');

            if (nl != std::string::npos) {
                line = buf.substr(0, nl);
                buf.erase(0, nl + 1);

                if (!line.empty() && line[line.size() - 1] == '\r') {
                    line.erase(line.size() - 1);
                }

                return true;
            }

            if (stop) {
                return false;
            }

            struct pollfd p;

            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            int r = poll(&p, 1, 200);

            if (r < 0 && errno != EINTR) {
                return false;
            }

            if (r > 0) {
                char tmp[4096];
                ssize_t n = read(fd, tmp, sizeof(tmp));

                if (n <= 0) {
                    return false;
                }

                buf.append(tmp, (size_t) n);
            }
        }
    }

    std::string
    angle(const std::string &line)
    {
        size_t a = line.find('<');
        size_t b = line.find('>', a == std::string::npos ? 0 : a);

        if (a == std::string::npos || b == std::string::npos || b < a) {
            return "";
        }

        return line.substr(a + 1, b - a - 1);
    }
}

SmtpSink::SmtpSink()
    : _listenFd(-1), _port(0), _started(false), _stop(false),
      _rcptReply()
{
    pthread_mutex_init(&_lock, NULL);
}

SmtpSink::~SmtpSink()
{
    stop();
    pthread_mutex_destroy(&_lock);
}

bool
SmtpSink::start()
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    int one = 1;

    _listenFd = socket(AF_INET, SOCK_STREAM, 0);

    if (_listenFd < 0) {
        return false;
    }

    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sa.sin_family = AF_INET;
    sa.sin_port = 0;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(_listenFd, (struct sockaddr *) &sa, sizeof(sa)) != 0
        || listen(_listenFd, 8) != 0
        || getsockname(_listenFd, (struct sockaddr *) &sa, &len) != 0) {
        close(_listenFd);
        _listenFd = -1;
        return false;
    }

    _port = ntohs(sa.sin_port);
    _stop = false;

    if (pthread_create(&_thread, NULL, threadMain, this) != 0) {
        close(_listenFd);
        _listenFd = -1;
        return false;
    }

    _started = true;
    return true;
}

void
SmtpSink::stop()
{
    if (!_started) {
        return;
    }

    _stop = true;
    pthread_join(_thread, NULL);
    _started = false;
    close(_listenFd);
    _listenFd = -1;
}

void
SmtpSink::rejectRecipients(const std::string &reply)
{
    pthread_mutex_lock(&_lock);
    _rcptReply = reply;
    pthread_mutex_unlock(&_lock);
}

size_t
SmtpSink::count()
{
    pthread_mutex_lock(&_lock);
    size_t n = _mails.size();

    pthread_mutex_unlock(&_lock);
    return n;
}

std::vector<SinkMail>
SmtpSink::mails()
{
    pthread_mutex_lock(&_lock);
    std::vector<SinkMail> copy = _mails;

    pthread_mutex_unlock(&_lock);
    return copy;
}

bool
SmtpSink::waitForCount(size_t n, int timeoutMs)
{
    struct timespec ts = { 0, 50 * 1000 * 1000 };

    for (int waited = 0; waited <= timeoutMs; waited += 50) {
        if (count() >= n) {
            return true;
        }

        nanosleep(&ts, NULL);
    }

    return count() >= n;
}

void *
SmtpSink::threadMain(void *arg)
{
    SmtpSink *self = static_cast<SmtpSink *>(arg);

    while (!self->_stop) {
        struct pollfd p;

        p.fd = self->_listenFd;
        p.events = POLLIN;
        p.revents = 0;

        if (poll(&p, 1, 200) > 0) {
            int fd = accept(self->_listenFd, NULL, NULL);

            if (fd >= 0) {
                self->serve(fd);
                close(fd);
            }
        }
    }

    return NULL;
}

void
SmtpSink::serve(int fd)
{
    std::string buf, line;
    SinkMail cur;

    if (!sendAll(fd, "220 sink ESMTP\r\n")) {
        return;
    }

    while (readLine(fd, buf, line, _stop)) {
        std::string up = line.substr(0, 4);

        for (size_t i = 0; i < up.size(); i++) {
            if (up[i] >= 'a' && up[i] <= 'z') {
                up[i] = (char) (up[i] - 'a' + 'A');
            }
        }

        if (up == "EHLO") {
            sendAll(fd, "250-sink\r\n250 8BITMIME\r\n");
        } else if (up == "HELO") {
            sendAll(fd, "250 sink\r\n");
        } else if (up == "MAIL") {
            cur = SinkMail();
            cur.from = angle(line);
            sendAll(fd, "250 ok\r\n");
        } else if (up == "RCPT") {
            pthread_mutex_lock(&_lock);
            std::string reject = _rcptReply;

            pthread_mutex_unlock(&_lock);

            if (!reject.empty()) {
                sendAll(fd, reject + "\r\n");
            } else {
                cur.rcpts.push_back(angle(line));
                sendAll(fd, "250 ok\r\n");
            }
        } else if (up == "DATA") {
            sendAll(fd, "354 go ahead\r\n");
            std::string data;

            while (readLine(fd, buf, line, _stop)) {
                if (line == ".") {
                    break;
                }

                if (line.size() > 1 && line[0] == '.') {
                    line.erase(0, 1);
                }

                data += line + "\r\n";
            }

            cur.data = data;
            pthread_mutex_lock(&_lock);
            _mails.push_back(cur);
            pthread_mutex_unlock(&_lock);
            sendAll(fd, "250 queued\r\n");
        } else if (up == "RSET" || up == "NOOP") {
            sendAll(fd, "250 ok\r\n");
        } else if (up == "QUIT") {
            sendAll(fd, "221 bye\r\n");
            return;
        } else {
            sendAll(fd, "502 not implemented\r\n");
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
