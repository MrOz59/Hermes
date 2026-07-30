#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PCAP_SCRIPT="${SCRIPT_DIR}/hermes-pcap.sh"

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local output="$1"
  local expected="$2"
  [[ "${output}" == *"${expected}"* ]] ||
    fail "expected output to contain '${expected}'"
}

main() {
  bash -n "${PCAP_SCRIPT}"

  local interface=""
  local interface_path
  for interface_path in /sys/class/net/*; do
    local candidate="${interface_path##*/}"
    if [[ "${candidate}" != "lo" && "$(<"${interface_path}/type")" == "1" ]]; then
      interface="${candidate}"
      break
    fi
  done
  [[ -n "${interface}" ]] ||
    fail "no Ethernet-style non-loopback interface is available for dry-run validation"

  local output_path="/tmp/hermes-pcap-dry-run-${BASHPID}.pcap"
  local output
  [[ ! -e "${output_path}" ]] ||
    fail "unexpected pre-existing dry-run output '${output_path}'"
  output="$("${PCAP_SCRIPT}" dry-run "${interface}" "${output_path}" --duration 15 --base-port 47989 --client 192.0.2.10)"
  assert_contains "${output}" "-s 42"
  assert_contains "${output}" "-G 15"
  assert_contains "${output}" "portrange 47998-48000"
  assert_contains "${output}" "host 192.0.2.10"
  [[ ! -e "${output_path}" ]] ||
    fail "dry-run unexpectedly created '${output_path}'"

  if "${PCAP_SCRIPT}" dry-run "${interface}" "${output_path}" --client 'bad filter' >/dev/null 2>&1; then
    fail "invalid client address unexpectedly succeeded"
  fi
  if "${PCAP_SCRIPT}" dry-run lo "${output_path}" >/dev/null 2>&1; then
    fail "loopback capture unexpectedly succeeded"
  fi

  printf 'PASS: hermes-pcap safe-path tests\n'
}

main "$@"
