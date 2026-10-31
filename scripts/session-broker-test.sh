#!/usr/bin/env bash
# Exercise hermes-session-broker end to end, the way it is actually shipped.
#
# This one cannot be faked the way the card broker's test fakes configfs: the
# broker's whole job is to create real accounts and ask systemd for real units,
# so the things worth asserting - a home nobody else can read, a unit running
# under another uid, a polkit rule that denies that uid and nobody else - only
# exist on a machine where it really happened. Run it in a disposable guest:
#
#   vng --systemd --run /usr/lib/modules/$(uname -r)/vmlinuz
#   # then, inside:  ./scripts/session-broker-test.sh
#
# It installs the packaged units, sysusers file and polkit rule, so it is not
# safe on a machine you care about.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BROKER="${1:-$REPO/build/hermes-session-broker}"
CLIENT_A="5f3ab1c2-0000-4000-8000-000000000001"
CLIENT_B="5f3ab1c2-0000-4000-8000-000000000002"
SOCKET="/run/hermes/session-broker.sock"
STATE="/var/lib/hermes/session-users.map"
HOMES="/var/lib/hermes/sessions"
INPUT_PROBE_PID=""
HID_PROBE_PID=""
SDL_A_PID=""
SDL_B_PID=""
INPUT_TEST_ROOT=""
BROKER_FROZEN=0

cleanup_input_probe() {
	if [ "$BROKER_FROZEN" -eq 1 ]; then systemctl thaw hermes-session-broker.service 2>/dev/null || true; fi
	for pid in "$SDL_A_PID" "$SDL_B_PID"; do
		[ -z "$pid" ] || kill -TERM "$pid" 2>/dev/null || true
	done
	if [ -n "$HID_PROBE_PID" ]; then
		kill -TERM "$HID_PROBE_PID" 2>/dev/null || true
		wait "$HID_PROBE_PID" 2>/dev/null || true
	fi
	if [ -n "$INPUT_PROBE_PID" ]; then
		kill -TERM "$INPUT_PROBE_PID" 2>/dev/null || true
		wait "$INPUT_PROBE_PID" 2>/dev/null || true
	fi
	[ -z "$INPUT_TEST_ROOT" ] || rm -rf -- "$INPUT_TEST_ROOT"
}
trap cleanup_input_probe EXIT

[ "$(id -u)" -eq 0 ] || { printf 'must run as root; this creates accounts\n' >&2; exit 2; }
systemd-detect-virt --vm --quiet || { printf 'must run inside a disposable VM; this installs system units and creates accounts\n' >&2; exit 2; }
[ -x "$BROKER" ] || { printf 'broker not found: %s\n  build: cmake --build build --target hermes-session-broker\n' "$BROKER" >&2; exit 2; }

# virtme-ng shares the host's filesystem as the host user and bind-mounts a few
# files over the overlay, so /etc/shadow, /etc/gshadow and the passwd lock are
# root's and 0600 on the host and unwritable to the guest's root. Every account
# tool then fails on the lock before it does anything. Rebuilt here from the
# databases the guest can read. This is a property of the harness; on a real
# machine the check below is false and none of it runs.
if ! ( : >>/etc/.pwd.lock ) 2>/dev/null; then
	umount /etc/shadow 2>/dev/null || true
	umount /etc/gshadow 2>/dev/null || true
	rm -f /etc/shadow /etc/gshadow /etc/.pwd.lock
	awk -F: '{print $1":!:19000:0:99999:7:::"}' /etc/passwd >/etc/shadow
	awk -F: '{print $1":!::"}' /etc/group >/etc/gshadow
	: >/etc/.pwd.lock
	chmod 600 /etc/shadow /etc/gshadow /etc/.pwd.lock
	printf 'note rebuilt the guest shadow database (virtme-ng harness artifact)\n'
fi

PASS=0
ok() { printf 'ok   %s\n' "$1"; PASS=$((PASS + 1)); }
fail() {
	printf 'FAIL %s\n  %s\n' "$1" "${2:-}" >&2
	if [ -n "$INPUT_TEST_ROOT" ]; then
		for node in /sys/class/input/event* /sys/class/hidraw/hidraw*; do
			[ -e "$node" ] || continue
			properties="$(udevadm info --query=property --path="$node")"
			if grep -q '^ID_SEAT=hermes-kms-' <<<"$properties"; then
				printf '%s\n' "$node" "$properties"
				device="$(sed -n 's/^DEVNAME=//p' <<<"$properties")"
				[ -z "$device" ] || getfacl -cpn "$device"
			fi
		done
		journalctl -u systemd-udevd --no-pager -n 200
	fi
	exit 1
}

