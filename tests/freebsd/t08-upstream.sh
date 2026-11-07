#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T08 - the upstream regression test still passes

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1

if [ -x tests/tests-opt-W.sh ]; then
	if IOTOP="$IOTOP" sh tests/tests-opt-W.sh; then
		pass "tests/tests-opt-W.sh (the option output of --debug-write)"
	else
		fail "tests/tests-opt-W.sh (the option output of --debug-write)"
	fi
else
	printf '  SKIP: tests/tests-opt-W.sh is not executable\n'
fi

# the config file is written and read back; it has no platform specific entries
export XDG_CONFIG_HOME="$TMPD/cfg"
"$IOTOP" -b -n 1 -k --debug-write > "$TMPD/dw.out" 2>&1
check "--debug-write prints the option list" grep -q "^--" "$TMPD/dw.out"
check "the kilobytes option is in the dumped config" grep -q "^--kilobytes$" "$TMPD/dw.out"

finish
