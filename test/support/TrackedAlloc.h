/*
 * TrackedAlloc.h
 *
 * Copyright (C) 2026, Charles Chiou
 *
 * Force-included when the add-on's C files are built for the unit tests.
 * malloc, calloc, realloc, free and strdup in those files go through
 * FaultInjection.cxx, which counts the allocations, can fail the nth one,
 * and hands the memory to CppUTest's leak detector.  (CppUTest's own
 * failure countdown replaces its allocator, which confuses its leak
 * bookkeeping, and its strdup macro copies into the NULL after a failure.)
 */

#ifndef ALPINE_MCP_TEST_TRACKED_ALLOC_H
#define ALPINE_MCP_TEST_TRACKED_ALLOC_H

/* the system headers first, so their declarations are not rewritten */
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

void *test_alloc_malloc(size_t size, const char *file, int line);
void *test_alloc_calloc(size_t count, size_t size, const char *file, int line);
void *test_alloc_realloc(void *p, size_t size, const char *file, int line);
void test_alloc_free(void *p, const char *file, int line);
char *test_alloc_strdup(const char *s, const char *file, int line);

#ifdef __cplusplus
}
#endif

#define malloc(n)	test_alloc_malloc((n), __FILE__, __LINE__)
#define calloc(c, n)	test_alloc_calloc((c), (n), __FILE__, __LINE__)
#define realloc(p, n)	test_alloc_realloc((p), (n), __FILE__, __LINE__)
#define free(p)		test_alloc_free((p), __FILE__, __LINE__)
#define strdup(s)	test_alloc_strdup((s), __FILE__, __LINE__)

#endif /* ALPINE_MCP_TEST_TRACKED_ALLOC_H */
