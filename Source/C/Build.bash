#!/usr/bin/env bash

#   -fno-gcse / -fno-crossjumping : keep GCC from merging the per-opcode
#   indirect jumps back into one shared branch (the whole point of threading).

set -euo pipefail

readonly C_DIR="$(cd "$(dirname "$0")" && pwd)"
readonly C_INC_DIR="$C_DIR/Include"
readonly C_FILES="$(find "$C_DIR" -type f -name '*.c')"
readonly C_FLAGS="-O3 -fPIC -fno-gcse -fno-crossjumping"
readonly C_INC="-I$C_INC_DIR"
readonly C_WARNS="-Wall -Wextra -Wno-unused-parameter -Wconversion -Werror"

readonly LIB_VM="libvoltvm"

for FILE in $C_FILES; do
    cc $C_FLAGS $C_WARNS -c "$FILE" -o "${FILE%.*}.o" $C_INC
done

readonly O_FILES="${C_FILES//.c/.o}"

cc $C_FLAGS $C_WARNS -shared -o "$C_DIR/$LIB_VM.so" $O_FILES
