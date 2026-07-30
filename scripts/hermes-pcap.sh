#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_NAME="${0##*/}"
readonly SNAP_LENGTH=42

usage() {
  cat <<EOF
Usage:
  ${SCRIPT_NAME} dry-run INTERFACE OUTPUT.pcap [--duration SECONDS] [--base-port PORT] [--client IP]
  ${SCRIPT_NAME} capture INTERFACE OUTPUT.pcap [--duration SECONDS] [--base-port PORT] [--client IP]

Captures at most ${SNAP_LENGTH} bytes per matching UDP packet on an Ethernet-style
interface. This reaches the end of a minimum Ethernet/IPv4/UDP header but never
the application payload. The pcap retains timestamps and original wire lengths.
Default base port: 47989; default duration: 60 seconds.
EOF
}

die() {
  printf '%s: %s\n' "${SCRIPT_NAME}" "$*" >&2
  exit 1
}

validate_interface() {
  local interface="$1"
  local link_type

  [[ "${interface}" =~ ^[[:alnum:]_.:-]+$ ]] ||
    die "invalid interface name '${interface}'"
  [[ "${interface}" != "lo" ]] ||
    die "refusing loopback capture; select the client-facing interface"
  [[ -e "/sys/class/net/${interface}" ]] ||
    die "interface '${interface}' does not exist"
  read -r link_type <"/sys/class/net/${interface}/type"
  [[ "${link_type}" == "1" ]] ||
    die "interface '${interface}' is not Ethernet-style (ARPHRD_ETHER); payload-free truncation cannot be guaranteed"
}

validate_output() {
  local output="$1"
  [[ "${output}" == *.pcap ]] ||
    die "output must use the .pcap extension"
  [[ ! -e "${output}" ]] ||
    die "output '${output}' already exists; refusing to overwrite it"
  [[ -d "$(dirname -- "${output}")" ]] ||
    die "output directory does not exist"
}

validate_integer_range() {
  local value="$1"
  local minimum="$2"
  local maximum="$3"
  local name="$4"

  [[ "${value}" =~ ^[0-9]+$ ]] ||
    die "${name} must be an integer"
  ((value >= minimum && value <= maximum)) ||
    die "${name} must be between ${minimum} and ${maximum}"
}

validate_client_ip() {
  local client="$1"
  python3 - "${client}" <<'PY'
import ipaddress
import sys

try:
    ipaddress.ip_address(sys.argv[1])
except ValueError:
    raise SystemExit(1)
PY
}

print_command() {
  printf 'tcpdump'
  printf ' %q' "$@"
  printf '\n'
}

main() {
  local action="${1:-}"
  (($# >= 3)) || {
    usage >&2
    exit 1
  }
  shift

  local interface="$1"
  local output="$2"
  shift 2

  local duration=60
  local base_port=47989
  local client=""

  while (($# > 0)); do
    case "$1" in
      --duration)
        (($# >= 2)) || die "--duration requires a value"
        duration="$2"
        shift 2
        ;;
      --base-port)
        (($# >= 2)) || die "--base-port requires a value"
        base_port="$2"
        shift 2
        ;;
      --client)
        (($# >= 2)) || die "--client requires an IP address"
        client="$2"
        shift 2
        ;;
      *)
        die "unknown option '$1'"
        ;;
    esac
  done

  [[ "${action}" == "dry-run" || "${action}" == "capture" ]] ||
    die "action must be dry-run or capture"
  validate_interface "${interface}"
  validate_output "${output}"
  validate_integer_range "${duration}" 1 3600 "duration"
  validate_integer_range "${base_port}" 1024 65524 "base port"
  if [[ -n "${client}" ]] && ! validate_client_ip "${client}"; then
    die "client must be a literal IPv4 or IPv6 address"
  fi
  command -v tcpdump >/dev/null 2>&1 ||
    die "tcpdump was not found"

  local first_port=$((base_port + 9))
  local last_port=$((base_port + 11))
  local -a filter=(udp and portrange "${first_port}-${last_port}")
  if [[ -n "${client}" ]]; then
    filter+=(and host "${client}")
  fi

  local -a command=(
    -i "${interface}"
    -p
    -nn
    -Q inout
    -s "${SNAP_LENGTH}"
    -B 4096
    -G "${duration}"
    -W 1
    -w "${output}"
    "${filter[@]}"
  )

  print_command "${command[@]}"
  if [[ "${action}" == "capture" ]]; then
    tcpdump "${command[@]}"
  fi
}

main "$@"
