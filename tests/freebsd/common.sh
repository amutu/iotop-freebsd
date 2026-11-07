#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Shared helpers of the FreeBSD tests. Every test sources this file, runs one
# group of checks and exits with 0 only when all of them passed.

IOTOP="${IOTOP:-./iotop}"
TESTS=0
FAILS=0
TMPD="${TMPD:-$(mktemp -d /tmp/iotop-test.XXXXXX)}"

pass() {
	TESTS=$((TESTS+1))
	printf '  ok   %s\n' "$*"
}

fail() {
	TESTS=$((TESTS+1))
	FAILS=$((FAILS+1))
	printf '  FAIL %s\n' "$*"
}

# check <description> <command...>
check() {
	_d=$1
	shift
	if "$@" >/dev/null 2>&1; then pass "$_d"; else fail "$_d"; fi
}

# nonzero <description> <value>  -- the value must not be empty or zero
nonzero() {
	if [ -n "$2" ] && [ "$2" != "0" ] && [ "$2" != "-1" ]; then
		pass "$1 ($2)"
	else
		fail "$1 (got '$2')"
	fi
}

# humanized <num> <unit> -> bytes
humanized() {
	awk -v n="$1" -v u="$2" 'BEGIN{
		s=1
		if (u=="K") s=1024
		else if (u=="M") s=1048576
		else if (u=="G") s=1073741824
		else if (u=="T") s=1099511627776
		printf "%.0f", n*s
	}'
}

# last_bytes <file> <tid> read|write -- the last value printed for that tid, in bytes, -1 when absent
last_bytes() {
	awk -v t="$2" -v w="$3" '
	function mul(u){ if(u=="K")return 1024; if(u=="M")return 1048576; if(u=="G")return 1073741824; if(u=="T")return 1099511627776; return 1 }
	($1+0)==(t+0) { v=(w=="read")?$4*mul($5):$6*mul($7); last=v; found=1 }
	END { if (found) printf "%.0f\n", last; else print "-1" }' "$1"
}

# sum_bytes <file> <read|write> -- the sum over all the task rows
sum_bytes() {
	awk -v w="$2" '
	function mul(u){ if(u=="K")return 1024; if(u=="M")return 1048576; if(u=="G")return 1073741824; if(u=="T")return 1099511627776; return 1 }
	$1 ~ /^[0-9]+$/ { s+=(w=="read")?$4*mul($5):$6*mul($7) }
	END { printf "%.0f\n", s }' "$1"
}

# rows <file> -- the number of task rows, the header lines have no numeric first column
count_rows() {
	awk '$1 ~ /^[0-9]+$/ { n++ } END { print n+0 }' "$1"
}

# within <got> <want> <percent>
within() {
	awk -v g="$1" -v w="$2" -v p="$3" 'BEGIN{
		if (w<=0) exit 1
		d=g-w
		if (d<0) d=-d
		exit (d <= w*p/100) ? 0 : 1
	}'
}

# need_root -- the byte counters come from dtrace
need_root() {
	if [ "$(id -u)" -ne 0 ]; then
		printf '  SKIP: the FreeBSD backend needs root for dtrace\n'
		exit 77
	fi
}

finish() {
	printf '  %d checks, %d failed\n' "$TESTS" "$FAILS"
	[ "$FAILS" -eq 0 ] || exit 1
	exit 0
}

# make_file <path> <mib>
make_file() {
	dd if=/dev/zero of="$1" bs=1m count="$2" 2>/dev/null
}
