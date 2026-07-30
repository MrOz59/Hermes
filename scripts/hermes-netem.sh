#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_NAME="${0##*/}"
readonly STATE_ROOT="${HERMES_NETEM_STATE_DIR:-/run/hermes-netem}"
readonly TC_COMMAND="${HERMES_NETEM_TEST_TC:-tc}"

usage() {
  cat <<EOF
Usage:
  ${SCRIPT_NAME} list
  ${SCRIPT_NAME} describe PROFILE
  ${SCRIPT_NAME} show INTERFACE
  ${SCRIPT_NAME} dry-run PROFILE INTERFACE
  sudo ${SCRIPT_NAME} apply PROFILE INTERFACE --force
  sudo ${SCRIPT_NAME} clear INTERFACE
  sudo ${SCRIPT_NAME} forget INTERFACE --force

Profiles: lan wifi wan burst-loss bufferbloat reordering

Applying a profile replaces the interface's root qdisc. The mandatory --force
acknowledges that replacement. clear only operates when this script's state
marker exists for the interface and the current root qdisc is still netem.
forget removes a stale marker without changing the qdisc.
EOF
}

die() {
  printf '%s: %s\n' "${SCRIPT_NAME}" "$*" >&2
  exit 1
}

validate_state_root() {
  case "${STATE_ROOT}" in
    "" | "/")
      die "refusing unsafe state directory '${STATE_ROOT}'"
      ;;
  esac
}

validate_interface() {
  local interface="$1"
  [[ "${interface}" =~ ^[[:alnum:]_.:-]+$ ]] ||
    die "invalid interface name '${interface}'"
  [[ -e "/sys/class/net/${interface}" ]] ||
    die "interface '${interface}' does not exist"
}

require_tc() {
  command -v "${TC_COMMAND}" >/dev/null 2>&1 ||
    die "tc was not found; install iproute2"
}

