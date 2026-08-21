#!/usr/bin/env bash

set -euo pipefail
shopt -s inherit_errexit


readonly ROOT="${1:?Missing ROOT argument}"
readonly VOLT_BIN="${2:?Missing VOLT_BIN argument}"
readonly SAMPLE="${3:?Missing SAMPLE argument}"
readonly GOLDEN="${4:?Missing GOLDEN argument}"
readonly LOWERED="${5:?Missing LOWERED argument}"
readonly RESOLVED="${6:?Missing RESOLVED argument}"
readonly STAMP="$(realpath -m "${7:?Missing STAMP argument}")"

mkdir -p -- "${GOLDEN%/*}"
cd -- "$ROOT"


function update_golden_samples()
{
  local -r volt_flag="${1:-}"
  local -r target_file="$2"
  local out exit_code=0

  if [[ -n "$volt_flag" ]]; then
    out="$( "$VOLT_BIN" parse --no-color "${volt_flag}" -i "$SAMPLE" 2>&1 )" || exit_code=$?
  else
    out="$( "$VOLT_BIN" parse --no-color -i "$SAMPLE" 2>&1 )" || exit_code=$?
  fi

  if [[ "$out" == *"error: "* ]]; then
    echo "SKIPPING $SAMPLE due to parse error" >&2
    return $exit_code
  fi

  printf '%s\n--- exit %d ---\n' "$out" "$exit_code" > "$target_file"
  return $exit_code
}


if update_golden_samples "" "$GOLDEN"; then
  if [[ -f "$LOWERED" ]]; then
    update_golden_samples "--lowered" "$LOWERED" || true
  fi
  if [[ -f "$RESOLVED" ]]; then
    update_golden_samples "--resolved" "$RESOLVED" || true
  fi
fi


touch -- "$STAMP"
