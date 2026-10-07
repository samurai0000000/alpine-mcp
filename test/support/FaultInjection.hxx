/*
 * FaultInjection.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_FAULT_INJECTION_HXX
#define ALPINE_MCP_TEST_FAULT_INJECTION_HXX

/*
 * Fault injection for the add-on's C code.
 *
 * The test executables are linked with -Wl,--wrap for the system calls
 * below, so every call from the add-on goes through here.  Nothing fails
 * unless a test arms a plan, and reset() clears every plan.
 *
 * The cJSON allocator is also hooked, so allocation failures can be put
 * into the JSON paths too.  Allocation failures in the add-on's own
 * malloc calls use CppUTest's malloc failure countdown instead.
 */
namespace Fault
{
    enum Call {
        Open, Write, Read, Fsync, Rename, Mkdir, Chmod, Fchmod, Unlink,
        Pipe2, Socket, Bind, Listen, PthreadCreate,
        NumCalls
    };

    void reset();

    /* the nth call from now (1 is the next one) fails with errno err */
    void failNth(Call c, int nth, int err);

    /* every call fails with errno err */
    void failAll(Call c, int err);

    /* only calls whose first string argument contains this text count
       (open, rename, mkdir, chmod, unlink); NULL removes the filter */
    void onlyPathsContaining(const char *text);

    /* how many times the call was made since reset() */
    int calls(Call c);

    /* time() answers the real time plus this many seconds */
    void advanceTime(long seconds);

    /* the add-on's own allocations: the nth from now fails; 0 only counts */
    void failAllocNth(int nth);
    int allocs();

    /* cJSON allocation: the nth allocation from now fails (0 counts only) */
    void installJsonHooks();
    void removeJsonHooks();
    void failJsonAllocNth(int nth);
    int jsonAllocs();
}

#endif /* ALPINE_MCP_TEST_FAULT_INJECTION_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
