#!/bin/sh
#
# generations-test.sh — the ephemeral-generation rule, as something that fails.
#
# Two of the builtins compile code without meaning to keep it:
#
#   `:type` emits the expression and abandons the emission. No generation is
#   ever opened, so the count cannot move at all.
#
#   `:bench` has to execute, so it does open one — a dylib of its own — and
#   drops it the moment the last iteration returns. The count comes back to
#   exactly where it started.
#
# Either rule is easy to state and easy to break silently: a leaked generation
# costs tens of kilobytes and no symptom. `:reset` prints the live count on its
# way out, which is what makes both observable from a script.

set -u

VOLT="$1"
ROUNDS=100

# The number `:reset` reports as live just before it rebuilds the session.
live_after ()
{
    printf '%s' "$1" | "$VOLT" repl 2>&1 |
        sed -n 's/^session restarted  generations \([0-9][0-9]*\) -> .*/\1/p' |
        head -1
}

repeat ()
{
    I=0
    while [ "$I" -lt "$2" ]; do
        printf '%s\n' "$1"
        I=$(( I + 1 ))
    done
}

BASE_SCRIPT="$( printf 'x = 1\n:reset\n' )"
BASE="$( live_after "$BASE_SCRIPT" )"

if [ -z "$BASE" ]; then
    echo "could not read a generation count from :reset"
    exit 1
fi

TYPE_SCRIPT="$( printf 'x = 1\n'; repeat ':type x + 1' "$ROUNDS"; printf ':reset\n' )"
TYPES="$( live_after "$TYPE_SCRIPT" )"

BENCH_SCRIPT="$( printf 'x = 1\n'; repeat ':bench 1 x + 1' "$ROUNDS"; printf ':reset\n' )"
BENCHES="$( live_after "$BENCH_SCRIPT" )"

STATUS=0

if [ "$TYPES" != "$BASE" ]; then
    echo "$ROUNDS x :type moved the generation count: $BASE -> $TYPES"
    STATUS=1
fi

if [ "$BENCHES" != "$BASE" ]; then
    echo "$ROUNDS x :bench leaked generations: $BASE -> $BENCHES"
    STATUS=1
fi

if [ "$STATUS" -eq 0 ]; then
    echo "generations: $BASE before, $TYPES after $ROUNDS :type, $BENCHES after $ROUNDS :bench"
fi
exit "$STATUS"