ask() {
	python3 - "$SOCKET" "$1" <<-'PY'
		import socket, sys
		with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
		    client.connect(sys.argv[1])
		    client.sendall(sys.argv[2].encode())
		    client.shutdown(socket.SHUT_WR)
		    data = b""
		    while chunk := client.recv(4096):
		        data += chunk
		    print(data.decode().strip())
	PY
}

expect() {
	local description="$1" request="$2" pattern="$3" reply
	reply="$(ask "$request")"
	[[ "$reply" == $pattern ]] || fail "$description" "request:  ${request%%$'\n'*}
  expected: $pattern
  got:      $reply"
	ok "$description"
}

# ---------------------------------------------------------------- install ----
install -Dm755 "$BROKER" /usr/bin/hermes-session-broker
install -Dm644 "$REPO/src_assets/linux/misc/hermes-session-broker.service" \
	/etc/systemd/system/hermes-session-broker.service
install -Dm644 "$REPO/src_assets/linux/misc/hermes-session-broker.socket" \
	/etc/systemd/system/hermes-session-broker.socket
install -Dm644 "$REPO/src_assets/linux/misc/hermes-sysusers.conf" /usr/lib/sysusers.d/hermes.conf
install -Dm644 "$REPO/src_assets/linux/misc/10-hermes-session-deny.rules" \
	/usr/share/polkit-1/rules.d/10-hermes-session-deny.rules
install -Dm644 "$REPO/src_assets/linux/misc/60-hermes.rules" /run/udev/rules.d/60-hermes.rules
install -Dm644 "$REPO/src_assets/linux/misc/99-hermes-session-input.rules" /run/udev/rules.d/99-hermes-session-input.rules
udevadm control --reload-rules
# The unit lists this under ReadWritePaths, and systemd refuses to start a unit
# whose ReadWritePaths does not exist.
mkdir -p /var/lib/hermes
systemd-sysusers
getent group hermes-session >/dev/null || fail 'sysusers did not create the hermes-session group'
ok 'the sysusers file creates the hermes-session group'

printf '# the test runs as root\n0\n' >/etc/hermes/session-broker.allow 2>/dev/null || {
	mkdir -p /etc/hermes && printf '# the test runs as root\n0\n' >/etc/hermes/session-broker.allow
}
chmod 644 /etc/hermes/session-broker.allow

systemctl daemon-reload
systemctl start hermes-session-broker.socket
[ -S "$SOCKET" ] || fail 'the broker socket was not created' "$(systemctl status hermes-session-broker.socket --no-pager 2>&1 | tail -5)"
ok 'the packaged socket unit starts and creates the socket'

# ---------------------------------------------------------------- protocol ---
expect 'an empty map lists nothing'            $'LIST\n'                   'OK'
expect 'an unknown verb is rejected'           $'HELLO\n'                  'ERR request*'
expect 'a malformed client id is rejected'     $'LOOKUP ../../etc/passwd\n' 'ERR request*'
expect 'an unmapped client has no account'     $'LOOKUP '"$CLIENT_A"$'\n'  'ERR notfound*'

# ---------------------------------------------------------------- accounts ---
expect 'the first client is given slot 1' $'ENSURE '"$CLIENT_A"$'\n' "OK hermes-s01 * $HOMES/hermes-s01"
getent passwd hermes-s01 >/dev/null || fail 'ENSURE did not create the account'
ok 'the account exists in passwd'

UID_A="$(id -u hermes-s01)"
[ "$(stat -c '%a %U' "$HOMES/hermes-s01")" = "700 hermes-s01" ] \
	|| fail 'the session home is not private' "$(stat -c '%a %U' "$HOMES/hermes-s01")"
ok 'the session home is 0700 and owned by the session user'

[ "$(stat -c '%a' "$HOMES")" = "711" ] \
	|| fail 'the home root is listable' "$(stat -c '%a' "$HOMES")"
ok 'the home root is traversable but not listable'

case "$(getent passwd hermes-s01 | cut -d: -f7)" in
	*nologin | *false) ok 'the account has no shell' ;;
	*) fail 'the account has a shell' "$(getent passwd hermes-s01)" ;;
esac

id -nG hermes-s01 | tr ' ' '\n' | grep -qx hermes-session || fail 'the account is not in hermes-session'
ok 'the account is in hermes-session'

