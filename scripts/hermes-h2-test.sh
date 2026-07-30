#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_NAME="${0##*/}"
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly HERMES_BINARY="${HERMES_H2_BINARY:-${PROJECT_ROOT}/build-test/sunshine}"
readonly HERMES_CONFIG="${HERMES_H2_CONFIG:-${XDG_CONFIG_HOME:-${HOME}/.config}/sunshine/sunshine.conf}"

usage() {
  cat <<EOF
Usage: ${SCRIPT_NAME} LABEL OUTPUT_DIR [-- HERMES_ARGUMENTS...]

Starts the locally built Hermes with terminal frame tracing enabled and writes
one process log to OUTPUT_DIR/hermes-LABEL.log. It refuses to start while the
installed hermes.service or another Hermes/Sunshine process is active.
EOF
}

die() {
  printf '%s: %s\n' "${SCRIPT_NAME}" "$*" >&2
  exit 1
}

main() {
  if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    return
  fi
  (($# >= 2)) || {
    usage >&2
    exit 2
  }

  local label="$1"
  local output_dir="$2"
  shift 2
  if [[ "${1:-}" == "--" ]]; then
    shift
  fi

  [[ "${label}" =~ ^[[:alnum:]_.-]+$ ]] ||
    die "invalid label '${label}'"
  [[ -x "${HERMES_BINARY}" ]] ||
    die "Hermes test binary is missing: ${HERMES_BINARY}"
  [[ -f "${HERMES_CONFIG}" ]] ||
    die "Hermes configuration is missing: ${HERMES_CONFIG}"

  if systemctl --user is-active --quiet hermes.service; then
    die "hermes.service is active; stop it first with: systemctl --user stop hermes.service"
  fi
  if pgrep -x hermes >/dev/null 2>&1 ||
    pgrep -x sunshine >/dev/null 2>&1; then
    die "another Hermes/Sunshine process is active"
  fi

  mkdir -p -- "${output_dir}"
  local log_path="${output_dir}/hermes-${label}.log"
  [[ ! -e "${log_path}" ]] ||
    die "refusing to overwrite ${log_path}"

  printf 'Hermes binary: %s\n' "${HERMES_BINARY}"
  printf 'Hermes config: %s\n' "${HERMES_CONFIG}"
  printf 'Trace log: %s\n' "${log_path}"
  printf 'Stop the test host with Ctrl-C.\n'

  HERMES_FRAME_TRACE=1 \
    "${HERMES_BINARY}" "${HERMES_CONFIG}" "$@" 2>&1 |
    tee -- "${log_path}"
}

main "$@"
