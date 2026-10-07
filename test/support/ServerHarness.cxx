/*
 * ServerHarness.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ServerHarness.hxx"

#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <poll.h>
#include <unistd.h>

extern "C" {
#include "mcpjson.h"
#include "mcp.h"
#include "mcpconf.h"
}

ServerHarness::ServerHarness()
    : _runtime("XDG_RUNTIME_DIR", _tmp.path().c_str()),
      _state("XDG_STATE_HOME", _tmp.child("state").c_str()),
      _home("HOME", _tmp.path().c_str()),
      _started(false)
{
    makeDir(_tmp.child(".config"), 0700);
    makeDir(_tmp.child(".config/alpine-mcp"), 0700);
}

ServerHarness::~ServerHarness()
{
    stop();
    mcp_config_load(NULL);
}

void
ServerHarness::useConfig(const std::string &json)
{
    writeFile(_tmp.child(".config/alpine-mcp/config.json"), json, 0600);
    mcp_config_load((char *) _tmp.path().c_str());
}

void
ServerHarness::usePolicy(bool lock, const std::string &allowedJson)
{
    useConfig(std::string("{\"mcp\":{\"enabled\":true},\"operator_lock\":")
              + (lock ? "true" : "false") + ",\"allowed_recipients\":"
              + allowedJson
              + ",\"aimon\":{\"enabled\":false,\"host\":\"127.0.0.1\","
                "\"port\":3885}}");
}

int
ServerHarness::start()
{
    int rc = mcp_server_start();

    _started = (rc == 0 && mcp_server_running());
    return rc;
}

void
ServerHarness::stop()
{
    if (mcp_server_running()) {
        mcp_server_stop();
    }

    _started = false;
}

bool
ServerHarness::running() const
{
    return mcp_server_running() != 0;
}

std::string
ServerHarness::socketPath() const
{
    /* XDG_RUNTIME_DIR is the temporary directory (see the constructor) */
    return _tmp.child("alpine-mcp.sock");
}

bool
ServerHarness::drain(int timeoutMs)
{
    int fd = mcp_wake_fd();

    if (fd < 0) {
        return false;
    }

    struct pollfd p = { fd, POLLIN, 0 };
    int rc;

    do {
        rc = poll(&p, 1, timeoutMs);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0) {
        return false;
    }

    mcp_drain();
    return true;
}

int
ServerHarness::drainAll(int timeoutMs, int quietMs)
{
    int n = 0;

    if (drain(timeoutMs)) {
        n++;
        while (drain(quietMs)) {
            n++;
        }
    }

    return n;
}

/*
 * What this process has open.  The directory stream used to read
 * /proc/self/fd is itself a descriptor; it is left out.
 */
std::string
descriptorList()
{
    std::string out;
    DIR *d = opendir("/proc/self/fd");

    if (d == NULL) {
        return out;
    }

    int own = dirfd(d);
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || atoi(e->d_name) == own) {
            continue;
        }

        char link[300];
        std::string path = std::string("/proc/self/fd/") + e->d_name;
        ssize_t n = readlink(path.c_str(), link, sizeof(link) - 1);

        out += std::string(e->d_name) + "=" + (n > 0 ? std::string(link, (size_t) n) : "?") + " ";
    }

    closedir(d);
    return out;
}

int
openDescriptors()
{
    std::string list = descriptorList();
    int n = 0;

    for (size_t i = 0; i < list.size(); i++) {
        if (list[i] == ' ') {
            n++;
        }
    }

    return n;
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
