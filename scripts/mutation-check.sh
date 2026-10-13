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
    for f in mcptools.c mcpwrite.c mcpserv.c mcpidem.c mcpread.c; do
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

# 5. mcp_uid_set range compression off by one
try 5 "mcp_uid_set range compression off by one" $mcp/mcptools.c \
    '	while(i + 1 < n && sorted[i + 1] == end + 1){' \
    '	while(i + 1 < n && sorted[i + 1] == end + 2){' \
    unit -g ToolValidator -n McpUidSetFormattingAndLimits

# 6. duplicate check removed
try 6 "duplicate check removed" $mcp/mcptools.c \
    '		      if(uids[i] == uids[j]){' \
    '		      if(0 && uids[i] == uids[j]){' \
    unit -g ToolValidator -n UidsArrayBoundariesAndTypes

# 7. single re-issue removed
try 7 "single re-issue removed" $mcp/mcptools.c \
    '    if(pass == 1)
      return(MCP_ACT_REISSUE);' \
    '    if(pass == 1)
      return(MCP_ACT_FAIL);' \
    unit -g ToolValidator -n McpBatchClassifyRacesAndPasses

# 8. missing-UID skip removed
try 8 "missing-UID skip removed" $mcp/mcpread.c \
    '	else {
	    if(uids && uids->nmissing < MCP_UID_BATCH_MAX)
	      uids->missing[uids->nmissing++] = target;' \
    '	else {
	    mcp_call_error(call, MCP_ERR_PARAMS, "missing uid not skipped");
	    return(0);' \
    integration -g WriteIntegration -n BatchSetFlagWithMissingUidAndAllMissing

# 9. source_removed computed from only first message
try 9 "source_removed computed from only first message" $mcp/mcpwrite.c \
    '		deleted_now = 1;
		for(i = 0; i < n_req; i++){
		    if(state[i] == MCP_UID_DIFFER){' \
    '		deleted_now = 0;
		for(i = 0; i < n_req; i++){
		    if(state[i] == MCP_UID_DIFFER){' \
    integration -g WriteIntegration -n BatchCopyAndMove

# 10. UIDs skipped on cached state
try 10 "UIDs skipped on cached state" $mcp/mcpwrite.c \
    '	mail_flag(s, uids.set, (char *) flag, ST_UID | (set ? ST_SET : 0));

	/* read back and mcp_batch_classify pass 1 */
	if(!mcp_uids_prepare(s, args, call, &uids, flag, set, state)){
	    mcp_release(s, opened);
	    return;
	}

	act = mcp_batch_classify(state, n_req, 1, &ngone, &nmatch, &ndiffer);
	if(act == MCP_ACT_REISSUE){' \
    '	/* skipped on cached state */
	if(0) mail_flag(s, uids.set, (char *) flag, ST_UID | (set ? ST_SET : 0));

	/* read back and mcp_batch_classify pass 1 */
	if(!mcp_uids_prepare(s, args, call, &uids, flag, set, state)){
	    mcp_release(s, opened);
	    return;
	}

	act = MCP_ACT_DONE;
	if(0 && act == MCP_ACT_REISSUE){' \
    integration -g WriteIntegration -n BatchSetFlagSetsAndClearsFlags

# 11. message numbers cached across the command
try 11 "message numbers cached across the command" $mcp/mcpread.c \
    '    for(j = 1; j <= s->nmsgs && nfound < MCP_UID_BATCH_MAX; j++){' \
    '    for(j = 1; (flag == NULL) && j <= s->nmsgs && nfound < MCP_UID_BATCH_MAX; j++){' \
    integration -g WriteIntegration -n BatchSetFlagSetsAndClearsFlags

echo
for f in mcptools.c mcpwrite.c mcpserv.c mcpidem.c mcpread.c; do
    if [ -e $mcp/$f.orig ]; then echo "LEFT OVER: $mcp/$f.orig"; bad=1; fi
done
(cd third_party/alpine && git diff --stat -- pith/mcp/mcptools.c >/dev/null)
make test-unit >/dev/null 2>&1 && echo "baseline: unit tests pass again with the sources restored" \
    || { echo "baseline: unit tests FAIL after restore"; bad=1; }
exit $bad
