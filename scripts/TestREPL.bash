#!/usr/bin/env bash
set -euo pipefail

readonly VOLT="./build/source/Volt/Volt/volt"
readonly SAMPLES_SKIP="Exceptions/UncaughtRaise.vl"
readonly JOBS="$(nproc 2>/dev/null || echo 4)"
export VOLT

if [[ -t 1 ]]; then
  readonly C_GRN=$'\e[32m'
  readonly C_RED=$'\e[31m'
  readonly C_RST=$'\e[0m'
else
  readonly C_GRN=''
  readonly C_RED=''
  readonly C_RST=''
fi
export C_GRN C_RED C_RST

function run_test()
{
  local file="$1"
  local expected_file="${file}.expected"

  if [[ ! -f "$expected_file" ]]; then
    printf '%sSKIP%s %s (no .expected)\n' "$C_RED" "$C_RST" "$file"
    return 0
  fi

  local expected
  expected=$(grep -oE 'exit=[0-9]+' "$expected_file" | head -1 | cut -d= -f2)
  if [[ -z "$expected" ]]; then
    printf '%sSKIP%s %s (bad .expected)\n' "$C_RED" "$C_RST" "$file"
    return 0
  fi

  "$VOLT" repl < "$file" >/dev/null 2>&1
  local actual=$?

  if [[ "$actual" -eq "$expected" ]]; then
    printf '%sPASS%s %s (exit=%s)\n' "$C_GRN" "$C_RST" "$file" "$actual"
  else
    printf '%sFAIL%s %s (expected exit=%s, got %s)\n' \
      "$C_RED" "$C_RST" "$file" "$expected" "$actual"
    return 1
  fi
}
export -f run_test

find samples/Tests/ -type f -name '*.vl' ! -name '*.expected' |
    grep -vE "$SAMPLES_SKIP" |
    tr '\n' '\0' |
    xargs -0 -P "$JOBS" -n 1 bash -c 'run_test "$1"' _
