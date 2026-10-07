/*
 * SocketPathTest.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */


#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "TestSupport.hxx"

extern "C" {
int mcp_build_socket_path(char *, size_t);
}

/* CppUTest after the standard headers: its new macros break them otherwise */
#include "CppUTest/TestHarness.h"

/*
 * The socket lives in $XDG_RUNTIME_DIR when that is an absolute, usable
 * directory of ours, otherwise in /tmp with the host name in it.  The
 * result must fit sockaddr_un.sun_path.
 */

TEST_GROUP(SocketPath)
{
    TempDir *tmp;

    void setup()
    {
        tmp = new TempDir();
    }

    void teardown()
    {
        delete tmp;
    }
};

TEST(SocketPath, UsesTheRuntimeDirectoryWhenItIsUsable)
{
    EnvGuard xdg("XDG_RUNTIME_DIR", tmp->path().c_str());
    char buf[256];

    LONGS_EQUAL(0, mcp_build_socket_path(buf, sizeof(buf)));
    STRCMP_EQUAL((tmp->path() + "/alpine-mcp.sock").c_str(), buf);
}

TEST(SocketPath, FallsBackToTmpWithTheHostNameWhenUnset)
{
    EnvGuard xdg("XDG_RUNTIME_DIR", NULL);
    char buf[256];

    LONGS_EQUAL(0, mcp_build_socket_path(buf, sizeof(buf)));
    STRNCMP_EQUAL("/tmp/alpine-mcp.", buf, 16);
    CHECK(strstr(buf, ".sock") != NULL);
    CHECK(strchr(buf + 5, '/') == NULL);        /* one path part after /tmp/ */
}

TEST(SocketPath, IgnoresARelativeRuntimeDirectory)
{
    EnvGuard xdg("XDG_RUNTIME_DIR", "relative/dir");
    char buf[256];

    LONGS_EQUAL(0, mcp_build_socket_path(buf, sizeof(buf)));
    STRNCMP_EQUAL("/tmp/alpine-mcp.", buf, 16);
}

TEST(SocketPath, IgnoresARuntimeDirectoryThatDoesNotExist)
{
    EnvGuard xdg("XDG_RUNTIME_DIR", (tmp->path() + "/missing").c_str());
    char buf[256];

    LONGS_EQUAL(0, mcp_build_socket_path(buf, sizeof(buf)));
    STRNCMP_EQUAL("/tmp/alpine-mcp.", buf, 16);
}

TEST(SocketPath, IgnoresARuntimeDirectoryThatIsAFile)
{
    CHECK(writeFile(tmp->child("afile"), "x", 0600));
    EnvGuard xdg("XDG_RUNTIME_DIR", tmp->child("afile").c_str());
    char buf[256];

    LONGS_EQUAL(0, mcp_build_socket_path(buf, sizeof(buf)));
    STRNCMP_EQUAL("/tmp/alpine-mcp.", buf, 16);
}

TEST(SocketPath, IgnoresARuntimeDirectoryWeCannotWriteTo)
{
    if (geteuid() == 0) {
        return;                     /* root can write anywhere */
    }

    CHECK(makeDir(tmp->child("ro"), 0500));
    EnvGuard xdg("XDG_RUNTIME_DIR", tmp->child("ro").c_str());
    char buf[256];

    LONGS_EQUAL(0, mcp_build_socket_path(buf, sizeof(buf)));
    STRNCMP_EQUAL("/tmp/alpine-mcp.", buf, 16);
    chmod(tmp->child("ro").c_str(), 0700);
}

TEST(SocketPath, RefusesAPathLongerThanSunPath)
{
    /* sockaddr_un.sun_path holds 108 bytes including the NUL */
    std::string base = tmp->child("d");

    CHECK(makeDir(base, 0700));
    std::string dir = base;

    while (dir.size() + std::string("/alpine-mcp.sock").size() < 107) {
        dir += "/x";
    }

    std::string deep = base;

    while (deep.size() < 120) {
        deep += "/xxxxxxxxxx";
        CHECK(makeDir(deep, 0700));
    }

    EnvGuard xdg("XDG_RUNTIME_DIR", deep.c_str());
    char buf[512];

    LONGS_EQUAL(-1, mcp_build_socket_path(buf, sizeof(buf)));
    STRCMP_EQUAL("", buf);
}

TEST(SocketPath, PathOfExactlyOneHundredSevenBytesFits)
{
    std::string base = tmp->path();
    const std::string tail = "/alpine-mcp.sock";
    std::string dir = base;

    /* grow the directory name until the result is exactly 107 characters */
    int guard = 0;

    while (dir.size() + tail.size() < 107 && guard++ < 20) {
        size_t room = 107 - (dir.size() + tail.size());
        std::string part = "/" + std::string(room > 1 ? (room - 1 > 30 ? 30 : room - 1) : 0, 'p');

        if (part.size() == 1) {
            break;
        }

        dir += part;
        CHECK(makeDir(dir, 0700));
    }

    if (dir.size() + tail.size() == 107) {
        EnvGuard xdg("XDG_RUNTIME_DIR", dir.c_str());
        char buf[256];

        LONGS_EQUAL(0, mcp_build_socket_path(buf, sizeof(buf)));
        LONGS_EQUAL(107, strlen(buf));
    }
}

TEST(SocketPath, BufferTooSmallIsAnErrorAndNullIsRefused)
{
    EnvGuard xdg("XDG_RUNTIME_DIR", tmp->path().c_str());
    char buf[8];

    LONGS_EQUAL(-1, mcp_build_socket_path(buf, sizeof(buf)));
    LONGS_EQUAL(-1, mcp_build_socket_path(NULL, 100));
    LONGS_EQUAL(-1, mcp_build_socket_path(buf, 0));
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
