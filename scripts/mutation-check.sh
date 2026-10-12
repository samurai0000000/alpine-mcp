#!/bin/sh
# ========================================================================
# Copyright (C) 2026, Charles Chiou
#
# ========================================================================
#
# Shows that the tests notice real defects.  Four deliberate breaks are
# made in the add-on, one at a time; for each, the tests that guard it must
# fail.  The source is put back after every break, and compared with the
# saved copy, whatever happens.  Run from the repository root:
#
#   scripts/mutation-check.sh            all four
#   scripts/mutation-check.sh 3          only the third
#
# Exit status is 0 only if every break was caught and every file restored.

mcp=third_party/alpine/pith/mcp
bin_unit=build/test/unit/AllUnitTests
bin_int=build/test/integration/AllIntegrationTests
alpine=$(pwd)/build/alpine/alpine/alpine
only="$1"
bad=0

# whatever happens, the sources go back as they were
restore() {
    for f in mcptools.c mcpwrite.c mcpserv.c mcpidem.c; do
        if [ -e $mcp/$f.orig ]; then
            mv $mcp/$f.orig $mcp/$f
            touch $mcp/$f
        fi
    done
    rm -f $mcp/mcptools.c.twice
}
trap restore EXIT INT TERM

# mutate FILE OLD NEW: exactly one occurrence of OLD is replaced by NEW
mutate() {
    python3 -I - "$1" "$2" "$3" <<'PY' || return 1
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
if text.count(old) != 1:
    sys.exit("mutation site not found exactly once in %s" % path)
open(path, "w").write(text.replace(old, new))
PY
}

# run one break: NUMBER NAME FILE OLD NEW KIND FILTER-ARGS...
try() {
    n="$1"; name="$2"; file="$3"; old="$4"; new="$5"; kind="$6"; shift 6
    [ -n "$only" ] && [ "$only" != "$n" ] && return 0

    echo
    echo "=== break $n: $name"
    cp -p "$file" "$file.orig" || exit 2
    if ! mutate "$file" "$old" "$new"; then
        mv "$file.orig" "$file"
        echo "NOT RUN: could not make the break"
        bad=1
        return 0
    fi

    caught=0
    if [ "$kind" = unit ]; then
        make build/test/unit/AllUnitTests >/dev/null 2>&1 || true      # builds; the run may fail
        $bin_unit "$@" 2>&1 | grep -E "Failure in|Errors \(|OK \(" | head -8
        $bin_unit "$@" >/dev/null 2>&1 || caught=1
    else
        make alpine >/dev/null 2>&1
        make build/test/integration/AllIntegrationTests >/dev/null 2>&1
        ALPINE_MCP_ALPINE=$alpine $bin_int "$@" 2>&1 \
            | grep -E "Failure in|Errors \(|OK \(" | head -8
        ALPINE_MCP_ALPINE=$alpine $bin_int "$@" >/dev/null 2>&1 || caught=1
    fi

    mv "$file.orig" "$file"
    touch "$file"
    if [ $caught = 1 ]; then
        echo "CAUGHT: break $n made the tests fail"
    else
        echo "MISSED: break $n did not fail any test"
        bad=1
    fi
}

# 1. the validator accepts a non-enum flag (\Draft)
try 1 "flag validator accepts non-enum flag \\Draft" $mcp/mcptools.c \
    '		 && strcmp(v->valuestring, "\\Deleted")){' \
    '		 && strcmp(v->valuestring, "\\Deleted") && strcmp(v->valuestring, "\\Draft")){' \
    unit -g ToolValidator -n SetFlagAllowsFourFlags

# 1b. copy with move=false still passes CP_MOVE
try 1b "copy with move=false passes CP_MOVE" $mcp/mcpwrite.c \
    '#define MCP_COPY_FLAGS(move)	((MCP_COPY_FLAGS_BASE) | ((move) ? CP_MOVE : 0))' \
    '#define MCP_COPY_FLAGS(move)	((MCP_COPY_FLAGS_BASE) | CP_MOVE)' \
    integration -g WriteIntegration -n CopiesToAnotherFolderAndLeavesTheOriginal

# 2. the envelope sender is left empty (MAIL FROM:<>)
try 2 "empty envelope sender" $mcp/mcpwrite.c \
    "m->env->return_path = rfc822_cpy_adr(m->env->from);" \
    "m->env->return_path = NULL;" \
    integration -g SendIntegration -n TheEnvelopeSenderIsTheFromAddress

# 3. a cancelled question still sends when the human says yes
try 3 "cancelled question still sends" $mcp/mcpserv.c \
    "	if(it->cancelled || mcp_lock_engaged()){
	    it->response = it->cancelled" \
    "	if(mcp_lock_engaged()){
	    it->response = it->cancelled" \
    unit -g ServerParking
try 3b "cancelled question still sends (real Alpine)" $mcp/mcpserv.c \
    "	if(it->cancelled || mcp_lock_engaged()){
	    it->response = it->cancelled" \
    "	if(mcp_lock_engaged()){
	    it->response = it->cancelled" \
    integration -g SendIntegration -n AClientThatLeavesCancelsTheCallEvenIfTheHumanSaysYes

# 4. the idempotency record is not forced to disk
try 4 "dropped fsync of the idempotency record" $mcp/mcpidem.c \
    "    return(fsync(fd));" \
    "    return(0);" \
    unit -g Idempotency

echo
for f in mcptools.c mcpwrite.c mcpserv.c mcpidem.c; do
    if [ -e $mcp/$f.orig ]; then echo "LEFT OVER: $mcp/$f.orig"; bad=1; fi
done
(cd third_party/alpine && git diff --stat -- pith/mcp/mcptools.c >/dev/null)
make test-unit >/dev/null 2>&1 && echo "baseline: unit tests pass again with the sources restored" \
    || { echo "baseline: unit tests FAIL after restore"; bad=1; }
exit $bad
