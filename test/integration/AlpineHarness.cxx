/*
 * AlpineHarness.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "AlpineHarness.hxx"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

namespace
{
    long long
    nowMs()
    {
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    void
    sleepMs(int ms)
    {
        struct timespec ts;

        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long) (ms % 1000) * 1000000;
        nanosleep(&ts, NULL);
    }
}

AlpineHarness::AlpineHarness()
    : _smtpPort(0), _lang("C"), _pid(-1), _fd(-1), _status(0),
      _exited(false), _readerOn(false)
{
    pthread_mutex_init(&_lock, NULL);
    makeDir(_tmp.child("mail"), 0700);
    _runDir = _tmp.child("run");
    makeDir(_runDir, 0700);
    makeDir(_tmp.child(".config"), 0700);
    makeDir(_tmp.child(".config/alpine-mcp"), 0700);
    useConfig("{\"mcp\":{\"enabled\":true},\"operator_lock\":false,"
              "\"allowed_recipients\":[\"ok@example.org\"],"
              "\"aimon\":{\"enabled\":false}}");
    addMbox("INBOX", standardInbox());

    /* the folders Alpine files into; empty mailboxes to begin with */
    addMbox("sent-mail", "");
    addMbox("saved-messages", "");
    addMbox("postponed-msgs", "");
}

AlpineHarness::~AlpineHarness()
{
    if (_pid > 0 && running()) {
        stop();
    }

    if (_pid > 0 && running()) {
        killNow();
    }

    if (_fd >= 0) {
        close(_fd);
    }

    if (_readerOn) {
        pthread_join(_reader, NULL);
    }

    pthread_mutex_destroy(&_lock);
}

void
AlpineHarness::useConfig(const std::string &json, int mode)
{
    std::string path = _tmp.child(".config/alpine-mcp/config.json");

    writeFile(path, json, (mode_t) mode);
    chmod(path.c_str(), (mode_t) mode);
}

void
AlpineHarness::addMbox(const std::string &name, const std::string &mboxText)
{
    writeFile(_tmp.child("mail/" + name), mboxText, 0600);
}

void
AlpineHarness::addContact(const std::string &nick, const std::string &name,
                          const std::string &address)
{
    std::string path = _tmp.child(".addressbook");

    writeFile(path, readFile(path) + nick + "\t" + name + "\t" + address
              + "\t\t\n", 0600);
}

void
AlpineHarness::addIncomingFolder(const std::string &name, const std::string &path)
{
    _extraIncomingFolders.push_back(name + " " + path);
}

std::string
AlpineHarness::standardInbox()
{
    return
        "From a@example.org Mon Jan  5 10:00:00 2026\n"
        "From: Ann Example <a@example.org>\n"
        "To: me@example.test\n"
        "Subject: first plain\n"
        "Date: Mon, 05 Jan 2026 10:00:00 +0000\n"
        "Message-ID: <m1@example.org>\n"
        "MIME-Version: 1.0\n"
        "Content-Type: text/plain; charset=us-ascii\n"
        "\n"
        "Hello one.\n"
        "Second line.\n"
        "\n"
        "From b@example.org Tue Jan  6 11:00:00 2026\n"
        "From: Bob Example <b@example.org>\n"
        "To: me@example.test\n"
        "Subject: with attachment\n"
        "Date: Tue, 06 Jan 2026 11:00:00 +0000\n"
        "Message-ID: <m2@example.org>\n"
        "MIME-Version: 1.0\n"
        "Content-Type: multipart/mixed; boundary=\"BND\"\n"
        "\n"
        "--BND\n"
        "Content-Type: text/plain; charset=us-ascii\n"
        "\n"
        "Body with an attachment.\n"
        "--BND\n"
        "Content-Type: text/plain; name=\"report.txt\"\n"
        "Content-Disposition: attachment; filename=\"report.txt\"\n"
        "\n"
        "attached text of 31 bytes......\n"
        "--BND--\n"
        "\n"
        "From c@example.org Wed Jan  7 12:00:00 2026\n"
        "From: =?UTF-8?Q?Caf=C3=A9_Owner?= <c@example.org>\n"
        "To: me@example.test\n"
        "Subject: =?UTF-8?Q?caf=C3=A9_menu?=\n"
        "Date: Wed, 07 Jan 2026 12:00:00 +0000\n"
        "Message-ID: <m3@example.org>\n"
        "MIME-Version: 1.0\n"
        "Content-Type: text/plain; charset=utf-8\n"
        "Content-Transfer-Encoding: quoted-printable\n"
        "\n"
        "caf=C3=A9 =C3=BCn=C3=AFcode\n"
        "\n"
        "From d@example.org Thu Jan  8 13:00:00 2026\n"
        "From: Dee Example <d@example.org>\n"
        "To: me@example.test\n"
        "Subject: html only\n"
        "Date: Thu, 08 Jan 2026 13:00:00 +0000\n"
        "Message-ID: <m4@example.org>\n"
        "MIME-Version: 1.0\n"
        "Content-Type: text/html; charset=us-ascii\n"
        "\n"
        "<html><body><p>Only html here.</p></body></html>\n"
        "\n";
}

