/*
 * ImapResilienceIntegrationTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

#include "IntegrationRig.hxx"

#include "CppUTest/TestHarness.h"

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

namespace
{

class FakeImapServer
{
public:
    enum GreetingMode {
        GreetingPreauth,
        GreetingRefuseBye,
        GreetingDrop
    };

    enum StatusMode {
        StatusNormal,
        StatusDropDuring
    };

    struct FakeMsg {
        unsigned long uid;
        std::string from;
        std::string to;
        std::string subject;
        std::string date;
        std::string messageId;
        std::string body;
        bool deleted;
        bool seen;
        bool flagged;

        FakeMsg(unsigned long u, const std::string &subj,
                const std::string &f = "test@example.org",
                const std::string &t = "me@example.test",
                const std::string &d = "Mon, 05 Jan 2026 10:00:00 +0000",
                const std::string &mid = "",
                const std::string &b = "")
            : uid(u), from(f), to(t), subject(subj), date(d),
              messageId(mid.empty() ? ("<msg" + std::to_string(u) + "@example.org>") : mid),
              body(b.empty() ? ("Body of " + subj + "\n") : b),
              deleted(false), seen(false), flagged(false)
        {
        }
    };

    FakeImapServer()
        : _listenFd(-1),
          _port(0),
          _started(false),
          _stop(false),
          _greetingMode(GreetingPreauth),
          _statusMode(StatusNormal),
          _connectionCount(0),
          _uidValidity(1),
          _uidNext(1)
    {
        pthread_mutex_init(&_lock, NULL);
    }

    ~FakeImapServer()
    {
        stop();
        pthread_mutex_destroy(&_lock);
    }

    void setGreetingMode(GreetingMode mode) { _greetingMode = mode; }
    void setStatusMode(StatusMode mode) { _statusMode = mode; }
    int port() const { return _port; }

    void setUidValidity(unsigned long uv)
    {
        pthread_mutex_lock(&_lock);
        _uidValidity = uv;
        pthread_mutex_unlock(&_lock);
    }

    void addMessage(const FakeMsg &msg)
    {
        pthread_mutex_lock(&_lock);
        _messages.push_back(msg);
        if (msg.uid >= _uidNext) {
            _uidNext = msg.uid + 1;
        }
        pthread_mutex_unlock(&_lock);
    }

    void expungeMessageByUid(unsigned long uid)
    {
        pthread_mutex_lock(&_lock);
        for (size_t i = 0; i < _messages.size(); i++) {
            if (_messages[i].uid == uid) {
                _messages.erase(_messages.begin() + i);
                break;
            }
        }
        pthread_mutex_unlock(&_lock);
    }

    void disconnectAllClientsWithBye()
    {
        pthread_mutex_lock(&_lock);
        for (size_t i = 0; i < _activeClients.size(); i++) {
            int fd = _activeClients[i];
            sendAll(fd, "* BYE Server restarting\r\n");
            shutdown(fd, SHUT_RDWR);
        }
        pthread_mutex_unlock(&_lock);
    }

    bool start()
    {
        _listenFd = socket(AF_INET, SOCK_STREAM, 0);
        if (_listenFd < 0) {
            return false;
        }

        int yes = 1;
        setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sin.sin_port = 0;

        if (bind(_listenFd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }

        socklen_t len = sizeof(sin);
        if (getsockname(_listenFd, (struct sockaddr *) &sin, &len) < 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }
        _port = ntohs(sin.sin_port);

        if (listen(_listenFd, 10) < 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }

        _stop = false;
        if (pthread_create(&_thread, NULL, threadMain, this) != 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }

        _started = true;
        return true;
    }

    void stop()
    {
        if (!_started) {
            return;
        }

        _stop = true;
        if (_listenFd >= 0) {
            shutdown(_listenFd, SHUT_RDWR);
            close(_listenFd);
            _listenFd = -1;
        }

        disconnectAllClientsWithBye();

        pthread_join(_thread, NULL);

        pthread_mutex_lock(&_lock);
        std::vector<pthread_t> threads = _clientThreads;
        _clientThreads.clear();
        pthread_mutex_unlock(&_lock);

        for (size_t i = 0; i < threads.size(); i++) {
            pthread_join(threads[i], NULL);
        }

        _started = false;
    }

    int connectionCount()
    {
        pthread_mutex_lock(&_lock);
        int c = _connectionCount;
        pthread_mutex_unlock(&_lock);
        return c;
    }

private:
    FakeImapServer(const FakeImapServer &);
    FakeImapServer &operator=(const FakeImapServer &);

    struct ClientSession {
        FakeImapServer *server;
        int fd;
        ClientSession(FakeImapServer *s, int f) : server(s), fd(f) {}
    };

    static void *clientThreadMain(void *arg)
    {
        ClientSession *cs = static_cast<ClientSession *>(arg);
        cs->server->serve(cs->fd);
        cs->server->removeClient(cs->fd);
        close(cs->fd);
        delete cs;
        return NULL;
    }

    void removeClient(int fd)
    {
        pthread_mutex_lock(&_lock);
        for (size_t i = 0; i < _activeClients.size(); i++) {
            if (_activeClients[i] == fd) {
                _activeClients.erase(_activeClients.begin() + i);
                break;
            }
        }
        pthread_mutex_unlock(&_lock);
    }

    static void *threadMain(void *arg)
    {
        FakeImapServer *self = static_cast<FakeImapServer *>(arg);
        self->run();
        return NULL;
    }

    void run()
    {
        while (!_stop) {
            struct pollfd p;
            p.fd = _listenFd;
            p.events = POLLIN;
            p.revents = 0;

            int r = poll(&p, 1, 200);
            if (r <= 0 || !(p.revents & POLLIN)) {
                continue;
            }

            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(_listenFd, (struct sockaddr *) &clientAddr,
                                  &clientLen);
            if (clientFd < 0) {
                continue;
            }

            pthread_mutex_lock(&_lock);
            _connectionCount++;
            _activeClients.push_back(clientFd);
            pthread_mutex_unlock(&_lock);

            ClientSession *cs = new ClientSession(this, clientFd);
            pthread_t ct;
            if (pthread_create(&ct, NULL, clientThreadMain, cs) == 0) {
                pthread_mutex_lock(&_lock);
                _clientThreads.push_back(ct);
                pthread_mutex_unlock(&_lock);
            } else {
                removeClient(clientFd);
                close(clientFd);
                delete cs;
            }
        }
    }

    bool sendAll(int fd, const std::string &s)
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

    bool readLine(int fd, std::string &buf, std::string &line)
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

            if (_stop) {
                return false;
            }

            struct pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            int r = poll(&p, 1, 300);
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

    std::vector<size_t> matchMessages(const std::string &spec, bool isUid)
    {
        std::vector<size_t> indices;
        if (_messages.empty()) {
            return indices;
        }

        size_t start = 0;
        while (start < spec.size()) {
            size_t comma = spec.find(',', start);
            std::string part = (comma != std::string::npos)
                                   ? spec.substr(start, comma - start)
                                   : spec.substr(start);
            start = (comma != std::string::npos) ? comma + 1 : spec.size();

            size_t colon = part.find(':');
            if (colon == std::string::npos) {
                if (part == "*") {
                    indices.push_back(_messages.size() - 1);
                } else {
                    unsigned long val = strtoul(part.c_str(), NULL, 10);
                    for (size_t i = 0; i < _messages.size(); i++) {
                        if (isUid ? (_messages[i].uid == val) : (i + 1 == val)) {
                            indices.push_back(i);
                            break;
                        }
                    }
                }
            } else {
                std::string sA = part.substr(0, colon);
                std::string sB = part.substr(colon + 1);
                unsigned long vA = (sA == "*") ? (isUid ? _messages.back().uid : _messages.size())
                                               : strtoul(sA.c_str(), NULL, 10);
                unsigned long vB = (sB == "*") ? (isUid ? _messages.back().uid : _messages.size())
                                               : strtoul(sB.c_str(), NULL, 10);
                if (vA > vB) {
                    std::swap(vA, vB);
                }
                for (size_t i = 0; i < _messages.size(); i++) {
                    unsigned long cur = isUid ? _messages[i].uid : (i + 1);
                    if (cur >= vA && cur <= vB) {
                        indices.push_back(i);
                    }
                }
            }
        }
        return indices;
    }

    void handleFetch(int fd, const std::string &tag, bool isUid, const std::string &fetchArgs)
    {
        size_t sp = fetchArgs.find(' ');
        std::string seqSpec = (sp != std::string::npos) ? fetchArgs.substr(0, sp) : fetchArgs;
        std::string items = (sp != std::string::npos) ? fetchArgs.substr(sp + 1) : "";

        pthread_mutex_lock(&_lock);
        std::vector<size_t> indices = matchMessages(seqSpec, isUid);

        for (size_t i = 0; i < indices.size(); i++) {
            size_t idx = indices[i];
            const FakeMsg &msg = _messages[idx];
            unsigned long seq = (unsigned long) (idx + 1);

            if (items == "UID" || items == "(UID)") {
                sendAll(fd, "* " + std::to_string(seq) + " FETCH (UID "
                        + std::to_string(msg.uid) + ")\r\n");
            } else if (items.find("HEADER") != std::string::npos && items.find("ENVELOPE") == std::string::npos) {
                std::string hdr = "From: " + msg.from + "\r\nTo: " + msg.to
                                  + "\r\nSubject: " + msg.subject + "\r\nDate: " + msg.date
                                  + "\r\nMessage-ID: " + msg.messageId + "\r\n\r\n";
                sendAll(fd, "* " + std::to_string(seq) + " FETCH (UID "
                        + std::to_string(msg.uid) + " BODY[HEADER] {"
                        + std::to_string(hdr.size()) + "}\r\n" + hdr + ")\r\n");
            } else if (items.find("TEXT") != std::string::npos) {
                sendAll(fd, "* " + std::to_string(seq) + " FETCH (UID "
                        + std::to_string(msg.uid) + " BODY[TEXT] {"
                        + std::to_string(msg.body.size()) + "}\r\n" + msg.body + ")\r\n");
            } else if ((items.find("BODY[]") != std::string::npos || items.find("RFC822") != std::string::npos)
                       && items.find("ENVELOPE") == std::string::npos) {
                std::string hdr = "From: " + msg.from + "\r\nTo: " + msg.to
                                  + "\r\nSubject: " + msg.subject + "\r\nDate: " + msg.date
                                  + "\r\nMessage-ID: " + msg.messageId + "\r\n\r\n";
                std::string full = hdr + msg.body;
                sendAll(fd, "* " + std::to_string(seq) + " FETCH (UID "
                        + std::to_string(msg.uid) + " BODY[] {"
                        + std::to_string(full.size()) + "}\r\n" + full + ")\r\n");
            } else if (items.find("ENVELOPE") != std::string::npos
                       || items.find("BODYSTRUCTURE") != std::string::npos
                       || items.find("FAST") != std::string::npos
                       || items.find("ALL") != std::string::npos
                       || items.find("FULL") != std::string::npos) {
                std::string flags = "(";
                if (msg.deleted) flags += "\\Deleted ";
                if (msg.seen) flags += "\\Seen ";
                if (msg.flagged) flags += "\\Flagged ";
                if (flags.size() > 1 && flags[flags.size() - 1] == ' ') {
                    flags.erase(flags.size() - 1);
                }
                flags += ")";

                std::string env = "(\"05-Jan-2026 10:00:00 +0000\" \"" + msg.subject
                                  + "\" ((NIL NIL \"test\" \"example.org\")) "
                                  "((NIL NIL \"test\" \"example.org\")) "
                                  "((NIL NIL \"test\" \"example.org\")) "
                                  "((NIL NIL \"me\" \"example.test\")) "
                                  "NIL NIL NIL \"" + msg.messageId + "\")";

                std::string bs = "(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"US-ASCII\") NIL NIL \"7BIT\" "
                                 + std::to_string(msg.body.size()) + " 2 NIL NIL NIL NIL)";

                std::string fullMsg = "From: " + msg.from + "\r\nTo: " + msg.to
                                      + "\r\nSubject: " + msg.subject + "\r\nDate: " + msg.date
                                      + "\r\nMessage-ID: " + msg.messageId + "\r\n\r\n" + msg.body;

                std::string resp = "* " + std::to_string(seq) + " FETCH (UID "
                                   + std::to_string(msg.uid) + " FLAGS " + flags
                                   + " INTERNALDATE \"05-Jan-2026 10:00:00 +0000\" RFC822.SIZE "
                                   + std::to_string(fullMsg.size()) + " ENVELOPE " + env
                                   + " BODYSTRUCTURE " + bs + ")\r\n";
                sendAll(fd, resp);
            } else {
                std::string flags = "(";
                if (msg.deleted) flags += "\\Deleted ";
                if (msg.seen) flags += "\\Seen ";
                if (msg.flagged) flags += "\\Flagged ";
                if (flags.size() > 1 && flags[flags.size() - 1] == ' ') {
                    flags.erase(flags.size() - 1);
                }
                flags += ")";
                sendAll(fd, "* " + std::to_string(seq) + " FETCH (FLAGS " + flags + ")\r\n");
            }
        }
        pthread_mutex_unlock(&_lock);

        sendAll(fd, tag + " OK " + (isUid ? "UID FETCH" : "FETCH") + " completed\r\n");
    }

    void handleStore(int fd, const std::string &tag, bool isUid, const std::string &storeArgs)
    {
        size_t sp1 = storeArgs.find(' ');
        std::string seqSpec = (sp1 != std::string::npos) ? storeArgs.substr(0, sp1) : storeArgs;
        std::string rest = (sp1 != std::string::npos) ? storeArgs.substr(sp1 + 1) : "";

        bool clearFlag = (rest.find('-') != std::string::npos);
        bool silent = (rest.find(".SILENT") != std::string::npos);

        pthread_mutex_lock(&_lock);
        std::vector<size_t> indices = matchMessages(seqSpec, isUid);
        for (size_t i = 0; i < indices.size(); i++) {
            size_t idx = indices[i];
            FakeMsg &msg = _messages[idx];
            if (clearFlag) {
                if (rest.find("\\DELETED") != std::string::npos || rest.find("\\Deleted") != std::string::npos) {
                    msg.deleted = false;
                }
                if (rest.find("\\SEEN") != std::string::npos || rest.find("\\Seen") != std::string::npos) {
                    msg.seen = false;
                }
            } else {
                if (rest.find("\\DELETED") != std::string::npos || rest.find("\\Deleted") != std::string::npos) {
                    msg.deleted = true;
                }
                if (rest.find("\\SEEN") != std::string::npos || rest.find("\\Seen") != std::string::npos) {
                    msg.seen = true;
                }
            }

            if (!silent) {
                std::string flags = "(";
                if (msg.deleted) flags += "\\Deleted ";
                if (msg.seen) flags += "\\Seen ";
                if (msg.flagged) flags += "\\Flagged ";
                if (flags.size() > 1 && flags[flags.size() - 1] == ' ') {
                    flags.erase(flags.size() - 1);
                }
                flags += ")";
                sendAll(fd, "* " + std::to_string(idx + 1) + " FETCH (FLAGS " + flags + ")\r\n");
            }
        }
        pthread_mutex_unlock(&_lock);

        sendAll(fd, tag + " OK " + (isUid ? "UID STORE" : "STORE") + " completed\r\n");
    }

    void serve(int fd)
    {
        if (_greetingMode == GreetingDrop) {
            return;
        }

        if (_greetingMode == GreetingRefuseBye) {
            sendAll(fd, "* BYE [UNAVAILABLE] Service unavailable\r\n");
            return;
        }

        /* Send preauth greeting */
        if (!sendAll(fd, "* PREAUTH [CAPABILITY IMAP4REV1 UIDPLUS] localhost ready\r\n")) {
            return;
        }

        std::string buf;
        std::string line;

        while (!_stop && readLine(fd, buf, line)) {
            if (line.empty()) {
                continue;
            }

            /* Tokenize tag and command */
            size_t sp1 = line.find(' ');
            std::string tag = (sp1 != std::string::npos) ? line.substr(0, sp1) : line;
            std::string rest = (sp1 != std::string::npos) ? line.substr(sp1 + 1) : "";

            size_t sp2 = rest.find(' ');
            std::string cmd = (sp2 != std::string::npos) ? rest.substr(0, sp2) : rest;
            std::string args = (sp2 != std::string::npos) ? rest.substr(sp2 + 1) : "";

            for (size_t i = 0; i < cmd.size(); i++) {
                if (cmd[i] >= 'a' && cmd[i] <= 'z') {
                    cmd[i] = (char) (cmd[i] - 'a' + 'A');
                }
            }

            if (cmd == "STATUS") {
                if (_statusMode == StatusDropDuring) {
                    return;
                }
                pthread_mutex_lock(&_lock);
                size_t nmsgs = _messages.size();
                unsigned long uv = _uidValidity;
                unsigned long un = _uidNext;
                pthread_mutex_unlock(&_lock);
                sendAll(fd, "* STATUS \"INBOX\" (MESSAGES " + std::to_string(nmsgs)
                        + " RECENT 0 UNSEEN 0 UIDVALIDITY " + std::to_string(uv)
                        + " UIDNEXT " + std::to_string(un) + ")\r\n");
                sendAll(fd, tag + " OK STATUS completed\r\n");
            } else if (cmd == "SELECT" || cmd == "EXAMINE") {
                pthread_mutex_lock(&_lock);
                size_t nmsgs = _messages.size();
                unsigned long uv = _uidValidity;
                unsigned long un = _uidNext;
                pthread_mutex_unlock(&_lock);
                sendAll(fd, "* " + std::to_string(nmsgs) + " EXISTS\r\n* 0 RECENT\r\n");
                sendAll(fd, "* OK [UIDVALIDITY " + std::to_string(uv) + "] UIDs valid\r\n");
                sendAll(fd, "* OK [UIDNEXT " + std::to_string(un) + "] Predicted next UID\r\n");
                sendAll(fd, "* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n");
                sendAll(fd, "* OK [PERMANENTFLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft \\*)] Limited\r\n");
                sendAll(fd, tag + " OK [READ-WRITE] " + cmd + " completed\r\n");
            } else if (cmd == "CAPABILITY") {
                sendAll(fd, "* CAPABILITY IMAP4REV1 UIDPLUS\r\n");
                sendAll(fd, tag + " OK CAPABILITY completed\r\n");
            } else if (cmd == "NOOP") {
                sendAll(fd, tag + " OK NOOP completed\r\n");
            } else if (cmd == "CHECK") {
                sendAll(fd, tag + " OK CHECK completed\r\n");
            } else if (cmd == "CLOSE") {
                sendAll(fd, tag + " OK CLOSE completed\r\n");
            } else if (cmd == "EXPUNGE") {
                sendAll(fd, tag + " OK EXPUNGE completed\r\n");
            } else if (cmd == "LOGOUT") {
                sendAll(fd, "* BYE Logging out\r\n");
                sendAll(fd, tag + " OK LOGOUT completed\r\n");
                return;
            } else if (cmd == "SEARCH" || (cmd == "UID" && args.substr(0, 6) == "SEARCH")) {
                bool isUid = (cmd == "UID");
                pthread_mutex_lock(&_lock);
                std::string res = "* SEARCH";
                for (size_t i = 0; i < _messages.size(); i++) {
                    if (isUid) {
                        res += " " + std::to_string(_messages[i].uid);
                    } else {
                        res += " " + std::to_string(i + 1);
                    }
                }
                pthread_mutex_unlock(&_lock);
                res += "\r\n";
                sendAll(fd, res);
                sendAll(fd, tag + " OK " + (isUid ? "UID SEARCH" : "SEARCH") + " completed\r\n");
            } else if (cmd == "FETCH" || (cmd == "UID" && args.substr(0, 5) == "FETCH")) {
                handleFetch(fd, tag, cmd == "UID", (cmd == "UID") ? args.substr(6) : args);
            } else if (cmd == "STORE" || (cmd == "UID" && args.substr(0, 5) == "STORE")) {
                handleStore(fd, tag, cmd == "UID", (cmd == "UID") ? args.substr(6) : args);
            } else {
                sendAll(fd, tag + " OK " + cmd + " completed\r\n");
            }
        }
    }

    int _listenFd;
    int _port;
    pthread_t _thread;
    bool _started;
    volatile bool _stop;
    GreetingMode _greetingMode;
    StatusMode _statusMode;
    pthread_mutex_t _lock;
    int _connectionCount;
    unsigned long _uidValidity;
    unsigned long _uidNext;
    std::vector<FakeMsg> _messages;
    std::vector<int> _activeClients;
    std::vector<pthread_t> _clientThreads;
};

