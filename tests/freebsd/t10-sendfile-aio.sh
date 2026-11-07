#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# T10 - the sendfile and the asynchronous counters agree with the bytes moved

. "$(dirname "$0")/common.sh"

cd "${IOTOP_DIR:-.}" || exit 1
need_root

IOTEST="${IOTEST:-./iotest}"
[ -x "$IOTEST" ] || { printf '  SKIP: %s is missing\n' "$IOTEST"; exit 77; }

make_file "$TMPD/data.bin" 32

# a sendfile call on loopback is slow whatever the cache state is, so the
# sampler runs until the helper is done instead of a fixed number of cycles
run_load() {
	_mode=$1
	_out=$2
	GO="$TMPD/go"
	rm -f "$GO"
	"$IOTEST" "$_mode" "$TMPD/data.bin" 20000000 3 3 "$GO" > "$_out.helper" 2>&1 &
	HPID=$!
	i=0
	while [ ! -s "$_out.helper" ] && [ $i -lt 100 ]; do
		sleep 0.05
		i=$((i+1))
	done

	"$IOTOP" -b -n 600 -d 1 -p "$HPID" -a > "$_out.iotop" 2>&1 &
	IPID=$!
	sleep 1.5
	: > "$GO"

	wait $HPID
	# the helper lingers on after its report, so a few more samples carry
	# the final counters; a gentle TERM makes the batch view flush them
	sleep 2
	kill -TERM $IPID 2>/dev/null
	wait $IPID 2>/dev/null
}

# sendfile reports the bytes in sbytes, not in the return value; the write
# counter must still match what the helper sent over the loopback socket
run_load s "$TMPD/sf"

WANT=$(awk '/^TOTAL/ { print $3 }' "$TMPD/sf.helper")
GOT=$(last_bytes "$TMPD/sf.iotop" "$HPID" write)

nonzero "the helper sent bytes over the socket" "$WANT"
nonzero "iotop reports the sendfile bytes of the helper" "$GOT"

if within "$GOT" "$WANT" 3; then
	pass "the write counter matches the sendfile bytes ($GOT vs $WANT)"
else
	fail "the write counter matches the sendfile bytes ($GOT vs $WANT)"
fi

# the peer is drained with recv, which the backend does not count
GOTR=$(last_bytes "$TMPD/sf.iotop" "$HPID" read)
check "a sender shows almost no reads (got $GOTR)" test "$GOTR" -lt 4096

# the asynchronous pair reports the bytes through aio_return, not through the
# return value of the submission; both counters must match its reports
run_load a "$TMPD/aio"

WANTR=$(awk '/^TOTAL/ { print $2 }' "$TMPD/aio.helper")
WANTW=$(awk '/^TOTAL/ { print $3 }' "$TMPD/aio.helper")
GOTR=$(last_bytes "$TMPD/aio.iotop" "$HPID" read)
GOTW=$(last_bytes "$TMPD/aio.iotop" "$HPID" write)

nonzero "the helper read bytes asynchronously" "$WANTR"
if within "$GOTR" "$WANTR" 3; then
	pass "the read counter matches the aio_return bytes ($GOTR vs $WANTR)"
else
	fail "the read counter matches the aio_return bytes ($GOTR vs $WANTR)"
fi

nonzero "the helper wrote bytes asynchronously" "$WANTW"
if within "$GOTW" "$WANTW" 3; then
	pass "the write counter matches the aio_return bytes ($GOTW vs $WANTW)"
else
	fail "the write counter matches the aio_return bytes ($GOTW vs $WANTW)"
fi

rm -f "$TMPD/data.bin" "$TMPD/data.bin.aw"
finish
