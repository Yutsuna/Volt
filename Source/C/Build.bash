#!/usr/bin/env bash

#   -fno-gcse / -fno-crossjumping : keep GCC from merging the per-opcode
#   indirect jumps back into one shared branch (the whole point of threading).

set -euo pipefail

readonly DIR="$(cd "$(dirname "$0")" && pwd)"

cc -O3 -fPIC -fno-gcse -fno-crossjumping \
   -Wall -Wextra \
   -c "$DIR/Dispatch.c" -o "$DIR/Dispatch.o"

echo "built $DIR/Dispatch.o"