void
AlpineHarness::shareRuntimeWith(const AlpineHarness &other)
{
    _runDir = other._runDir;
}

std::string
AlpineHarness::mailPath(const std::string &name) const
{
    return _tmp.child("mail/" + name);
}

std::string
AlpineHarness::socketPath() const
{
    return _runDir + "/alpine-mcp.sock";
}

std::string
AlpineHarness::idempotencyDir() const
{
    return _tmp.child(".local/state/alpine-mcp/idempotency");
}

void
AlpineHarness::writePinerc()
{
    std::string inc = "Work " + mailPath("work") + ", Primary INBOX";

    for (size_t i = 0; i < _extraIncomingFolders.size(); i++) {
        inc += ", " + _extraIncomingFolders[i];
    }

    std::string inbox = _inboxPath.empty() ? mailPath("INBOX") : _inboxPath;

    std::string rc =
        "personal-name=Test User\n"
        "user-domain=example.test\n"
        "last-version-used=6.26\n"
        "inbox-path=" + inbox + "\n"
        "folder-collections=Local " + _tmp.child("mail") + "/[]\n"
        "default-fcc=sent-mail\n"
        "incoming-folders=" + inc + "\n"
        "feature-list=quell-status-message-beeping,expunge-only-manually\n";

    if (_smtpPort > 0) {
        rc += "smtp-server=127.0.0.1:" + std::to_string(_smtpPort) + "/notls\n";
    }

    writeFile(_tmp.child("pinerc"), rc, 0600);
}

void *
AlpineHarness::readerMain(void *arg)
{
    AlpineHarness *self = static_cast<AlpineHarness *>(arg);
    char buf[8192];

    for (;;) {
        ssize_t n = read(self->_fd, buf, sizeof(buf));

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            break;      /* the child closed the terminal */
        }

        pthread_mutex_lock(&self->_lock);
        self->_out.append(buf, (size_t) n);
        pthread_mutex_unlock(&self->_lock);
    }

    return NULL;
}

