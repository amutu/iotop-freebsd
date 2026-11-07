#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T11 - the death of the dtrace backend is reported, the iotop process stays up

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1
need_root

alive() {
	s=$(ps -o state= -p "$1" 2>/dev/null)
	[ -n "$s" ] && [ "$s" != "Z" ]
}

"$IOTOP" -b -n 100 -d 1 > "$TMPD/run.out" 2> "$TMPD/run.err" &
IPID=$!
sleep 2

DTPID=$(pgrep -f 'dtrace.*-q -n' 2>/dev/null | head -1)
nonzero "the dtrace byte counter is running" "$DTPID"

kill -9 "$DTPID" 2>/dev/null
sleep 3 # the next cycles see the closed pipe and report the death

if alive $IPID; then
	pass "iotop survives the death of the dtrace backend"
else
	fail "iotop survives the death of the dtrace backend"
fi

check "the frozen counters are reported on stderr" grep -q "no longer grow" "$TMPD/run.err"

kill -TERM $IPID 2>/dev/null
_i=0
while alive $IPID && [ $_i -lt 50 ]; do
	sleep 0.1
	_i=$((_i+1))
done
alive $IPID && fail "SIGTERM stops iotop after the backend died" || pass "SIGTERM stops iotop after the backend died"

finish
