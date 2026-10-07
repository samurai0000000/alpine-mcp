#!/usr/bin/env python3
# ========================================================================
# Copyright (C) 2026, Charles Chiou
#
# ========================================================================
#
# Line coverage per source file from an lcov .info file, against the
# thresholds of the plan: the files that link without Alpine (named on the
# command line) at least MIN_FILE percent, the sum of all at least
# MIN_TOTAL percent.  Every file below MIN_FILE is listed; only a named
# file or the total below its threshold makes the exit status 1.  The
# others need a real IMAP server or a person at the terminal, and are
# reported so the shortfall is visible.
#
#   covcheck.py FILE.info MIN_FILE MIN_TOTAL [STRICT_FILE ...]

import sys


def main():
    if len(sys.argv) < 4:
        print("usage: covcheck.py FILE.info MIN_FILE MIN_TOTAL [STRICT_FILE ...]",
              file=sys.stderr)
        return 2

    path, min_file, min_total = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    strict = set(sys.argv[4:])
    files = {}
    cur = None

    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("SF:"):
                cur = line[3:]
                files.setdefault(cur, [0, 0])
            elif line.startswith("LF:") and cur:
                files[cur][1] = int(line[3:])
            elif line.startswith("LH:") and cur:
                files[cur][0] = int(line[3:])

    hit = sum(v[0] for v in files.values())
    found = sum(v[1] for v in files.values())
    low = []
    must = []

    print("\nline coverage (thresholds: file >= %g%%, total >= %g%%)" % (min_file, min_total))
    for name in sorted(files):
        h, n = files[name]
        pct = 100.0 * h / n if n else 100.0
        base = name.rsplit("/", 1)[-1]
        mark = "" if pct >= min_file else "   <-- below %g%%" % min_file
        print("  %6.1f%%  %4d/%-4d  %s%s%s" % (pct, h, n, base, mark,
                                            "  (required)" if base in strict else ""))
        if pct < min_file:
            low.append(name)
            if base in strict:
                must.append(name)

    total = 100.0 * hit / found if found else 100.0
    print("  %6.1f%%  %4d/%-4d  TOTAL" % (total, hit, found))

    if total < min_total:
        print("FAIL: total %.1f%% is below %g%%" % (total, min_total))
        return 1

    if must:
        print("FAIL: %d required file(s) below %g%%" % (len(must), min_file))
        return 1

    if low:
        print("NOTE: %d file(s) below %g%% that need IMAP or a person; not required"
              % (len(low), min_file))

    return 0


sys.exit(main())
