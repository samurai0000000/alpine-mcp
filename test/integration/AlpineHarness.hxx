/*
 * AlpineHarness.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_ALPINE_HARNESS_HXX
#define ALPINE_MCP_TEST_ALPINE_HARNESS_HXX

#include <pthread.h>
#include <string>
#include <sys/types.h>
#include <vector>

#include "TestSupport.hxx"

/*
 * A real Alpine, the binary the top-level Makefile builds, running on a
 * pseudo terminal with a home directory of its own: a pinerc, the MCP
 * policy file, local mbox folders and an address book.  Nothing of the
 * user's home is touched.  The tests drive it with keys, as a person
 * would, and with MCP calls on its socket.
 *
 * The binary is $ALPINE_MCP_ALPINE, or build/alpine/alpine/alpine below
 * the working directory (the repository root, where make runs the tests).
 */
class AlpineHarness
{
public:
    AlpineHarness();
    ~AlpineHarness();

    /* ---- before start() ---- */

    /* config.json text; the default is MCP on, no lock, ok@example.org
       allowed, aimon off */
    void useConfig(const std::string &json, int mode = 0600);

    /* a mbox file under the home's mail directory, e.g. "work" */
    void addMbox(const std::string &name, const std::string &mboxText);

    /* a line of the address book: nickname, name, address */
    void addContact(const std::string &nick, const std::string &name,
                    const std::string &address);

    /* an incoming folder: nickname and folder specification */
    void addIncomingFolder(const std::string &name, const std::string &path);

    /* an additional feature-list entry */
    void addFeature(const std::string &feat);

    /* serve MCP from the runtime directory of another harness, so that
       the two Alpines contend for one socket */
    void shareRuntimeWith(const AlpineHarness &other);

    void setSmtpPort(int port) { _smtpPort = port; }
    void setLocale(const std::string &lang) { _lang = lang; }
    void setInboxPath(const std::string &path) { _inboxPath = path; }

    /* the four messages the tests start from, as mbox text */
    static std::string standardInbox();

    /* ---- running ---- */

    /* forks Alpine and waits for the folder to open and, when expectSocket
       is set, for its MCP socket */
    bool start(int timeoutMs = 20000, bool expectSocket = true);

    /* quits with the keys a person would use; true if Alpine ended by
       itself, false if it had to be killed */
    bool stop(int timeoutMs = 8000);
    bool running();
    pid_t pid() const { return _pid; }
    bool waitForExit(int timeoutMs);
    int exitStatus() const { return _status; }

    /* kills it without a goodbye, for the tests of an abrupt end */
    void killNow();

    std::string socketPath() const;
    std::string home() const { return _tmp.path(); }
    std::string mailPath(const std::string &name) const;
    std::string idempotencyDir() const;

    /* ---- the terminal ---- */

    void sendKeys(const std::string &keys);

    /* what Alpine has written so far; a mark is a position in it */
    size_t mark();
    std::string since(size_t from);
    bool waitForText(const std::string &text, int timeoutMs, size_t from = 0);

    /* true if the text does not appear within the time */
    bool staysAbsent(const std::string &text, int quietMs, size_t from);

private:
    AlpineHarness(const AlpineHarness &);
    AlpineHarness &operator=(const AlpineHarness &);

    static void *readerMain(void *arg);
    void writePinerc();

    TempDir _tmp;
    std::string _runDir;
    int _smtpPort;
    std::string _lang;
    pid_t _pid;
    int _fd;
    int _status;
    bool _exited;
    bool _readerOn;
    pthread_t _reader;
    pthread_mutex_t _lock;
    std::string _out;
    std::string _inboxPath;
    std::vector<std::string> _extraIncomingFolders;
    std::vector<std::string> _extraFeatures;
};

#endif /* ALPINE_MCP_TEST_ALPINE_HARNESS_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