int findAlpineClientSocket(pid_t alpinePid, int targetPort)
{
    int pidfd = (int) syscall(SYS_pidfd_open, alpinePid, 0);
    if (pidfd < 0) {
        return -1;
    }

    std::string fdDir = "/proc/" + std::to_string(alpinePid) + "/fd";
    DIR *dir = opendir(fdDir.c_str());
    if (!dir) {
        close(pidfd);
        return -1;
    }

    int foundFd = -1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        int fdNum = atoi(ent->d_name);
        if (fdNum < 0) {
            continue;
        }

        int dupFd = (int) syscall(SYS_pidfd_getfd, pidfd, fdNum, 0);
        if (dupFd < 0) {
            continue;
        }

        struct sockaddr_in peer;
        socklen_t peerLen = sizeof(peer);
        if (getpeername(dupFd, (struct sockaddr *) &peer, &peerLen) == 0 &&
            peer.sin_family == AF_INET &&
            ntohs(peer.sin_port) == targetPort) {
            foundFd = dupFd;
            break;
        }
        close(dupFd);
    }

    closedir(dir);
    close(pidfd);
    return foundFd;
}

} /* anonymous namespace */

TEST_GROUP(ImapResilienceIntegration)
{
    ScopedThreadSafeTracking threadSafe;
    Rig *rig;
    FakeImapServer imap;

    void setup()
    {
        rig = new Rig();
        CHECK(imap.start());
    }

    void teardown()
    {
        rig->c.closeNow();
        rig->a.stop();
        imap.stop();
        delete rig;
    }
};

