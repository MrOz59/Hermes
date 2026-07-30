#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly NETEM_SCRIPT="${SCRIPT_DIR}/hermes-netem.sh"
readonly FAKE_TC="${SCRIPT_DIR}/testdata/fake-tc.sh"

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
  bash -n "${NETEM_SCRIPT}"
  bash -n "${FAKE_TC}"

  local profiles
  profiles="$("${NETEM_SCRIPT}" list)"
  for profile in lan wifi wan burst-loss bufferbloat reordering; do
    assert_contains "${profiles}" "${profile}"
  done

  local burst
  burst="$("${NETEM_SCRIPT}" dry-run burst-loss lo)"
  assert_contains "${burst}" "tc qdisc replace dev lo root netem"
  assert_contains "${burst}" "loss gemodel"
  assert_contains "${burst}" "seed 1004"

  local bloat
  bloat="$("${NETEM_SCRIPT}" describe bufferbloat)"
  assert_contains "${bloat}" "rate 20mbit"
  assert_contains "${bloat}" "limit 4000"

  if "${NETEM_SCRIPT}" describe unknown >/dev/null 2>&1; then
    fail "unknown profile unexpectedly succeeded"
  fi
  if "${NETEM_SCRIPT}" dry-run lan definitely-not-an-interface >/dev/null 2>&1; then
    fail "unknown interface unexpectedly succeeded"
  fi

  local interface=""
  local interface_path
  for interface_path in /sys/class/net/*; do
    if [[ "${interface_path##*/}" != "lo" ]]; then
      interface="${interface_path##*/}"
      break
    fi
  done
  [[ -n "${interface}" ]] || fail "no non-loopback interface available for fake-tc tests"

  local test_root
  test_root="$(mktemp -d /tmp/hermes-netem-test.XXXXXX)"
  trap "rm -rf -- ${test_root@Q}" EXIT
  local state_root="${test_root}/state"
  local fake_log="${test_root}/tc.log"
  local fake_state="${test_root}/qdisc"
  local marker="${state_root}/${interface}.state"
  local -a test_environment=(
    "HERMES_NETEM_STATE_DIR=${state_root}"
    "HERMES_NETEM_TEST_TC=${FAKE_TC}"
    "HERMES_NETEM_TEST_LOG=${fake_log}"
    "HERMES_NETEM_TEST_STATE=${fake_state}"
  )

  env "${test_environment[@]}" \
    "${NETEM_SCRIPT}" apply lan "${interface}" --force >/dev/null
  [[ -f "${marker}" ]] || fail "apply did not create a state marker"
  assert_contains "$(<"${marker}")" "status=armed"
  assert_contains "$(<"${marker}")" "profile=lan"
  if env "${test_environment[@]}" \
    "${NETEM_SCRIPT}" apply wifi "${interface}" --force >/dev/null 2>&1; then
    fail "second apply unexpectedly replaced an active harness profile"
  fi
  env "${test_environment[@]}" \
    "${NETEM_SCRIPT}" clear "${interface}" >/dev/null
  [[ ! -e "${marker}" ]] || fail "clear did not remove the state marker"
  [[ "$(<"${fake_state}")" == "noqueue" ]] ||
    fail "clear did not remove the fake root qdisc"

  if env "${test_environment[@]}" \
    HERMES_NETEM_TEST_FAIL_REPLACE=1 \
    "${NETEM_SCRIPT}" apply lan "${interface}" --force >/dev/null 2>&1; then
    fail "failed tc replacement unexpectedly succeeded"
  fi
  [[ ! -e "${marker}" ]] ||
    fail "failed tc replacement left a state marker behind"

  env "${test_environment[@]}" \
    "${NETEM_SCRIPT}" apply lan "${interface}" --force >/dev/null
  printf 'noqueue\n' >"${fake_state}"
  if env "${test_environment[@]}" \
    "${NETEM_SCRIPT}" clear "${interface}" >/dev/null 2>&1; then
    fail "clear removed a qdisc after the marked root changed"
  fi
  [[ -f "${marker}" ]] ||
    fail "qdisc mismatch unexpectedly removed the state marker"
  env "${test_environment[@]}" \
    "${NETEM_SCRIPT}" forget "${interface}" --force >/dev/null
  [[ ! -e "${marker}" ]] || fail "forget did not remove the stale marker"

  printf 'PASS: hermes-netem safe-path tests\n'
}

main "$@"
