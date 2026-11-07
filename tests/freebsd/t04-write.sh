#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T04 - the write counters of a known task agree with the bytes it moved

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1
need_root

IOTEST="${IOTEST:-./iotest}"
[ -x "$IOTEST" ] || { printf '  SKIP: %s is missing\n' "$IOTEST"; exit 77; }

GO="$TMPD/go"
"$IOTEST" w "$TMPD/out.bin" 40000000 3 1 3 "$GO" > "$TMPD/helper.out" 2>&1 &
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

WANT_BYTES=$(awk '/^TOTAL/ { print $3 }' "$TMPD/helper.out")
GOT=$(last_bytes "$TMPD/iotop.out" "$HPID" write)
GOTR=$(last_bytes "$TMPD/iotop.out" "$HPID" read)

nonzero "the helper moved bytes" "$WANT_BYTES"
nonzero "iotop reports the write bytes of the helper" "$GOT"

if within "$GOT" "$WANT_BYTES" 3; then
	pass "the write counter matches the bytes moved ($GOT vs $WANT_BYTES)"
else
	fail "the write counter matches the bytes moved ($GOT vs $WANT_BYTES)"
fi

check "a writer shows almost no reads (got $GOTR)" test "$GOTR" -lt 65536

rm -f "$TMPD/out.bin"
finish
