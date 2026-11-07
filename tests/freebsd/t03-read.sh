#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T03 - the read counters of a known task agree with the bytes it moved

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1
need_root

IOTEST="${IOTEST:-./iotest}"
[ -x "$IOTEST" ] || { printf '  SKIP: %s is missing\n' "$IOTEST"; exit 77; }

make_file "$TMPD/data.bin" 32

# the helper does not move a byte until the go file exists, so iotop is already
# up and has taken its first sample when the load begins
GO="$TMPD/go"
"$IOTEST" r "$TMPD/data.bin" 40000000 3 1 3 "$GO" > "$TMPD/helper.out" 2>&1 &
HPID=$!
i=0
while [ ! -s "$TMPD/helper.out" ] && [ $i -lt 100 ]; do
	sleep 0.05
	i=$((i+1))
done

"$IOTOP" -b -n 10 -d 1 -p "$HPID" -a > "$TMPD/iotop.out" 2>&1 &
IPID=$!
sleep 1.5
: > "$GO"

wait $HPID
wait $IPID

WANT_BYTES=$(awk '/^TOTAL/ { print $2 }' "$TMPD/helper.out")
GOT=$(last_bytes "$TMPD/iotop.out" "$HPID" read)

nonzero "the helper moved bytes" "$WANT_BYTES"
nonzero "iotop reports the read bytes of the helper" "$GOT"

if within "$GOT" "$WANT_BYTES" 3; then
	pass "the read counter matches the bytes moved ($GOT vs $WANT_BYTES)"
else
	fail "the read counter matches the bytes moved ($GOT vs $WANT_BYTES)"
fi

# the helper writes the few bytes of its report; that must not look like traffic
GOTW=$(last_bytes "$TMPD/iotop.out" "$HPID" write)
check "a reader shows almost no writes (got $GOTW)" test "$GOTW" -lt 4096

check "the header line is printed" grep -q "Total DISK READ" "$TMPD/iotop.out"

rm -f "$TMPD/data.bin"
finish
