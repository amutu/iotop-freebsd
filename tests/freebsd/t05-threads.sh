#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T05 - every thread is accounted on its own and the process view adds them up

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1
need_root

IOTEST="${IOTEST:-./iotest}"
[ -x "$IOTEST" ] || { printf '  SKIP: %s is missing\n' "$IOTEST"; exit 77; }

make_file "$TMPD/data.bin" 16

NT=4
GO="$TMPD/go"
"$IOTEST" r "$TMPD/data.bin" 32000000 3 $NT 3 "$GO" > "$TMPD/helper.out" 2>&1 &
HPID=$!
i=0
while [ ! -s "$TMPD/helper.out" ] && [ $i -lt 100 ]; do
	sleep 0.05
	i=$((i+1))
done

# no -p for the thread view: --pid keeps the row of the process leader only,
# and this test is about the threads, which are matched by the id the helper
# prints for each of them
"$IOTOP" -b -n 11 -d 1 -a > "$TMPD/threads.out" 2>&1 &
IP1=$!
"$IOTOP" -b -n 11 -d 1 -a -P -p "$HPID" > "$TMPD/proc.out" 2>&1 &
IP2=$!
sleep 1.5
: > "$GO"

wait $HPID
wait $IP1
wait $IP2

WANT_BYTES=$(awk '/^TOTAL/ { print $2 }' "$TMPD/helper.out")
NTHR=$(awk '/^THREAD/ { n++ } END { print n+0 }' "$TMPD/helper.out")
check "the helper ran $NT threads (got $NTHR)" test "$NTHR" -eq "$NT"

BAD=0
SUM=0
while read -r tag tid want; do
	[ "$tag" = "THREAD" ] || continue
	got=$(last_bytes "$TMPD/threads.out" "$tid" read)
	if ! within "$got" "$want" 5; then
		BAD=$((BAD+1))
		printf '       thread %s: iotop %s, moved %s\n' "$tid" "$got" "$want"
	fi
	SUM=$((SUM+got))
done < "$TMPD/helper.out"

check "every thread counter matches the bytes it moved ($BAD wrong)" test "$BAD" -eq 0
if within "$SUM" "$WANT_BYTES" 5; then
	pass "the thread counters add up to the total ($SUM vs $WANT_BYTES)"
else
	fail "the thread counters add up to the total ($SUM vs $WANT_BYTES)"
fi

PROC=$(last_bytes "$TMPD/proc.out" "$HPID" read)
if within "$PROC" "$WANT_BYTES" 5; then
	pass "the process view aggregates the threads ($PROC vs $WANT_BYTES)"
else
	fail "the process view aggregates the threads ($PROC vs $WANT_BYTES)"
fi

rm -f "$TMPD/data.bin"
finish
