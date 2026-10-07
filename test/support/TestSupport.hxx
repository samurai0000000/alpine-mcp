/*
 * TestSupport.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_SUPPORT_HXX
#define ALPINE_MCP_TEST_SUPPORT_HXX

#include <string>
#include <sys/types.h>

/*
 * A directory under /tmp that is removed, with everything in it, when the
 * object goes away.  The name is short on purpose: a Unix socket path in
 * it must stay under the 108 byte limit.
 */
class TempDir
{
public:
    TempDir();
    ~TempDir();

    const std::string &path() const { return _path; }
    std::string child(const std::string &name) const;

private:
    TempDir(const TempDir &);
    TempDir &operator=(const TempDir &);

    std::string _path;
};

/* Sets an environment variable and puts the old value back afterwards. */
class EnvGuard
{
public:
    EnvGuard(const char *name, const char *value);
    ~EnvGuard();

private:
    EnvGuard(const EnvGuard &);
    EnvGuard &operator=(const EnvGuard &);

    std::string _name;
    std::string _old;
    bool _had;
};

/*
 * CppUTest's default leak tracking is not thread-safe.  A test that starts
 * threads holds one of these for its whole body: new and delete then use
 * CppUTest's thread-safe tracking (leaks are still found) until it goes out
 * of scope.
 */
class ScopedThreadSafeTracking
{
public:
    ScopedThreadSafeTracking();
    ~ScopedThreadSafeTracking();

private:
    ScopedThreadSafeTracking(const ScopedThreadSafeTracking &);
    ScopedThreadSafeTracking &operator=(const ScopedThreadSafeTracking &);
};

/* File helpers for the tests. */
bool writeFile(const std::string &path, const std::string &content,
               mode_t mode);
std::string readFile(const std::string &path);
bool fileExists(const std::string &path);
int fileMode(const std::string &path);          /* permission bits, or -1 */
long countFiles(const std::string &dir);        /* entries but . and .. */
bool makeDir(const std::string &path, mode_t mode);

/* Removes a directory tree. */
void removeTree(const std::string &path);

#endif /* ALPINE_MCP_TEST_SUPPORT_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