for group in wheel sudo video input render; do
	if id -nG hermes-s01 | tr ' ' '\n' | grep -qx "$group"; then
		fail "the account is in $group"
	fi
done
ok 'the account is in none of wheel, sudo, video, input or render'

expect 'ENSURE is idempotent'   $'ENSURE '"$CLIENT_A"$'\n' "OK hermes-s01 $UID_A *"
expect 'LOOKUP finds it'        $'LOOKUP '"$CLIENT_A"$'\n' "OK hermes-s01 $UID_A *"

# ---------------------------------------------------------------- sessions ---
expect 'nothing is running yet' $'STATUS '"$CLIENT_A"$'\n' 'OK inactive hermes-session-01.service'

START=$'START '"$CLIENT_A"$'\nARG /usr/bin/sleep\nARG 600\nENV HERMES_TEST=yes\nENV XDG_SEAT=hermes-kms-1\nEND\n'
expect 'START asks systemd for a unit' "$START" 'OK hermes-session-01.service'

for _ in $(seq 50); do
	[ "$(systemctl show -p ActiveState --value hermes-session-01.service)" = active ] && break
	sleep 0.1
done
[ "$(systemctl show -p ActiveState --value hermes-session-01.service)" = active ] \
	|| fail 'the session unit is not active' "$(systemctl status hermes-session-01.service --no-pager 2>&1 | tail -10)"
ok 'the session unit is active'

[ "$(systemctl show -p User --value hermes-session-01.service)" = hermes-s01 ] \
	|| fail 'the unit does not run as the session account'
ok 'the unit runs as the session account'

pgrep -u hermes-s01 -x sleep >/dev/null || fail 'no process is running as the session account'
ok 'the command really runs under the session uid'

[ -d "/run/user/$UID_A" ] || fail 'lingering did not create the runtime directory'
ok 'lingering gave the session a runtime directory'

for _ in $(seq 50); do
	[[ "$(ask $'STATUS '"$CLIENT_A"$'\n')" == 'OK active '* ]] && break
	sleep 0.1
done
expect 'STATUS reports it active' $'STATUS '"$CLIENT_A"$'\n' 'OK active hermes-session-01.service'

# Simulate a slow account-database operation by holding its real mutation lock.
# A second ENSURE must queue, while the active client's liveness remains usable.
SECOND_ACCOUNT="$(python3 - "$SOCKET" "$CLIENT_A" "$CLIENT_B" <<-'PY'
	import fcntl, select, socket, sys
	def connect(request, timeout):
	    peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	    peer.settimeout(timeout)
	    peer.connect(sys.argv[1])
	    peer.sendall(request.encode())
	    peer.shutdown(socket.SHUT_WR)
	    return peer
	def response(peer):
	    text = b''
	    while b'\n' not in text:
	        chunk = peer.recv(4096)
	        assert chunk, 'broker closed before replying'
	        text += chunk
	    return text.decode().strip()
	with open('/run/hermes/session-broker.lock', 'r+') as lock:
	    fcntl.flock(lock, fcntl.LOCK_EX)
	    with connect('ENSURE ' + sys.argv[3] + '\n', 180) as pending:
	        assert not select.select([pending], [], [], 0.05)[0], 'ENSURE did not queue behind the mutation lock'
	        with connect('STATUS ' + sys.argv[2] + '\n', 2) as status:
	            assert response(status) == 'OK active hermes-session-01.service', 'liveness was blocked or lost'
	        fcntl.flock(lock, fcntl.LOCK_UN)
	        print(response(pending))
PY
)"
[[ "$SECOND_ACCOUNT" == "OK hermes-s02 "* ]] || fail 'the second account was not provisioned' "$SECOND_ACCOUNT"
ok 'a second client gets slot 2'
ok 'an active session remains queryable during queued account provisioning'
expect 'LIST reports both' $'LIST\n' "OK $CLIENT_A=hermes-s01 $CLIENT_B=hermes-s02"
expect 'no Wayland socket yet'    $'SOCKET '"$CLIENT_A"$'\n' 'ERR notfound*'

python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind('/run/user/$UID_A/wayland-1')
s.listen(1)
" && expect 'a Wayland socket is found by name' $'SOCKET '"$CLIENT_A"$'\n' 'OK wayland-1'

