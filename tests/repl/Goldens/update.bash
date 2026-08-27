#!/usr/bin/env bash
#
# update.bash — regenerate the pure-module goldens.
#
# Run through `ninja -C build repl-goldens-update` after a deliberate change to
# a highlighter, a table or the completer. Never run it to make a red test
# green without reading the diff first: these files are the record of what the
# modules produce, and rewriting them is how that record stops meaning anything.

set -euo pipefail

readonly EXE="$1"
readonly OUT_DIR="$2"
readonly STAMP="$3"

for section in syntax doc core parse complete; do
    "$EXE" "$section" > "${OUT_DIR}/${section}.golden"
    printf 'updated %s\n' "${OUT_DIR}/${section}.golden"
done

: > "$STAMP"
