#!/usr/bin/env bash
# Exercise hermes-kms-card-broker against a fake configfs and sysfs tree.
#
# Everything except the final CREATE runs without root and without the driver:
# authorization, the allow file, request parsing, ownership of a card name, seat
# allocation and the rollback when a card cannot be configured. A real CREATE
# needs configfs to materialise the card's attributes on mkdir, which only the
# kernel does, so this asserts the rollback path instead and leaves the happy
# path to a machine with the module loaded.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BROKER="${1:-$REPO/build/hermes-kms-card-broker}"

if [ ! -x "$BROKER" ]; then
	printf 'broker binary not found: %s\n' "$BROKER" >&2
	printf 'build it with: cmake --build build --target hermes-kms-card-broker\n' >&2
	exit 2
fi

ROOT="$(mktemp -d)"
trap 'kill "${BROKER_PID:-}" 2>/dev/null || true; rm -rf "$ROOT"' EXIT

CONFIGFS="$ROOT/configfs"
PLATFORM="$ROOT/platform"
SOCKET="$ROOT/broker.sock"
ALLOW="$ROOT/allow"
mkdir -p "$CONFIGFS" "$PLATFORM"
: >"$ALLOW"

# A statically configured pool card, so allocation has something to avoid.
mkdir -p "$PLATFORM/hermes-kms.1"
printf 'session\n' >"$PLATFORM/hermes-kms.1/hermes_kms_role"
printf '1\n' >"$PLATFORM/hermes-kms.1/hermes_kms_session_index"

start_broker() {
	"$BROKER" --socket "$SOCKET" --allow-file "$ALLOW" --configfs "$CONFIGFS" \
		--platform "$PLATFORM" --lock-file "$ROOT/lock" --max-cards 2 &
	BROKER_PID=$!
	for _ in $(seq 1 50); do
		[ -S "$SOCKET" ] && return 0
		sleep 0.05
	done
	printf 'broker did not start\n' >&2
	exit 1
}

ask() {
	python3 - "$SOCKET" "$1" <<-'PY'
		import socket, sys
		with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
		    client.connect(sys.argv[1])
		    client.sendall((sys.argv[2] + "\n").encode())
		    client.shutdown(socket.SHUT_WR)
		    print(client.recv(4096).decode().strip())
	PY
}

expect() {
	local description="$1" request="$2" pattern="$3" reply
	reply="$(ask "$request")"
	if [[ "$reply" != $pattern ]]; then
		printf 'FAIL %s\n  request:  %s\n  expected: %s\n  got:      %s\n' \
			"$description" "$request" "$pattern" "$reply" >&2
		exit 1
	fi
	printf 'ok   %s\n' "$description"
}

start_broker

# An empty allow file denies everyone, including the user running the test.
expect 'an unlisted uid is refused' 'LIST' 'ERR denied*'

printf '# comment\n%s\n' "$(id -u)" >"$ALLOW"

expect 'a listed uid is served' 'LIST' 'OK'
expect 'sweeping nothing removes nothing' 'SWEEP' 'OK 0'
expect 'an unknown request is rejected' 'HELLO' 'ERR request*'
expect "another user's card cannot be removed" "REMOVE hermes-u0-1" 'ERR denied*'
expect 'a path is not a card name' "REMOVE ../../etc/passwd" 'ERR denied*'

# Seat 1 is taken by the static card above, so allocation must land on 2 and
# then fail to configure it, because a plain directory has no configfs
# attributes. The rollback is what leaves no card behind.
expect 'a card is allocated past the static pool' 'CREATE' 'ERR create could not configure*'
if [ -n "$(ls -A "$CONFIGFS")" ]; then
	printf 'FAIL a failed CREATE left a card behind: %s\n' "$(ls -A "$CONFIGFS")" >&2
	exit 1
fi
printf 'ok   a failed CREATE leaves no card behind\n'

# Fill every seat and the answer changes from "could not configure" to
# "exhausted", which is the allocator refusing rather than configfs failing.
for index in $(seq 2 8); do
	mkdir -p "$PLATFORM/hermes-kms.$index"
	printf '%s\n' "$index" >"$PLATFORM/hermes-kms.$index/hermes_kms_session_index"
done
expect 'a full pool is reported as exhausted' 'CREATE' 'ERR exhausted*'

# Ownership is by name, so a card the user "has" is listed and swept.
mkdir -p "$CONFIGFS/hermes-u$(id -u)-3" "$CONFIGFS/hermes-u0-4" "$CONFIGFS/static-card"
expect "only this user's cards are listed" 'LIST' "OK hermes-u$(id -u)-3"
expect "sweeping removes only this user's cards" 'SWEEP' 'OK 1'
if [ ! -d "$CONFIGFS/hermes-u0-4" ] || [ ! -d "$CONFIGFS/static-card" ]; then
	printf "FAIL sweep removed a card it does not own\n" >&2
	exit 1
fi
printf "ok   sweep left other cards alone\n"

printf '\nall card-broker checks passed\n'
