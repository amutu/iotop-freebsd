#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T01 - the binary runs and its option parsing is intact

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1

"$IOTOP" -h > "$TMPD/help.out" 2>&1
check "-h exits 0" test $? -eq 0
check "-h prints the usage" grep -q "Usage" "$TMPD/help.out"
check "-h mentions the FreeBSD specific columns" grep -qE "SWAPIN|PRIO" "$TMPD/help.out"

"$IOTOP" --version > "$TMPD/ver.out" 2>&1
check "--version exits 0" test $? -eq 0
check "--version prints a version" grep -qE "[0-9]+\.[0-9]+" "$TMPD/ver.out"

"$IOTOP" --no-such-option >/dev/null 2>&1
check "an unknown option is rejected" test $? -ne 0

# the config file path must not have changed: the FreeBSD build keeps the XDG one
XDG_CONFIG_HOME="$TMPD/cfg" "$IOTOP" -b -n 1 >/dev/null 2>&1
check "a batch run with an empty config dir works" test $? -eq 0

finish
