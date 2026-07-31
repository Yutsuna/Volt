#!/usr/bin/env bash

set -euo pipefail
shopt -s inherit_errexit


readonly ROOT="${1:?Missing ROOT argument}"
readonly VOLT_BIN="${2:?Missing VOLT_BIN argument}"
readonly SAMPLE="${3:?Missing SAMPLE argument}"
readonly GOLDEN="${4:?Missing GOLDEN argument}"
readonly LOWERED="${5:?Missing LOWERED argument}"
readonly STAMP="${6:?Missing STAMP argument}"

mkdir -p -- "${GOLDEN%/*}"

# `-i "$SAMPLE"` must stay project-root-relative: `volt parse` echoes it
# verbatim in the `Program '...'` header, and that header is exactly what
# the golden file records. An absolute path would bake this machine's
# checkout location into a committed fixture — works here, breaks on any
# other machine or in CI. `cd` first so the relative path resolves the same
# way everywhere, matching the `workdir` the comparison tests run under
# (tests/meson.build).
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
  update_golden_samples "--lowered" "$LOWERED"
fi


touch -- "$STAMP"