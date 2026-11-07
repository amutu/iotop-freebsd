#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T07 - a signal stops iotop and leaves no dtrace behind

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1
need_root

# a zombie is not alive any more, but kill -0 says it is
alive() {
	s=$(ps -o state= -p "$1" 2>/dev/null)
	[ -n "$s" ] && [ "$s" != "Z" ]
}

wait_gone() {
	_i=0
	while alive "$1" && [ $_i -lt 50 ]; do
		sleep 0.1
		_i=$((_i+1))
	done
	alive "$1"
}

BEFORE=$(pgrep -f 'dtrace.*-q -n' 2>/dev/null | wc -l | tr -d ' ')

"$IOTOP" -b -n 100 -d 1 > "$TMPD/run.out" 2>&1 &
IPID=$!
sleep 2

DURING=$(pgrep -f 'dtrace.*-q -n' 2>/dev/null | wc -l | tr -d ' ')
if [ "$DURING" -gt "$BEFORE" ]; then
	pass "the dtrace byte counter runs while iotop does"
else
	fail "the dtrace byte counter runs while iotop does ($BEFORE -> $DURING)"
fi

kill -INT $IPID 2>/dev/null
if wait_gone $IPID; then
	fail "SIGINT stops iotop within 5 s"
	kill -9 $IPID 2>/dev/null
else
	pass "SIGINT stops iotop within 5 s"
fi

sleep 2 # dtrace notices the closed pipe on its next write
AFTER=$(pgrep -f 'dtrace.*-q -n' 2>/dev/null | wc -l | tr -d ' ')
check "no dtrace child is left behind ($BEFORE -> $AFTER)" test "$AFTER" -eq "$BEFORE"

check "the cycles before the signal were printed" grep -q "Total DISK READ" "$TMPD/run.out"

"$IOTOP" -b -n 100 -d 1 > "$TMPD/run2.out" 2>&1 &
IPID=$!
sleep 1
kill -TERM $IPID 2>/dev/null
wait_gone $IPID
sleep 2
AFTER2=$(pgrep -f 'dtrace.*-q -n' 2>/dev/null | wc -l | tr -d ' ')
check "no dtrace child is left after SIGTERM ($BEFORE -> $AFTER2)" test "$AFTER2" -eq "$BEFORE"

# the exit code of a normal end of the iterations must be 0
"$IOTOP" -b -n 2 -d 1 >/dev/null 2>&1
check "-n 2 exits with 0" test $? -eq 0

finish