TEST(ImapResilienceIntegration, ClientSocketEnablesTcpKeepaliveWithCustomTimers)
{
    std::string remoteMailbox = "{127.0.0.1:" + std::to_string(imap.port())
                                + "/imap/notls}INBOX";

    rig->a.addIncomingFolder("Remote", remoteMailbox);
    CHECK(rig->up());

    Json folders(rig->callOk("mail_list_folders", "{}"));
    CHECK(folders.ok());

    /* Trigger connection to fake IMAP server via mail_folder_status */
    Json status(rig->callOk("mail_folder_status",
                            Rig::folderArgs(remoteMailbox)));
    CHECK(status.ok());

    int clientFd = findAlpineClientSocket(rig->a.pid(), imap.port());
    CHECK(clientFd >= 0);

    int keepalive = 0;
    socklen_t optlen = sizeof(keepalive);
    int rc = getsockopt(clientFd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, &optlen);
    CHECK_EQUAL(0, rc);
    CHECK_EQUAL(1, keepalive);

    int keepidle = 0;
    optlen = sizeof(keepidle);
    rc = getsockopt(clientFd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, &optlen);
    CHECK_EQUAL(0, rc);
    CHECK_EQUAL(45, keepidle);

    int keepintvl = 0;
    optlen = sizeof(keepintvl);
    rc = getsockopt(clientFd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, &optlen);
    CHECK_EQUAL(0, rc);
    CHECK_EQUAL(10, keepintvl);

    int keepcnt = 0;
    optlen = sizeof(keepcnt);
    rc = getsockopt(clientFd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, &optlen);
    CHECK_EQUAL(0, rc);
    CHECK_EQUAL(3, keepcnt);

    close(clientFd);
}