SCOPED=$'START '"$CLIENT_A"$'\nARG /usr/bin/sleep\nARG 600\nSCOPE app\nEND\n'
expect 'a scoped unit is a second unit' "$SCOPED" 'OK hermes-session-01-app.service'
[ "$(systemctl show -p PartOf --value hermes-session-01-app.service)" = hermes-session-01.service ] \
	|| fail 'the scoped unit is not bound to the session'
ok 'the scoped unit is PartOf the session unit'

expect 'a bad SCOPE is rejected' $'START '"$CLIENT_A"$'\nARG /usr/bin/true\nSCOPE App\nEND\n' 'ERR request*'
expect 'a relative command is rejected' $'START '"$CLIENT_A"$'\nARG true\nEND\n' 'ERR request*'

# The broker itself, not the harness, must grant the two accounts access.
# Keep the root-owned producer alive through STOP to verify actual revocation.
expect 'a duplicate input lease is rejected' "$START" 'ERR input*'
expect 'a malformed seat is rejected' $'START '"$CLIENT_B"$'\nARG /usr/bin/true\nENV XDG_SEAT=../../dev/input\nEND\n' 'ERR request*'
expect 'a second client cannot take an occupied seat' $'START '"$CLIENT_B"$'\nARG /usr/bin/sleep\nARG 600\nENV XDG_SEAT=hermes-kms-1\nEND\n' 'ERR input*'
expect 'a second session registers its own seat' $'START '"$CLIENT_B"$'\nARG /usr/bin/sleep\nARG 600\nENV XDG_SEAT=hermes-kms-2\nEND\n' 'OK hermes-session-02.service'
expect 'a scoped app cannot cross to another seat' $'START '"$CLIENT_B"$'\nARG /usr/bin/true\nSCOPE wrong\nENV XDG_SEAT=hermes-kms-1\nEND\n' 'ERR input*'

UID_B="$(id -u hermes-s02)"
INPUT_TEST_ROOT="$(mktemp -d /tmp/hermes-broker-input.XXXXXX)"
chmod 755 "$INPUT_TEST_ROOT"
cc -O2 -Wall -Wextra "$REPO/scripts/hermes-uinput-seat-probe.c" -o "$INPUT_TEST_ROOT/probe"
cc -O2 -Wall -Wextra -Werror "$REPO/scripts/hermes-uhid-seat-probe.c" -o "$INPUT_TEST_ROOT/hid-probe"
cc -O2 -Wall -Wextra -Werror "$REPO/scripts/hermes-sdl-hotplug-probe.c" \
	$(pkg-config --cflags --libs sdl2) -o "$INPUT_TEST_ROOT/sdl-probe"

start_sdl_probes() {
	runuser -u hermes-s01 -- "$INPUT_TEST_ROOT/sdl-probe" \
		hermes-kms-1 hermes-kms-2 >"$INPUT_TEST_ROOT/sdl-a.log" 2>&1 &
	SDL_A_PID="$!"
	runuser -u hermes-s02 -- "$INPUT_TEST_ROOT/sdl-probe" \
		hermes-kms-2 hermes-kms-1 >"$INPUT_TEST_ROOT/sdl-b.log" 2>&1 &
	SDL_B_PID="$!"
	for _ in $(seq 100); do
		if grep -qx ready "$INPUT_TEST_ROOT/sdl-a.log" && grep -qx ready "$INPUT_TEST_ROOT/sdl-b.log"; then return; fi
		sleep 0.02
	done
	fail 'SDL probes did not initialize before device creation'
}

wait_sdl_probes() {
	if ! wait "$SDL_A_PID" || ! wait "$SDL_B_PID"; then
		fail 'SDL missed a controller created after initialization' "$(cat "$INPUT_TEST_ROOT/sdl-a.log" "$INPUT_TEST_ROOT/sdl-b.log")"
	fi
	SDL_A_PID=""
	SDL_B_PID=""
	ok 'running SDL clients discover only their own hotplugged controller'
}

start_hid_probe() {
	"$INPUT_TEST_ROOT/hid-probe" >"$INPUT_TEST_ROOT/hid-probe.log" 2>&1 &
	HID_PROBE_PID="$!"
	HID_A=""
	HID_B=""
	for _ in $(seq 100); do
		kill -0 "$HID_PROBE_PID" 2>/dev/null || fail 'the UHID probe exited' "$(cat "$INPUT_TEST_ROOT/hid-probe.log")"
		for node in /sys/class/hidraw/hidraw*; do
			[ -f "$node/device/uevent" ] || continue
			if grep -qx 'HID_PHYS=hermes-kms-1' "$node/device/uevent"; then HID_A="/dev/$(basename "$node")"; fi
			if grep -qx 'HID_PHYS=hermes-kms-2' "$node/device/uevent"; then HID_B="/dev/$(basename "$node")"; fi
		done
		[ -n "$HID_A" ] && [ -n "$HID_B" ] && return
		sleep 0.05
	done
	fail 'the kernel did not expose both session hidraw nodes'
}

