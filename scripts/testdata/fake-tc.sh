#!/usr/bin/env bash

set -euo pipefail

: "${HERMES_NETEM_TEST_LOG:?}"
: "${HERMES_NETEM_TEST_STATE:?}"

printf '%q' "$1" >>"${HERMES_NETEM_TEST_LOG}"
printf ' %q' "${@:2}" >>"${HERMES_NETEM_TEST_LOG}"
printf '\n' >>"${HERMES_NETEM_TEST_LOG}"

if [[ "$*" == "qdisc show dev "* ]]; then
  if [[ -f "${HERMES_NETEM_TEST_STATE}" ]] &&
    [[ "$(<"${HERMES_NETEM_TEST_STATE}")" == "netem" ]]; then
    printf 'qdisc netem 8001: root refcnt 2 limit 1000 delay 1ms\n'
  else
    printf 'qdisc noqueue 0: root refcnt 2\n'
  fi
  exit 0
fi

if [[ "$*" == "qdisc replace dev "*" root netem "* ]]; then
  [[ "${HERMES_NETEM_TEST_FAIL_REPLACE:-0}" != "1" ]] || exit 42
  printf 'netem\n' >"${HERMES_NETEM_TEST_STATE}"
  exit 0
fi

if [[ "$*" == "qdisc del dev "*" root" ]]; then
  printf 'noqueue\n' >"${HERMES_NETEM_TEST_STATE}"
  exit 0
fi

printf 'unexpected fake tc arguments: %s\n' "$*" >&2
exit 64