TEST(ImapResilienceIntegration, FaultInjectionGreetingRefusalHandledGracefully)
{
    imap.setGreetingMode(FakeImapServer::GreetingRefuseBye);

    std::string remoteMailbox = "{127.0.0.1:" + std::to_string(imap.port())
                                + "/imap/notls}INBOX";

    rig->a.addIncomingFolder("Remote", remoteMailbox);
    CHECK(rig->up());

    Json folders(rig->callOk("mail_list_folders", "{}"));
    CHECK(folders.ok());

    /* Calling folder status on refusing server should fail cleanly, not crash */
    cJSON *res = rig->callOk("mail_folder_status",
                             Rig::folderArgs(remoteMailbox));
    CHECK(res == NULL);

    /* Alpine must still be running healthy */
    CHECK(rig->a.running());
}

TEST(ImapResilienceIntegration, FaultInjectionDisconnectDuringStatusHandledGracefully)
{
    imap.setStatusMode(FakeImapServer::StatusDropDuring);

    std::string remoteMailbox = "{127.0.0.1:" + std::to_string(imap.port())
                                + "/imap/notls}INBOX";

    rig->a.addIncomingFolder("Remote", remoteMailbox);
    CHECK(rig->up());

    Json folders(rig->callOk("mail_list_folders", "{}"));
    CHECK(folders.ok());

    /* Calling folder status when server drops mid-STATUS should fail cleanly */
    cJSON *res = rig->callOk("mail_folder_status",
                             Rig::folderArgs(remoteMailbox));
    CHECK(res == NULL);

    /* Alpine must still be running healthy */
    CHECK(rig->a.running());
}