bool
AlpineHarness::start(int timeoutMs, bool expectSocket)
{
    const char *bin = getenv("ALPINE_MCP_ALPINE");
    std::string binary = bin != NULL ? bin : "build/alpine/alpine/alpine";
    std::string home = _tmp.path();
    std::string rc = _tmp.child("pinerc");
    struct winsize ws;
    int master = -1;

    /* a second start on the same home: let go of the first process */
    if (_readerOn) {
        pthread_join(_reader, NULL);
        _readerOn = false;
    }

    if (_fd >= 0) {
        close(_fd);
        _fd = -1;
    }

    pthread_mutex_lock(&_lock);
    _out.clear();
    pthread_mutex_unlock(&_lock);
    _exited = false;
    _status = 0;

    writePinerc();
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 24;
    ws.ws_col = 80;
    _pid = forkpty(&master, NULL, NULL, &ws);

    if (_pid < 0) {
        return false;
    }

    if (_pid == 0) {
        /* Alpine must hold none of the test's descriptors: a copy of the
           SMTP sink's listening socket would keep the port answering after
           the sink is stopped, and copies of client sockets would keep
           connections open after the test closed them */
        long maxFd = sysconf(_SC_OPEN_MAX);

        for (long fd = 3; fd < (maxFd > 65536 ? 65536 : maxFd); fd++) {
            close((int) fd);
        }

        setenv("HOME", home.c_str(), 1);
        setenv("XDG_RUNTIME_DIR", _runDir.c_str(), 1);
        unsetenv("XDG_STATE_HOME");
        unsetenv("XDG_CONFIG_HOME");
        setenv("TERM", "xterm", 1);
        setenv("LANG", _lang.c_str(), 1);
        unsetenv("LC_ALL");
        unsetenv("DISPLAY");

        if (chdir(home.c_str()) != 0) {
            _exit(126);
        }

        char *argv[7];

        argv[0] = const_cast<char *>("alpine");
        argv[1] = const_cast<char *>("-d");
        argv[2] = const_cast<char *>("0");
        argv[3] = const_cast<char *>("-p");
        argv[4] = const_cast<char *>(rc.c_str());
        argv[5] = NULL;
        argv[6] = NULL;
        execv(binary.c_str(), argv);
        _exit(127);
    }

    _fd = master;

    if (pthread_create(&_reader, NULL, readerMain, this) != 0) {
        return false;
    }

    _readerOn = true;
    long long end = nowMs() + timeoutMs;

    while (nowMs() < end) {
        struct stat st;

        /* the folder is open, or Alpine says why it has no socket */
        if ((!expectSocket || stat(socketPath().c_str(), &st) == 0)
            && (waitForText("Folder: INBOX", 100)
                || (!expectSocket
                    && waitForText("MCP socket not opened", 100)))) {
            return true;
        }

        if (!running()) {
            break;
        }
    }

    /* a start that fails should say what Alpine had on its screen */
    std::string out = since(0);

    fprintf(stderr, "[harness] Alpine did not come up; screen ended with:\n%s\n",
            out.substr(out.size() > 800 ? out.size() - 800 : 0).c_str());
    return false;
}

bool
AlpineHarness::running()
{
    if (_pid <= 0 || _exited) {
        return false;
    }

    int st = 0;
    pid_t r = waitpid(_pid, &st, WNOHANG);

    if (r == _pid) {
        _exited = true;
        _status = st;
        return false;
    }

    return true;
}

bool
AlpineHarness::waitForExit(int timeoutMs)
{
    long long end = nowMs() + timeoutMs;

    while (nowMs() < end) {
        if (!running()) {
            return true;
        }

        sleepMs(50);
    }

    return !running();
}

bool
AlpineHarness::stop(int timeoutMs)
{
    if (!running()) {
        return true;
    }

    /* a status message on the screen can swallow a key, so ask again
       until the quit question is there */
    for (int tries = 0; tries < 6; tries++) {
        size_t m = mark();

        sendKeys("q");

        if (waitForText("Really quit", 1500, m)) {
            sendKeys("y");
            break;
        }
    }

    if (waitForText("Expunge", 500)) {
        sendKeys("n");
    }

    if (waitForExit(timeoutMs)) {
        return true;
    }

    killNow();
    return false;
}

void
AlpineHarness::killNow()
{
    if (_pid > 0 && running()) {
        kill(_pid, SIGKILL);
        waitForExit(3000);
    }
}

void
AlpineHarness::sendKeys(const std::string &keys)
{
    size_t off = 0;

    while (off < keys.size() && _fd >= 0) {
        ssize_t n = write(_fd, keys.data() + off, keys.size() - off);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            break;
        }

        off += (size_t) n;
    }
}

size_t
AlpineHarness::mark()
{
    pthread_mutex_lock(&_lock);
    size_t n = _out.size();

    pthread_mutex_unlock(&_lock);
    return n;
}

std::string
AlpineHarness::since(size_t from)
{
    pthread_mutex_lock(&_lock);
    std::string s = from < _out.size() ? _out.substr(from) : std::string();

    pthread_mutex_unlock(&_lock);
    return s;
}

bool
AlpineHarness::waitForText(const std::string &text, int timeoutMs, size_t from)
{
    long long end = nowMs() + timeoutMs;

    do {
        if (since(from).find(text) != std::string::npos) {
            return true;
        }

        sleepMs(25);
    } while (nowMs() < end);

    return false;
}

bool
AlpineHarness::staysAbsent(const std::string &text, int quietMs, size_t from)
{
    sleepMs(quietMs);
    return since(from).find(text) == std::string::npos;
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
