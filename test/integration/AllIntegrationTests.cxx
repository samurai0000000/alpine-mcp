/*
 * AllIntegrationTests.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cstdio>

#include "RealHomeGuard.hxx"

#include "CppUTest/CommandLineTestRunner.h"

int
main(int argc, char **argv)
{
    RealHomeGuard guard;
    int rc = CommandLineTestRunner::RunAllTests(argc, argv);
    std::string changed = guard.changes();

    if (!changed.empty()) {
        fprintf(stderr, "FAIL: the run changed the real home directory:\n%s",
                changed.c_str());
        return rc != 0 ? rc : 3;
    }

    return rc;
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