TEST(ImapResilienceIntegration, Tier1InPlaceReconnectOnServerDisconnect)
{
    imap.setUidValidity(100);
    imap.addMessage(FakeImapServer::FakeMsg(10, "AlphaSubject"));
    imap.addMessage(FakeImapServer::FakeMsg(20, "BetaSubject"));
    imap.addMessage(FakeImapServer::FakeMsg(30, "GammaSubject"));

    std::string remoteMailbox = "{127.0.0.1:" + std::to_string(imap.port())
                                + "/imap/notls}INBOX";

    rig->a.setInboxPath(remoteMailbox);
    CHECK(rig->up());

    Json folders(rig->callOk("mail_list_folders", "{}"));
    CHECK(folders.ok());

    /* Enter MESSAGE INDEX */
    rig->a.sendKeys("i");
    CHECK(rig->a.waitForText("AlphaSubject", 3000));

    /* Verify initial connection is open and MCP mail_get_message for UID 10 succeeds */
    Json m1(rig->callOk("mail_get_message", Rig::folderArgs(remoteMailbox,
        ",\"uid\":10,\"uidvalidity\":100")));
    CHECK(m1.ok());

    /* Drop the active IMAP socket */
    imap.disconnectAllClientsWithBye();

    /* Verify MCP recovery: mail_get_message for UID 20 succeeds and returns "BetaSubject" */
    Json m2(rig->callOk("mail_get_message", Rig::folderArgs(remoteMailbox,
        ",\"uid\":20,\"uidvalidity\":100")));
    CHECK(m2.ok());
    cJSON *hdr2 = m2.at("headers");
    CHECK(hdr2 != NULL);
    cJSON *sub2 = cJSON_GetObjectItemCaseSensitive(hdr2, "subject");
    CHECK(sub2 != NULL && sub2->valuestring != NULL);
    STRCMP_EQUAL("BetaSubject", sub2->valuestring);

    /* Drop active clients again, then trigger Ctrl-L (\x0c) in the TUI */
    imap.disconnectAllClientsWithBye();

    size_t m = rig->a.mark();
    rig->a.sendKeys("\x0c");
    CHECK(rig->a.waitForText("Folder connection restored.", 5000, m));
    CHECK(rig->a.staysAbsent("CLOSED DUE TO ACCESS ERROR", 1000, m));
    CHECK(rig->a.running());
}

