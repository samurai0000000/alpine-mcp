/*
 * config.h
 *
 * Copyright (C) 2026, Charles Chiou
 *
 * Stand-in for the config.h that Alpine's configure generates.  Two of the
 * add-on's C files include <config.h>; the unit tests build them without
 * Alpine, so they get this one.
 */

#ifndef ALPINE_MCP_TEST_CONFIG_H
#define ALPINE_MCP_TEST_CONFIG_H

#define PACKAGE_VERSION "2.26"
#define HAVE_PTHREAD 1

#endif /* ALPINE_MCP_TEST_CONFIG_H */
