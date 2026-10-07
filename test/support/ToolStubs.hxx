/*
 * ToolStubs.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef ALPINE_MCP_TEST_TOOL_STUBS_HXX
#define ALPINE_MCP_TEST_TOOL_STUBS_HXX

#include <string>

/*
 * The tool run functions of the add-on need Alpine.  The server and the
 * tool table do not, so the unit tests link them with these stand-ins.
 * Each stand-in records what it was asked and then does what the test
 * told it to do.
 */
struct StubBehavior {
    int code;                   /* 0 = success, else a JSON-RPC error code */
    std::string message;        /* error message when code is not 0 */
    std::string resultJson;     /* result when code is 0 ("" = a default) */
    bool park;                  /* ask the human first (the send tool) */
    std::string prompt;         /* the question when parking */
    int delayMs;                /* sleep inside the run function */

    /* filled in by the stand-ins */
    int runs;
    int confirmedRuns;
    char lastTool[64];          /* fixed arrays: no heap, so a test that   */
    char lastArgs[2048];        /* turns leak tracking off can touch them  */

    StubBehavior();
    void reset();
};

extern StubBehavior stubBehavior;

#endif /* ALPINE_MCP_TEST_TOOL_STUBS_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
