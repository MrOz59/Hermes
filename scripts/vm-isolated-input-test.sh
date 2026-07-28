#!/usr/bin/env bash
# Validate that Hermes' uinput phys tags become distinct udev/libinput seats in
# a disposable virtme-ng guest. No host udev rule is changed.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_ROOT="$(mktemp -d /tmp/hermes-input-seats.XXXXXX)"
RULE_PATH="/run/udev/rules.d/60-hermes.rules"
PROBE_PID=""

cleanup()
{
	if [ -n "$PROBE_PID" ]; then
		kill -TERM "$PROBE_PID" 2>/dev/null || true
		wait "$PROBE_PID" 2>/dev/null || true
	fi
	rm -f -- "$RULE_PATH"
	udevadm control --reload-rules 2>/dev/null || true
	rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

fail()
{
	printf 'FAIL: %s\n' "$1" >&2
	[ -f "$TEST_ROOT/probe.log" ] && cat "$TEST_ROOT/probe.log" >&2
	exit 1
}

[ "$(id -u)" -eq 0 ] ||
	{ printf 'run as root inside a disposable VM\n' >&2; exit 1; }
command -v cc >/dev/null || { printf 'cc is required\n' >&2; exit 1; }
command -v udevadm >/dev/null || { printf 'udevadm is required\n' >&2; exit 1; }

modprobe uinput
mkdir -p "$(dirname "$RULE_PATH")"
cp "$REPO/src_assets/linux/misc/60-hermes.rules" "$RULE_PATH"
udevadm control --reload-rules

cc -O2 -Wall -Wextra \
	"$REPO/scripts/hermes-uinput-seat-probe.c" \
	-o "$TEST_ROOT/hermes-uinput-seat-probe"
"$TEST_ROOT/hermes-uinput-seat-probe" >"$TEST_ROOT/probe.log" 2>&1 &
PROBE_PID="$!"

for attempt in $(seq 1 100); do
	[ "$(wc -l <"$TEST_ROOT/probe.log")" -ge 2 ] && break
	kill -0 "$PROBE_PID" 2>/dev/null ||
		fail "uinput probe exited before creating both devices"
	sleep 0.02
done
[ "$(wc -l <"$TEST_ROOT/probe.log")" -ge 2 ] ||
	fail "timed out waiting for uinput devices"

udevadm settle
for instance in 1 2; do
	sysname="$(awk -F= -v key="hermes-kms-$instance" '$1 == key { print $2; exit }' "$TEST_ROOT/probe.log")"
	[ -n "$sysname" ] || fail "missing sysname for hermes-kms-$instance"
	[ -e "/sys/class/input/$sysname" ] ||
		fail "missing sysfs device /sys/class/input/$sysname"

	seat="$(udevadm info --query=property --path="/sys/class/input/$sysname" |
		awk -F= '$1 == "ID_SEAT" { print $2; exit }')"
	[ "$seat" = "hermes-kms-$instance" ] ||
		fail "$sysname has seat ${seat:-<none>}, expected hermes-kms-$instance"

	event_path=""
	for candidate in "/sys/class/input/$sysname"/event*; do
		[ -e "$candidate" ] || continue
		event_path="$candidate"
		break
	done
	[ -n "$event_path" ] || fail "$sysname has no event node"
	event_seat="$(udevadm info --query=property --path="$event_path" |
		awk -F= '$1 == "ID_SEAT" { print $2; exit }')"
	[ "$event_seat" = "hermes-kms-$instance" ] ||
		fail "$(basename "$event_path") did not inherit hermes-kms-$instance"
done

printf '%s\n' \
	"PASS: isolated uinput devices received distinct udev/libinput seats" \
	"seat_1=hermes-kms-1" \
	"seat_2=hermes-kms-2"
