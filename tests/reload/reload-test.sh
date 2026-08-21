#!/bin/sh

set -eu

readonly VOLT="$1"
readonly BEFORE="$2"
readonly AFTER="$3"
readonly MARKER="$4"

WORK="$( mktemp -d )"
LOG="$WORK/log"
cp "$BEFORE" "$WORK/subject.vl"

function cleanup ()
{
    [ -n "${WATCHED:-}" ] && kill "$WATCHED" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

"$VOLT" run --watch -i "$WORK/subject.vl" >"$LOG" 2>&1 &
WATCHED=$!

Elapsed=0
while ! grep -q "watching for changes" "$LOG" 2>/dev/null; do
    sleep 1
    Elapsed=$(( Elapsed + 1 ))
    if [ "$Elapsed" -ge 30 ]; then
        printf 'the watch loop never started\n--- log ---\n' >&2
        cat "$LOG" >&2
        exit 1
    fi
done

cp "$AFTER" "$WORK/subject.vl"

Elapsed=0
while ! grep -q -- "$MARKER" "$LOG" 2>/dev/null; do
    sleep 1
    Elapsed=$(( Elapsed + 1 ))
    if [ "$Elapsed" -ge 30 ]; then
        printf 'expected to find: %s\n--- log ---\n' "$MARKER" >&2
        cat "$LOG" >&2
        exit 1
    fi
done