start_input_probe() {
	"$INPUT_TEST_ROOT/probe" >"$INPUT_TEST_ROOT/probe.log" 2>&1 &
	INPUT_PROBE_PID="$!"
	for _ in $(seq 100); do
		[ "$(wc -l <"$INPUT_TEST_ROOT/probe.log")" -ge 2 ] && break
		kill -0 "$INPUT_PROBE_PID" 2>/dev/null || fail 'the uinput probe exited' "$(cat "$INPUT_TEST_ROOT/probe.log")"
		sleep 0.02
	done
	NODE_A=""
	NODE_B=""
	for instance in 1 2; do
		local sysname node
		sysname="$(awk -F= -v key="hermes-kms-$instance" '$1 == key {print $2; exit}' "$INPUT_TEST_ROOT/probe.log")"
		[ -n "$sysname" ] || fail 'the probe did not report both devices'
		for node in "/sys/class/input/$sysname"/event*; do
			[ -e "$node" ] || continue
			if [ "$instance" -eq 1 ]; then NODE_A="/dev/input/$(basename "$node")"; else NODE_B="/dev/input/$(basename "$node")"; fi
			break
		done
	done
	[ -n "$NODE_A" ] && [ -n "$NODE_B" ] || fail 'the kernel did not create both event nodes'
}

can_open_input() {
	runuser -u "$1" -- python3 - "$2" <<-'PY'
		import os, sys
		try:
		    fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
		except (PermissionError, FileNotFoundError):
		    raise SystemExit(1)
		os.close(fd)
	PY
}

wait_input_access() {
	for _ in $(seq 100); do
		if can_open_input hermes-s01 "$NODE_A" && can_open_input hermes-s02 "$NODE_B" && \
		   can_open_input hermes-s01 "$HID_A" && can_open_input hermes-s02 "$HID_B"; then return; fi
		sleep 0.1
	done
	fail 'the broker did not grant the two accounts their own input nodes' "$(journalctl -u hermes-session-broker --no-pager -n 30)"
}

udevadm control --log-level=debug
# The asynchronous reconciliation loop cannot rescue these add events. The
# production udev helper must grant access before already-running SDL sees them.
systemctl is-active --quiet hermes-session-broker.service || fail 'the broker is not active'
BROKER_FROZEN=1
# Freeze every worker, not just MainPID. The udev helper runs in udev's unit
# and must independently authorize the new nodes before their add broadcasts.
flock --exclusive --timeout 5 /run/hermes/session-broker.lock.input systemctl freeze hermes-session-broker.service
start_sdl_probes
start_input_probe
start_hid_probe
udevadm settle --timeout=10
wait_input_access
wait_sdl_probes
systemctl thaw hermes-session-broker.service
BROKER_FROZEN=0
ok 'udev grants hotplug access while broker reconciliation is paused'
! can_open_input hermes-s01 "$NODE_B" || fail 'client A can open client B input'
! can_open_input hermes-s02 "$NODE_A" || fail 'client B can open client A input'
! can_open_input hermes-s01 "$HID_B" || fail 'client A can open client B hidraw'
! can_open_input hermes-s02 "$HID_A" || fail 'client B can open client A hidraw'
ok 'the broker grants only each account own virtual input nodes'

kill -TERM "$INPUT_PROBE_PID"
wait "$INPUT_PROBE_PID"
INPUT_PROBE_PID=""
kill -TERM "$HID_PROBE_PID"
wait "$HID_PROBE_PID"
HID_PROBE_PID=""
for _ in $(seq 100); do
	if [ ! -e "$NODE_A" ] && [ ! -e "$NODE_B" ] && [ ! -e "$HID_A" ] && [ ! -e "$HID_B" ]; then break; fi
	sleep 0.02
done
[ ! -e "$HID_A" ] && [ ! -e "$HID_B" ] || fail 'old UHID devices survived producer shutdown'
start_sdl_probes
start_input_probe
start_hid_probe
wait_input_access
wait_sdl_probes
ok 'hotplugged replacement devices receive their account ACLs'

