/*
 * RealHomeGuard.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_REAL_HOME_GUARD_HXX
#define ALPINE_MCP_TEST_REAL_HOME_GUARD_HXX

#include <map>
#include <string>

/*
 * The integration tests run Alpine in a home of its own, but Alpine takes
 * its idea of the home directory from the password file, not from $HOME,
 * so a relative folder name can land in the user's real mail.  This
 * records the real home's mail files, pinerc, address book and MCP
 * directories before the run and compares them afterwards: any change
 * fails the run, whichever test caused it.
 */
class RealHomeGuard
{
public:
    RealHomeGuard();

    /* what differs from the snapshot taken by the constructor, one line
       per path; empty if nothing changed */
    std::string changes() const;

private:
    typedef std::map<std::string, std::string> Snapshot;

    static Snapshot take(const std::string &home);

    std::string _home;
    Snapshot _before;
};

#endif /* ALPINE_MCP_TEST_REAL_HOME_GUARD_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
