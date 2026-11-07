#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T02 - the task list agrees with procstat, the metadata columns are filled

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1
need_root

"$IOTOP" -b -n 1 > "$TMPD/list.out" 2>&1
check "the first cycle prints the header" grep -q "TID" "$TMPD/list.out"

ROWS=$(count_rows "$TMPD/list.out")
PSROWS=$(procstat -a -t 2>/dev/null | awk 'NR>1 && $1 ~ /^[0-9]+$/ { n++ } END { print n+0 }')

nonzero "the task list is not empty" "$ROWS"
# both read the same kinfo_getallproc(3) array; a few tasks may be born or die between the runs
if within "$ROWS" "$PSROWS" 20; then
	pass "the row count matches procstat -a -t ($ROWS vs $PSROWS)"
else
	fail "the row count matches procstat -a -t ($ROWS vs $PSROWS)"
fi

if grep -qE "^ *$$ " "$TMPD/list.out"; then
	pass "the task of this test script ($$) is listed"
else
	fail "the task of this test script ($$) is listed"
fi

# the io priority column is synthesised on FreeBSD: rt|be|id plus the level
PRIO_BAD=$(awk '$1 ~ /^[0-9]+$/ && $2 !~ /^(rt|be|id|xx|-)\/[0-7]$/ { n++ } END { print n+0 }' "$TMPD/list.out")
check "every PRIO value is a synthesised class/level ($PRIO_BAD bad)" test "$PRIO_BAD" -eq 0

USER_BAD=$(awk '$1 ~ /^[0-9]+$/ && $3 == "" { n++ } END { print n+0 }' "$TMPD/list.out")
check "every row has a user name" test "$USER_BAD" -eq 0

# kernel threads have no argv: their command is the bracketed name, like on Linux
check "kernel threads show a bracketed name" grep -q "\[kernel\]\|\[init\]" "$TMPD/list.out"

# the process view (-P) must have one row per process, not per thread
"$IOTOP" -b -n 1 -P > "$TMPD/proc.out" 2>&1
PROWS=$(count_rows "$TMPD/proc.out")
if [ "$PROWS" -lt "$ROWS" ]; then
	pass "the process view has fewer rows than the thread view ($PROWS < $ROWS)"
else
	fail "the process view has fewer rows than the thread view ($PROWS >= $ROWS)"
fi

finish