setfacl -x "u:$UID_A" "$NODE_A"
setfacl -x "u:$UID_A" "$HID_A"
systemctl restart hermes-session-broker.service
wait_input_access
! can_open_input hermes-s01 "$NODE_B" || fail 'a broker restart crossed input seats'
ok 'persisted leases recover correct access after a broker restart'

# ------------------------------------------------------------------ polkit ---
systemctl start polkit 2>/dev/null || true
if command -v pkcheck >/dev/null && systemctl is-active --quiet polkit; then
	set +e
	systemd-run --quiet --uid=hermes-s01 --pipe --wait --collect \
		/usr/bin/pkcheck --action-id org.freedesktop.login1.reboot --process $$ >/dev/null 2>&1
	DENIED=$?
	set -e
	[ "$DENIED" -ne 0 ] || fail 'polkit authorized a hermes-session account'
	ok 'polkit refuses the session account'

	if journalctl -b -t polkitd --no-pager 2>/dev/null | grep -q 'Error evaluating'; then
		fail 'the polkit rule throws; it breaks authorization for every user' \
			"$(journalctl -b -t polkitd --no-pager | grep 'Error evaluating' | tail -2)"
	fi
	ok 'the polkit rule evaluates without throwing'
else
	printf 'skip polkit checks (no polkitd in this guest)\n'
fi

# ------------------------------------------------------------------- teardown -
expect 'STOP ends the session' $'STOP '"$CLIENT_A"$'\n' 'OK'
! can_open_input hermes-s01 "$NODE_A" || fail 'STOP left client A input ACL in place'
! can_open_input hermes-s01 "$HID_A" || fail 'STOP left client A hidraw ACL in place'
can_open_input hermes-s02 "$NODE_B" || fail 'STOP revoked another session input ACL'
can_open_input hermes-s02 "$HID_B" || fail 'STOP revoked another session hidraw ACL'
ok 'STOP revokes only the stopped session input access'
[ "$(systemctl show -p ActiveState --value hermes-session-01.service)" != active ] \
	|| fail 'the session unit survived STOP'
ok 'the session unit is gone'

[ "$(systemctl show -p ActiveState --value hermes-session-01-app.service)" != active ] \
	|| fail 'the scoped unit survived the session it was bound to'
ok 'the scoped unit went with the session'

loginctl show-user hermes-s01 -p Linger --value 2>/dev/null | grep -qx no \
	|| [ -z "$(loginctl show-user hermes-s01 -p Linger --value 2>/dev/null)" ] \
	|| fail 'lingering was left enabled after STOP'
ok 'lingering was turned back off'

expect 'STOP is idempotent' $'STOP '"$CLIENT_A"$'\n' 'OK'

# --------------------------------------------------------------------- purge -
expect 'PURGE removes the account' $'PURGE '"$CLIENT_A"$'\n' 'OK'
! getent passwd hermes-s01 >/dev/null || fail 'PURGE left the account behind'
[ ! -d "$HOMES/hermes-s01" ] || fail 'PURGE left the home behind'
ok 'the account and its home are gone'

expect 'PURGE of an unmapped client fails' $'PURGE '"$CLIENT_A"$'\n' 'ERR notfound*'
expect 'the freed slot is reused' $'ENSURE '"$CLIENT_A"$'\n' 'OK hermes-s01 * *'
! can_open_input hermes-s01 "$NODE_A" || fail 'a recreated account inherited stale input access'
expect 'PURGE stops the second session and revokes its lease' $'PURGE '"$CLIENT_B"$'\n' 'OK'
if getfacl -cpn "$NODE_B" | grep -q "^user:$UID_B:"; then
	fail 'PURGE left a named ACL for a deleted account'
fi
if getfacl -cpn "$HID_B" | grep -q "^user:$UID_B:"; then fail 'PURGE left a named hidraw ACL'; fi
ok 'PURGE revokes device access before releasing the account slot'

# ------------------------------------------------------------ damaged state --
cp "$STATE" "$STATE.bak"
printf 'this-line-has-no-account\n' >>"$STATE"
expect 'a damaged map is refused, not silently repaired' $'LIST\n' 'ERR state*'
expect 'and no request acts on it'                       $'ENSURE '"$CLIENT_B"$'\n' 'ERR state*'
cp "$STATE.bak" "$STATE"
expect 'a repaired map works again' $'LIST\n' 'OK *'

printf '\nall %d session-broker checks passed\n' "$PASS"
