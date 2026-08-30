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

[ "$(id -u)" -eq 0 ] || { printf 'must run as root; this creates accounts\n' >&2; exit 2; }
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
fail() { printf 'FAIL %s\n  %s\n' "$1" "${2:-}" >&2; exit 1; }

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
expect 'a second client gets slot 2' $'ENSURE '"$CLIENT_B"$'\n' "OK hermes-s02 * *"
expect 'LIST reports both'      $'LIST\n' "OK $CLIENT_A=hermes-s01 $CLIENT_B=hermes-s02"

# ---------------------------------------------------------------- sessions ---
expect 'nothing is running yet' $'STATUS '"$CLIENT_A"$'\n' 'OK inactive hermes-session-01.service'

START=$'START '"$CLIENT_A"$'\nARG /usr/bin/sleep\nARG 600\nENV HERMES_TEST=yes\nEND\n'
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

expect 'STATUS reports it active' $'STATUS '"$CLIENT_A"$'\n' 'OK active hermes-session-01.service'
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

# ------------------------------------------------------------ damaged state --
cp "$STATE" "$STATE.bak"
printf 'this-line-has-no-account\n' >>"$STATE"
expect 'a damaged map is refused, not silently repaired' $'LIST\n' 'ERR state*'
expect 'and no request acts on it'                       $'ENSURE '"$CLIENT_B"$'\n' 'ERR state*'
cp "$STATE.bak" "$STATE"
expect 'a repaired map works again' $'LIST\n' 'OK *'

printf '\nall %d session-broker checks passed\n' "$PASS"