TEST(ImapResilienceIntegration, Tier2FullReopenAndCursorRestoreAfterExpungeDuringDisconnect)
{
    imap.setUidValidity(100);
    imap.addMessage(FakeImapServer::FakeMsg(10, "AlphaSubject"));
    imap.addMessage(FakeImapServer::FakeMsg(20, "BetaSubject"));
    imap.addMessage(FakeImapServer::FakeMsg(30, "GammaSubject"));

    std::string remoteMailbox = "{127.0.0.1:" + std::to_string(imap.port())
                                + "/imap/notls}INBOX";

    rig->a.setInboxPath(remoteMailbox);
    CHECK(rig->up());

    /* Enter MESSAGE INDEX */
    rig->a.sendKeys("i");

    /* Wait for Alpine to paint the folder index */
    CHECK(rig->a.waitForText("AlphaSubject", 3000));
    CHECK(rig->a.waitForText("GammaSubject", 3000));

    /* Move the TUI cursor to message 3 ("GammaSubject") */
    rig->a.sendKeys("nn");

    /* Simulate server-side expunge while disconnected: UID 10 is expunged */
    imap.expungeMessageByUid(10);
    imap.disconnectAllClientsWithBye();

    /* Trigger new_mail(1, ...) and mail_index() auto-recovery */
    size_t m = rig->a.mark();
    rig->a.sendKeys("\x0c");

    CHECK(rig->a.waitForText("Folder reconnected and synchronized.", 5000, m));
    CHECK(rig->a.staysAbsent("CLOSED DUE TO ACCESS ERROR", 1000, m));

    /* Press Enter (\r) to view the current message */
    m = rig->a.mark();
    rig->a.sendKeys("\r");
    CHECK(rig->a.waitForText("GammaSubject", 3000, m));
    CHECK(rig->a.running());
}

