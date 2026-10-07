/*
 * RealHomeGuard.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "RealHomeGuard.hxx"

#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    /* what Alpine and the add-on read or write under the home */
    const char *roots[] = {
        "mail", ".pinerc", ".addressbook", ".pine-interrupted-mail",
        ".config/alpine-mcp", ".local/state/alpine-mcp",
    };

    std::string
    describe(const struct stat &st)
    {
        return std::to_string((long long) st.st_size) + " "
            + std::to_string((long long) st.st_mtim.tv_sec) + "."
            + std::to_string((long long) st.st_mtim.tv_nsec) + " "
            + std::to_string((int) (st.st_mode & 07777));
    }

    void
    walk(const std::string &path, std::map<std::string, std::string> &out)
    {
        struct stat st;

        if (lstat(path.c_str(), &st) != 0) {
            out[path] = "absent";
            return;
        }

        out[path] = describe(st);

        if (!S_ISDIR(st.st_mode)) {
            return;
        }

        DIR *d = opendir(path.c_str());

        if (d == NULL) {
            return;
        }

        for (struct dirent *e; (e = readdir(d)) != NULL; ) {
            std::string name = e->d_name;

            if (name != "." && name != "..") {
                walk(path + "/" + name, out);
            }
        }

        closedir(d);
    }
}

RealHomeGuard::RealHomeGuard()
{
    struct passwd *pw = getpwuid(getuid());

    _home = pw != NULL ? pw->pw_dir : "";
    _before = take(_home);
}

RealHomeGuard::Snapshot
RealHomeGuard::take(const std::string &home)
{
    Snapshot s;

    if (home.empty()) {
        return s;
    }

    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        walk(home + "/" + roots[i], s);
    }

    return s;
}

std::string
RealHomeGuard::changes() const
{
    Snapshot now = take(_home);
    std::string out;

    for (Snapshot::const_iterator i = now.begin(); i != now.end(); ++i) {
        Snapshot::const_iterator was = _before.find(i->first);

        if (was == _before.end()) {
            out += "new      " + i->first + "\n";
        } else if (was->second != i->second) {
            out += "changed  " + i->first + "  (" + was->second + " -> "
                + i->second + ")\n";
        }
    }

    for (Snapshot::const_iterator i = _before.begin(); i != _before.end(); ++i) {
        if (now.find(i->first) == now.end()) {
            out += "removed  " + i->first + "\n";
        }
    }

    return out;
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
