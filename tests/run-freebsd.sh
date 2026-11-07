#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs the FreeBSD test suite. Must be started from the source tree root,
# as root (the byte counters are read through dtrace).
#
#	sh tests/run-freebsd.sh
#
# Environment:
#	IOTOP	the binary to test, ./iotop by default
#	ONLY	run a single test file, e.g. ONLY=t03-read.sh

cd "$(dirname "$0")/.." || exit 1
IOTOP_DIR=$(pwd)
IOTOP="${IOTOP:-$IOTOP_DIR/iotop}"
IOTEST="${IOTEST:-$IOTOP_DIR/iotest}"
export IOTOP_DIR IOTOP IOTEST

if [ ! -x "$IOTOP" ]; then
	printf 'run-freebsd: no %s, build it first (gmake)\n' "$IOTOP"
	exit 1
fi

printf 'building the io helper\n'
cc -O2 -o "$IOTEST" tests/freebsd/iotest.c -lpthread || {
	printf 'run-freebsd: can not build the io helper\n'
	exit 1
}

if [ "$(id -u)" -ne 0 ]; then
	printf 'run-freebsd: the tests need root for dtrace\n'
	exit 77
fi

RAN=0
FAILED=0
SKIPPED=0
FAILED_NAMES=""

for t in tests/freebsd/t*.sh; do
	[ -n "${ONLY:-}" ] && [ "$(basename "$t")" != "$ONLY" ] && continue
	printf '\n== %s\n' "$(basename "$t")"
	sh "$t"
	rc=$?
	RAN=$((RAN+1))
	if [ $rc -eq 77 ]; then
		SKIPPED=$((SKIPPED+1))
	elif [ $rc -ne 0 ]; then
		FAILED=$((FAILED+1))
		FAILED_NAMES="$FAILED_NAMES $(basename "$t")"
	fi
done

printf '\n== summary: %d files, %d failed, %d skipped\n' "$RAN" "$FAILED" "$SKIPPED"
[ -n "$FAILED_NAMES" ] && printf '   failed:%s\n' "$FAILED_NAMES"

[ "$FAILED" -eq 0 ] || exit 1
exit 0
