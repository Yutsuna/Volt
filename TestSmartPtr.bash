#!/usr/bin/env bash

set -euo pipefail

krystal

readonly SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
readonly VOLT_BIN="$SCRIPT_DIR/bin/Volt"
readonly SMART_PTR="$SCRIPT_DIR/SmartPtr.vl"

REPL_TESTS=(
  "sp = SmartPtr[Int].new 100" # => SmartPtr(100)
  "sp.value" # => 100
  "sp.value = 500" # => 500
  "sp.ptr" # => 500
  "sp.ptr.inspect" # => "Pointer(<address>)"
  "sp.null?" # => false
  ":exit"
)

$VOLT_BIN repl -i "$SMART_PTR" <<< "$(printf "%s\n" "${REPL_TESTS[@]}")"
