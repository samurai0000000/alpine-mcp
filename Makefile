# ========================================================================
# Copyright (C) 2026, Charles Chiou
#
# ========================================================================
#
# Top-level build for alpine-mcp: Alpine 2.26 (third_party/alpine) with
# the MCP add-on.  Everything the build makes goes
# under build/, which git ignores.
#
# Alpine's bundled c-client library builds inside its own source
# directory and cannot build out of tree.  So the sources are copied
# into build/alpine (git's list of tracked and new, not-ignored files,
# which includes uncommitted work) and configured and built there.  The
# copy keeps timestamps, so a second run rebuilds only what changed and
# the submodule stays free of build products.
#
#   make              build Alpine
#   make -j8          the same, in parallel
#   make clean        remove build products (keeps the configured copy)
#   make distclean    remove build/
#   make clean-intree remove products of an old in-tree build in the
#                     submodule
#   make test         build Alpine, then run the unit and integration tests
#   make test-unit    the CppUTest unit tests (test/unit)
#   make test-integration  CppUTest tests of a real Alpine on a pty
#                     (test/integration); builds Alpine first
#   make coverage     both test suites on instrumented code (build/test-cov,
#                     build/alpine-cov); lcov report in build/coverage
#   make help         this list
#
# Variables: ALPINE_CONFIGURE_ARGS is passed to Alpine's configure;
# JOBS sets the parallelism of Alpine's own make (default: all CPUs).

BUILD	:= build
SRC	:= third_party/alpine
TREE	:= $(BUILD)/alpine
ALPINE	:= $(TREE)/alpine/alpine

JOBS	?= $(shell nproc 2>/dev/null || echo 2)
ALPINE_CONFIGURE_ARGS ?=

.PHONY: all alpine sync clean distclean clean-intree help \
	test test-unit test-integration coverage

all: alpine

help:
	@sed -n '/^#   make /p' Makefile | sed 's/^#   //'

# The submodule is initialized if its files are missing.
$(SRC)/configure:
	git submodule update --init $(SRC)

# Copy the Alpine sources into build/alpine.  Files that disappeared
# from the submodule are not removed; use distclean for a fresh copy.
sync: $(SRC)/configure
	@mkdir -p $(TREE)
	@git -C $(SRC) ls-files -co --exclude-standard -z \
	  | rsync -a --from0 --files-from=- --ignore-missing-args \
	      $(SRC)/ $(TREE)/

alpine: sync
	@test -f $(TREE)/config.status \
	  || (cd $(TREE) && ./configure $(ALPINE_CONFIGURE_ARGS))
	$(MAKE) -C $(TREE) -j$(JOBS)

clean:
	-@test -f $(TREE)/Makefile && $(MAKE) -C $(TREE) clean >/dev/null 2>&1

distclean:
	rm -rf $(BUILD)

clean-intree:
	@if [ -f $(SRC)/Makefile ]; then $(MAKE) -C $(SRC) distclean; \
	else echo "no in-tree build in $(SRC)"; fi


# ------------------------------------------------------------------------
# Tests: CppUTest (libcpputest-dev), C++ sources under test/.  The add-on's
# own C files are linked without the rest of Alpine.  Everything goes to
# build/test.
# ------------------------------------------------------------------------

TESTBUILD := $(BUILD)/test
MCPDIR	  := $(SRC)/pith/mcp
CXX	  ?= c++
COVFLAGS  := $(if $(COVERAGE),--coverage,)
TEST_INC  := -I test/support -I $(MCPDIR)
TEST_CFLAGS := -g -O0 -Wall -Wextra -std=gnu99 -D_FORTIFY_SOURCE=0 \
	-DHAVE_CONFIG_H \
	$(TEST_INC) $(COVFLAGS) -MMD -MP
TEST_CXXFLAGS := -g -O0 -Wall -Wextra -std=gnu++14 -D_FORTIFY_SOURCE=0 \
	$(TEST_INC) $(COVFLAGS) -MMD -MP
TEST_LIBS := -lCppUTestExt -lCppUTest -lpthread
comma := ,
TEST_WRAPS := $(addprefix -Wl$(comma)--wrap=,open write read fsync rename \
	mkdir chmod fchmod unlink pipe2 socket bind listen pthread_create time)

# add-on C files that link without Alpine; the first group is built with
# CppUTest's malloc tracking (leak detection and allocation failures).
# The server and cJSON create threads of their own, so they are not.
TRACKED_C := mcpidem mcpconf mcptools mcppath mcpjson
PLAIN_C   := mcpserv cJSON

