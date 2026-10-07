/*
 * SmtpSink.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_SMTP_SINK_HXX
#define ALPINE_MCP_TEST_SMTP_SINK_HXX

#include <pthread.h>
#include <string>
#include <vector>

/* One message as the sink received it. */
struct SinkMail
{
    std::string from;                   /* MAIL FROM, without the brackets */
    std::vector<std::string> rcpts;     /* RCPT TO, without the brackets */
    std::string data;                   /* the DATA part, dot-unstuffed */
};

/*
 * A loopback SMTP server that takes every message and keeps it, in place
 * of a real mail host.  Plain text only, no authentication: Alpine is
 * pointed at it with the /notls option.
 */
class SmtpSink
{
public:
    SmtpSink();
    ~SmtpSink();

    bool start();                       /* on 127.0.0.1, any free port */
    void stop();
    int port() const { return _port; }

    /* make the next RCPT TO fail with this reply, e.g. "550 no such user" */
    void rejectRecipients(const std::string &reply);

    size_t count();
    std::vector<SinkMail> mails();
    bool waitForCount(size_t n, int timeoutMs);

private:
    SmtpSink(const SmtpSink &);
    SmtpSink &operator=(const SmtpSink &);

    static void *threadMain(void *arg);
    void serve(int fd);

    int _listenFd;
    int _port;
    pthread_t _thread;
    bool _started;
    volatile bool _stop;
    pthread_mutex_t _lock;
    std::vector<SinkMail> _mails;
    std::string _rcptReply;
};

#endif /* ALPINE_MCP_TEST_SMTP_SINK_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