require_root() {
  if ((EUID == 0)); then
    return
  fi
  if [[ -n "${HERMES_NETEM_TEST_TC:-}" ]]; then
    [[ "${HERMES_NETEM_TEST_TC}" == /* &&
      -x "${HERMES_NETEM_TEST_TC}" &&
      "${STATE_ROOT}" == /tmp/* ]] ||
      die "unprivileged test mode requires an executable absolute fake tc and a /tmp state directory"
    return
  fi
  die "this operation requires root; rerun it with sudo"
}

profile_definition() {
  local profile="$1"

  case "${profile}" in
    lan)
      PROFILE_DESCRIPTION="Clean wired LAN: ~2 ms added RTT with low correlated jitter."
      PROFILE_ARGS=(
        limit 1000
        delay 1ms 0.2ms 10%
        distribution normal
        seed 1001
      )
      ;;
    wifi)
      PROFILE_DESCRIPTION="Typical Wi-Fi: jitter, correlated random loss, and short transmit slots."
      PROFILE_ARGS=(
        limit 2000
        delay 8ms 4ms 25%
        distribution paretonormal
        loss random 0.5% 25%
        slot 1ms 4ms packets 16
        seed 1002
      )
      ;;
    wan)
      PROFILE_DESCRIPTION="Moderate WAN: ~70 ms added RTT, jitter, light loss, and a 100 Mbit/s link."
      PROFILE_ARGS=(
        limit 2000
        delay 35ms 8ms 20%
        distribution normal
        loss random 0.3% 10%
        rate 100mbit 38
        seed 1003
      )
      ;;
    burst-loss)
      PROFILE_DESCRIPTION="Gilbert-Elliott burst loss over a moderate-latency path."
      PROFILE_ARGS=(
        limit 2000
        delay 20ms 5ms 20%
        distribution normal
        loss gemodel 1% 25% 95% 0.1%
        seed 1004
      )
      ;;
    bufferbloat)
      PROFILE_DESCRIPTION="Constrained 20 Mbit/s path with a deliberately deep delayed queue."
      PROFILE_ARGS=(
        limit 4000
        delay 25ms 5ms 20%
        distribution normal
        rate 20mbit 38
        seed 1005
      )
      ;;
    reordering)
      PROFILE_DESCRIPTION="Moderate path with deterministic packet reordering pressure."
      PROFILE_ARGS=(
        limit 2000
        delay 15ms 5ms 20%
        distribution normal
        reorder 5% 25% gap 5
        seed 1006
      )
      ;;
    *)
      die "unknown profile '${profile}'"
      ;;
  esac
}

print_tc_command() {
  local interface="$1"
  shift

  printf 'tc qdisc replace dev %q root netem' "${interface}"
  printf ' %q' "$@"
  printf '\n'
}

state_file_for() {
  local interface="$1"
  printf '%s/%s.state\n' "${STATE_ROOT}" "${interface}"
}

list_profiles() {
  local profile
  for profile in lan wifi wan burst-loss bufferbloat reordering; do
    profile_definition "${profile}"
    printf '%-12s %s\n' "${profile}" "${PROFILE_DESCRIPTION}"
  done
}

describe_profile() {
  local profile="$1"
  profile_definition "${profile}"
  printf '%s\n' "${PROFILE_DESCRIPTION}"
  printf 'netem'
  printf ' %q' "${PROFILE_ARGS[@]}"
  printf '\n'
}

show_interface() {
  local interface="$1"
  validate_interface "${interface}"
  require_tc

  local state_file
  state_file="$(state_file_for "${interface}")"
  if [[ -f "${state_file}" ]]; then
    printf 'Hermes state marker:\n'
    sed 's/^/  /' "${state_file}"
  else
    printf 'Hermes state marker: none\n'
  fi
  "${TC_COMMAND}" qdisc show dev "${interface}"
}

dry_run_profile() {
  local profile="$1"
  local interface="$2"
  validate_interface "${interface}"
  profile_definition "${profile}"
  print_tc_command "${interface}" "${PROFILE_ARGS[@]}"
}

apply_profile() {
  local profile="$1"
  local interface="$2"
  local confirmation="${3:-}"

  validate_interface "${interface}"
  [[ "${interface}" != "lo" ]] ||
    die "refusing to replace the loopback qdisc"
  [[ "${confirmation}" == "--force" ]] ||
    die "apply requires --force to acknowledge root qdisc replacement"
  validate_state_root
  require_root
  require_tc
  profile_definition "${profile}"

  local state_file
  local previous_qdisc
  local temporary_state
  state_file="$(state_file_for "${interface}")"
  install -d -m 0755 "${STATE_ROOT}"
  [[ ! -e "${state_file}" ]] ||
    die "a Hermes state marker already exists for '${interface}'; clear or forget it first"
  previous_qdisc="$("${TC_COMMAND}" qdisc show dev "${interface}")"
  temporary_state="$(mktemp "${STATE_ROOT}/.${interface}.state.XXXXXX")"

  if ! {
    printf 'status=armed\n'
    printf 'interface=%s\n' "${interface}"
    printf 'profile=%s\n' "${profile}"
    printf 'seed=%s\n' "${PROFILE_ARGS[${#PROFILE_ARGS[@]} - 1]}"
    printf 'applied_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'previous_qdisc=%q\n' "${previous_qdisc}"
  } >"${temporary_state}"; then
    unlink -- "${temporary_state}"
    die "could not prepare state marker for '${interface}'"
  fi
  chmod 0644 "${temporary_state}"
  if ! ln "${temporary_state}" "${state_file}" 2>/dev/null; then
    unlink -- "${temporary_state}"
    die "a Hermes state marker already exists for '${interface}'"
  fi
  unlink -- "${temporary_state}"

  print_tc_command "${interface}" "${PROFILE_ARGS[@]}"
  if ! "${TC_COMMAND}" qdisc replace dev "${interface}" root netem "${PROFILE_ARGS[@]}"; then
    unlink -- "${state_file}"
    die "tc failed while applying '${profile}' to '${interface}'; marker rolled back"
  fi
}

root_qdisc_is_netem() {
  local interface="$1"
  local line
  while IFS= read -r line; do
    if [[ "${line}" =~ ^qdisc[[:space:]]+netem[[:space:]].*[[:space:]]root([[:space:]]|$) ]]; then
      return 0
    fi
  done < <("${TC_COMMAND}" qdisc show dev "${interface}")
  return 1
}

clear_profile() {
  local interface="$1"
  validate_interface "${interface}"
  validate_state_root
  require_root
  require_tc

  local state_file
  state_file="$(state_file_for "${interface}")"
  [[ -f "${state_file}" ]] ||
    die "no Hermes state marker for '${interface}'; refusing to remove an unrelated qdisc"
  root_qdisc_is_netem "${interface}" ||
    die "state marker exists but '${interface}' no longer has a root netem qdisc; refusing removal (use forget --force for a stale marker)"

  "${TC_COMMAND}" qdisc del dev "${interface}" root
  unlink -- "${state_file}"
  printf 'Removed Hermes netem profile from %s\n' "${interface}"
}

forget_marker() {
  local interface="$1"
  local confirmation="${2:-}"
  validate_interface "${interface}"
  [[ "${confirmation}" == "--force" ]] ||
    die "forget requires --force"
  validate_state_root
  require_root

  local state_file
  state_file="$(state_file_for "${interface}")"
  [[ -f "${state_file}" ]] ||
    die "no Hermes state marker for '${interface}'"
  unlink -- "${state_file}"
  printf 'Removed stale Hermes state marker for %s; qdisc was not changed\n' "${interface}"
}

main() {
  local action="${1:-list}"

  case "${action}" in
    list)
      (($# == 1)) || die "list takes no arguments"
      list_profiles
      ;;
    describe)
      (($# == 2)) || die "describe requires PROFILE"
      describe_profile "$2"
      ;;
    show)
      (($# == 2)) || die "show requires INTERFACE"
      show_interface "$2"
      ;;
    dry-run)
      (($# == 3)) || die "dry-run requires PROFILE and INTERFACE"
      dry_run_profile "$2" "$3"
      ;;
    apply)
      (($# == 4)) || die "apply requires PROFILE, INTERFACE, and --force"
      apply_profile "$2" "$3" "$4"
      ;;
    clear)
      (($# == 2)) || die "clear requires INTERFACE"
      clear_profile "$2"
      ;;
    forget)
      (($# == 3)) || die "forget requires INTERFACE and --force"
      forget_marker "$2" "$3"
      ;;
    help | --help | -h)
      usage
      ;;
    *)
      usage >&2
      die "unknown action '${action}'"
      ;;
  esac
}

main "$@"
