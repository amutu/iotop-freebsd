#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T09 - the interactive view starts, draws the columns of the platform and quits on q

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1
need_root

# a terminal is needed for ncurses; script(1) provides one on FreeBSD
command -v script >/dev/null 2>&1 || { printf '  SKIP: no script(1)\n'; exit 77; }

TERM=xterm printf 'q' | TERM=xterm script -q /dev/null "$IOTOP" > "$TMPD/curses.out" 2>&1
RC=$?

check "the interactive view exits on q" test $RC -eq 0
check "ncurses could open the terminal" sh -c "! grep -q 'Error opening terminal' '$TMPD/curses.out'"

# the SWAPIN and IO columns must be gone: there is no delay accounting to fill them
if grep -q "SWAPIN" "$TMPD/curses.out"; then
	fail "the SWAPIN column is hidden (no delay accounting on FreeBSD)"
else
	pass "the SWAPIN column is hidden (no delay accounting on FreeBSD)"
fi

# the header of the remaining columns is there, the escape sequences may split it
if grep -q "DISK READ" "$TMPD/curses.out"; then
	pass "the DISK READ column is drawn"
else
	fail "the DISK READ column is drawn"
fi

finish
