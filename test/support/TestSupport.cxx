/*
 * TestSupport.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TestSupport.hxx"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* CppUTest after the standard headers: its new macros break them otherwise */
#include "CppUTest/TestHarness.h"
#include "CppUTest/MemoryLeakWarningPlugin.h"

ScopedThreadSafeTracking::ScopedThreadSafeTracking()
{
    MemoryLeakWarningPlugin::turnOnThreadSafeNewDeleteOverloads();
}

ScopedThreadSafeTracking::~ScopedThreadSafeTracking()
{
    MemoryLeakWarningPlugin::turnOnDefaultNotThreadSafeNewDeleteOverloads();
}

TempDir::TempDir()
{
    char tmpl[] = "/tmp/amcp.XXXXXX";

    if (mkdtemp(tmpl) == NULL) {
        perror("mkdtemp");
        abort();
    }

    _path = tmpl;
}

TempDir::~TempDir()
{
    removeTree(_path);
}

std::string
TempDir::child(const std::string &name) const
{
    return _path + "/" + name;
}

EnvGuard::EnvGuard(const char *name, const char *value)
    : _name(name), _had(false)
{
    const char *old = getenv(name);

    if (old != NULL) {
        _had = true;
        _old = old;
    }

    if (value != NULL) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

EnvGuard::~EnvGuard()
{
    if (_had) {
        setenv(_name.c_str(), _old.c_str(), 1);
    } else {
        unsetenv(_name.c_str());
    }
}

bool
writeFile(const std::string &path, const std::string &content, mode_t mode)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);

    if (fd < 0) {
        return false;
    }

    size_t done = 0;

    while (done < content.size()) {
        ssize_t n = write(fd, content.data() + done, content.size() - done);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            close(fd);
            return false;
        }

        done += (size_t) n;
    }

    fchmod(fd, mode);
    close(fd);
    return true;
}

std::string
readFile(const std::string &path)
{
    std::string out;
    int fd = open(path.c_str(), O_RDONLY);

    if (fd < 0) {
        return out;
    }

    char buf[4096];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, (size_t) n);
    }

    close(fd);
    return out;
}

bool
fileExists(const std::string &path)
{
    struct stat sb;

    return lstat(path.c_str(), &sb) == 0;
}

int
fileMode(const std::string &path)
{
    struct stat sb;

    if (lstat(path.c_str(), &sb) < 0) {
        return -1;
    }

    return (int) (sb.st_mode & 07777);
}

long
countFiles(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());

    if (d == NULL) {
        return -1;
    }

    long n = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            n++;
        }
    }

    closedir(d);
    return n;
}

bool
makeDir(const std::string &path, mode_t mode)
{
    if (mkdir(path.c_str(), mode) < 0 && errno != EEXIST) {
        return false;
    }

    return chmod(path.c_str(), mode) == 0;
}

void
removeTree(const std::string &path)
{
    struct stat sb;

    if (lstat(path.c_str(), &sb) < 0) {
        return;
    }

    if (S_ISDIR(sb.st_mode)) {
        DIR *d = opendir(path.c_str());

        if (d != NULL) {
            struct dirent *e;

            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") != 0
                    && strcmp(e->d_name, "..") != 0) {
                    removeTree(path + "/" + e->d_name);
                }
            }

            closedir(d);
        }

        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
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
