#!/bin/sh

set -u

VOLT="$1"
SCRIPT="$2"
MODE="$3"
ARG="$4"
# Whatever else the case needs on the command line — `--no-stdlib-cache` for
# the two that read the stdlib's own source.
shift 4

OUT="$( "$VOLT" repl "$@" < "$SCRIPT" 2>&1 )"

case "$MODE" in
exact)
    EXPECTED="$( cat "$ARG" )"
    if [ "$OUT" = "$EXPECTED" ]; then
        exit 0
    fi
    echo "--- expected ---"
    echo "$EXPECTED"
    echo "--- actual ---"
    echo "$OUT"
    exit 1
    ;;
contains)
    if echo "$OUT" | grep -qF -- "$ARG"; then
        exit 0
    fi
    echo "--- marker not found: $ARG"
    echo "--- actual ---"
    echo "$OUT"
    exit 1
    ;;
*)
    echo "repl-test.sh: unknown mode '$MODE'"
    exit 2
    ;;
esac
