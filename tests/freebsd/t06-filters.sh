#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T06 - the pid, user and only-nonzero filters work on FreeBSD

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1
need_root

IOTEST="${IOTEST:-./iotest}"
[ -x "$IOTEST" ] || { printf '  SKIP: %s is missing\n' "$IOTEST"; exit 77; }

make_file "$TMPD/data.bin" 8
"$IOTEST" r "$TMPD/data.bin" 8000000 2 1 4 > "$TMPD/helper.out" 2>&1 &
HPID=$!

"$IOTOP" -b -n 5 -d 1 -p "$HPID" -a > "$TMPD/pid.out" 2>&1

# every task row must belong to the requested process
STRANGE=$(awk -v p="$HPID" '$1 ~ /^[0-9]+$/ && ($1+0)!=(p+0) { n++ } END { print n+0 }' "$TMPD/pid.out")
check "--pid keeps only the threads of that process ($STRANGE foreign rows)" test "$STRANGE" -eq 0

ROWS=$(count_rows "$TMPD/pid.out")
nonzero "--pid still shows the process" "$ROWS"

# --user: all the rows must be of that user
"$IOTOP" -b -n 1 -u root > "$TMPD/root.out" 2>&1
BAD=$(awk '$1 ~ /^[0-9]+$/ && $3 != "root" { n++ } END { print n+0 }' "$TMPD/root.out")
check "--user root keeps only root tasks ($BAD foreign rows)" test "$BAD" -eq 0

# --only (-o): no row may have zero bytes on both sides
"$IOTOP" -b -n 4 -d 1 -o > "$TMPD/only.out" 2>&1
ZERO=$(awk '
	function mul(u){ if(u=="K")return 1024; if(u=="M")return 1048576; if(u=="G")return 1073741824; return 1 }
	$1 ~ /^[0-9]+$/ { r=$4*mul($5); w=$6*mul($7); if (r==0 && w==0) n++ }
	END { print n+0 }' "$TMPD/only.out")
check "--only hides the idle tasks ($ZERO idle rows)" test "$ZERO" -eq 0

# -n must stop after the requested number of cycles
LINES=$("$IOTOP" -b -n 3 -d 1 2>/dev/null | grep -c "Total DISK READ")
check "-n 3 prints exactly three cycles (got $LINES)" test "$LINES" -eq 3

wait $HPID 2>/dev/null
rm -f "$TMPD/data.bin"
finish