TEST(ImapResilienceIntegration, FaultInjectionOfflinePreservesCachedIndexAndRateLimitsRetries)
{
    imap.setUidValidity(100);
    imap.addMessage(FakeImapServer::FakeMsg(10, "AlphaSubject"));
    imap.addMessage(FakeImapServer::FakeMsg(20, "BetaSubject"));
    imap.addMessage(FakeImapServer::FakeMsg(30, "GammaSubject"));

    std::string remoteMailbox = "{127.0.0.1:" + std::to_string(imap.port())
                                + "/imap/notls}INBOX";

    rig->a.setInboxPath(remoteMailbox);
    CHECK(rig->up());

    /* Enter MESSAGE INDEX */
    rig->a.sendKeys("i");
    CHECK(rig->a.waitForText("AlphaSubject", 3000));

    /* Stop the IMAP server completely, making port unreachable */
    imap.stop();

    /* Trigger ping and auto-recovery attempt */
    size_t m = rig->a.mark();
    rig->a.sendKeys("\x0c");

    /* Server is unreachable, so CLOSED DUE TO ACCESS ERROR appears */
    CHECK(rig->a.waitForText("CLOSED DUE TO ACCESS ERROR", 5000, m));
    CHECK(rig->a.waitForText("AlphaSubject", 1000, m));

    int connsAfterFirst = imap.connectionCount();

    /* Send Ctrl-L again immediately; rate limit must prevent tight reconnect storm */
    m = rig->a.mark();
    rig->a.sendKeys("\x0c");
    CHECK(rig->a.running());
    CHECK_EQUAL(connsAfterFirst, imap.connectionCount());
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