UNIT_CXX  := $(wildcard test/unit/*.cxx) $(wildcard test/support/*.cxx)
UNIT_OBJS := $(patsubst %.cxx,$(TESTBUILD)/obj/%.o,$(UNIT_CXX)) \
	$(patsubst %,$(TESTBUILD)/obj/tracked/%.o,$(TRACKED_C)) \
	$(patsubst %,$(TESTBUILD)/obj/plain/%.o,$(PLAIN_C))
UNIT_BIN  := $(TESTBUILD)/unit/AllUnitTests

$(TESTBUILD)/obj/tracked/%.o: $(MCPDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -include TrackedAlloc.h \
	  -c -o $@ $<

$(TESTBUILD)/obj/plain/%.o: $(MCPDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TESTBUILD)/obj/%.o: %.cxx
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_CXXFLAGS) -c -o $@ $<

$(UNIT_BIN): $(UNIT_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(COVFLAGS) -o $@ $(UNIT_OBJS) $(TEST_WRAPS) $(TEST_LIBS)

-include $(shell find $(TESTBUILD)/obj -name '*.d' 2>/dev/null)

test-unit: $(UNIT_BIN)
	$(UNIT_BIN) -c

# Integration tests drive the real Alpine binary on a pseudo terminal.
INT_CXX   := $(wildcard test/integration/*.cxx)
INT_SUP   := test/support/TestSupport.cxx test/support/JsonLineClient.cxx \
	test/support/FakeGateway.cxx
INT_OBJS  := $(patsubst %.cxx,$(TESTBUILD)/obj/%.o,$(INT_CXX) $(INT_SUP)) \
	$(TESTBUILD)/obj/plain/cJSON.o
INT_BIN   := $(TESTBUILD)/integration/AllIntegrationTests

$(INT_BIN): $(INT_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(COVFLAGS) -o $@ $(INT_OBJS) $(TEST_LIBS) -lutil

test-integration: alpine $(INT_BIN)
	ALPINE_MCP_ALPINE=$(abspath $(ALPINE)) $(INT_BIN) -c

test: test-unit test-integration

# ------------------------------------------------------------------------
# Coverage: the unit tests and an instrumented Alpine (-O0 --coverage)
# driven by the integration tests, both reported over the add-on's own
# sources (pith/mcp and alpine/mcpui.c, not the vendored cJSON).  Paths of the build copy are
# mapped back to third_party/alpine so the two runs merge file by file.
# ------------------------------------------------------------------------

COVDIR	  := $(BUILD)/coverage
COV_UNIT  := $(BUILD)/test-cov
COV_TREE  := $(BUILD)/alpine-cov
COV_FILES := '*/pith/mcp/*' '*/alpine/mcpui.c'
COV_IGN   := --ignore-errors mismatch,mismatch,inconsistent,unused,negative,gcov,source,empty
COV_IGN_HTML := --ignore-errors mismatch,inconsistent,unused,negative,source,empty
# the thresholds the plan asks for, in percent of lines
COV_MIN_FILE  ?= 85
COV_MIN_TOTAL ?= 75
# the add-on files that link without Alpine, held to COV_MIN_FILE
COV_STRICT    := mcpconf.c mcpidem.c mcpjson.c mcppath.c mcpserv.c mcptools.c

coverage:
	@mkdir -p $(COVDIR)
	find $(COV_UNIT) $(COV_TREE) -name '*.gcda' -delete 2>/dev/null || true
	$(MAKE) TESTBUILD=$(COV_UNIT) COVERAGE=1 test-unit
	$(MAKE) TREE=$(COV_TREE) TESTBUILD=$(COV_UNIT) COVERAGE=1 \
	  ALPINE_CONFIGURE_ARGS="CFLAGS='-g -O0 --coverage' LDFLAGS='--coverage'" \
	  test-integration
	lcov --capture --directory $(COV_UNIT) $(COV_IGN) \
	  --output-file $(COVDIR)/unit.info >/dev/null
	lcov --capture --directory $(COV_TREE) $(COV_IGN) \
	  --output-file $(COVDIR)/alpine.info >/dev/null
	@# the instrumented Alpine was built from the copy: name its files as
	@# the submodule does, so both runs count for the same file
	sed -i 's#^SF:.*/$(COV_TREE)/#SF:$(abspath $(SRC))/#' $(COVDIR)/alpine.info
	lcov -a $(COVDIR)/unit.info -a $(COVDIR)/alpine.info $(COV_IGN) \
	  --output-file $(COVDIR)/raw.info >/dev/null
	lcov --extract $(COVDIR)/raw.info $(COV_FILES) $(COV_IGN) \
	  --output-file $(COVDIR)/all.info >/dev/null
	lcov --remove $(COVDIR)/all.info '*/cJSON.c' $(COV_IGN) \
	  --output-file $(COVDIR)/mcp.info >/dev/null
	genhtml $(COVDIR)/mcp.info $(COV_IGN_HTML) --output-directory $(COVDIR)/html >/dev/null
	@python3 -I scripts/covcheck.py $(COVDIR)/mcp.info $(COV_MIN_FILE) \
	  $(COV_MIN_TOTAL) $(COV_STRICT) > $(COVDIR)/summary.txt; rc=$$?; \
	  cat $(COVDIR)/summary.txt; exit $$rc
