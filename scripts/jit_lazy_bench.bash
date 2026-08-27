#!/usr/bin/env bash
#
# jit_lazy_bench.bash — what lazy JIT compilation is worth, and where it stops
# being worth it.
#
# A manual diagnostic, not a test: the numbers move with the machine, so there
# is no threshold here to go red in CI. It answers two questions that decided
# the shape of `JitOptions::bLazyCompilation` (.agents/backend/jit.md, "Lazy
# compilation"), and it is the tool to re-run when that policy is touched.
#
#   sweep      how startup scales with functions the program never calls
#   crossover  where calling more of what you define stops paying off
#   samples    eager and lazy agree on every executable sample
#   files ...  compare two modes on programs you name
#
# Usage:
#   scripts/jit_lazy_bench.bash sweep      [reps]
#   scripts/jit_lazy_bench.bash crossover  [reps]
#   scripts/jit_lazy_bench.bash samples
#   scripts/jit_lazy_bench.bash files a.vl b.vl ...
#
# Two things about the method are load-bearing, both learned the hard way:
#
#   - The modes are alternated *inside* one loop rather than measured in two
#     passes. A background build drifting the machine between passes produced a
#     confident, reproducible, and completely wrong "lazy is slower" reading.
#   - Wall clock, never `volt run -v`'s total. Under lazy, compilation moves
#     into the program's own execution, which no PhaseScope brackets:
#     `backend.jit.materialize` collapses to ~0.2 ms and the work it used to
#     account for appears nowhere at all.

set -u

VOLT=${VOLT:-./build/source/Volt/Volt/volt}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$VOLT" ]; then
    echo "no volt at '$VOLT' — build first, or set VOLT=" >&2
    exit 1
fi

# --- corpus ------------------------------------------------------------------

# N functions defined, K of them called from the top level. The body is big
# enough that codegen for it is worth measuring and small enough that running
# it is not.
Generate ()
{
    local N=$1 K=$2 OUT=$3
    : > "$OUT"
    local i
    for i in $( seq 1 "$N" ); do
        cat >> "$OUT" <<VOLT
def fn_$i( a : Int32, b : Int32 ) -> Int32
  t = 0
  k = a
  while k < b
    t += ( k * 3 ) % 7
    t -= ( k / 2 ) % 5
    k += 1
  end
  t
end
VOLT
    done
    echo "acc = 0" >> "$OUT"
    for i in $( seq 1 "$K" ); do
        echo "acc += fn_$i( 0, 4 )" >> "$OUT"
    done
    echo 'assert!( acc >= 0 )' >> "$OUT"
}

# --- measurement -------------------------------------------------------------

# Best-of-Reps wall time in ms, printed as "eager lazy". Alternated per
# iteration so that whatever the machine is doing lands on both.
Compare ()
{
    local File=$1 Reps=$2
    local Eager=99999999 Lazy=99999999 A B Ms

    local R
    for R in $( seq 1 "$Reps" ); do
        A=$( date +%s%N ); "$VOLT" run --no-lazy -i "$File" > /dev/null 2>&1; B=$( date +%s%N )
        Ms=$(( ( B - A ) / 1000000 )); [ "$Ms" -lt "$Eager" ] && Eager=$Ms

        A=$( date +%s%N ); "$VOLT" run           -i "$File" > /dev/null 2>&1; B=$( date +%s%N )
        Ms=$(( ( B - A ) / 1000000 )); [ "$Ms" -lt "$Lazy" ] && Lazy=$Ms
    done
    echo "$Eager $Lazy"
}

Row ()
{
    local Label=$1 Eager=$2 Lazy=$3
    local Delta=$(( Lazy - Eager ))
    local Pct="n/a"
    [ "$Eager" -gt 0 ] && Pct=$(( ( Delta * 100 ) / Eager ))
    printf "%-22s %8s %8s %8s %7s\n" "$Label" "${Eager}ms" "${Lazy}ms" "${Delta}ms" "${Pct}%"
}

Header ()
{
    printf "%-22s %8s %8s %8s %7s\n" "$1" "eager" "lazy" "delta" ""
    printf "%.0s-" $( seq 1 58 ); echo
}

# --- modes -------------------------------------------------------------------

# How startup scales with dead weight. Lazy should be near flat; eager should
# be a straight line through the number of functions nothing calls.
Sweep ()
{
    local Reps=$1
    Header "1 called, N defined"
    local N
    for N in 0 25 100 400; do
        Generate "$N" 0 "$WORK/sweep.vl"
        # One live call, so the program does something in both modes.
        echo 'assert!( true )' >> "$WORK/sweep.vl"
        read -r E L <<< "$( Compare "$WORK/sweep.vl" "$Reps" )"
        Row "N=$N" "$E" "$L"
    done
}

# The bet, stated as a curve: 100 functions defined, K of them called. Lazy
# wins big at the left, loses at the right, and where it crosses is the whole
# argument about the default.
Crossover ()
{
    local Reps=$1
    Header "100 defined, K called"
    local K
    for K in 0 25 50 100; do
        Generate 100 "$K" "$WORK/cross.vl"
        read -r E L <<< "$( Compare "$WORK/cross.vl" "$Reps" )"
        Row "K=$K (${K}%)" "$E" "$L"
    done
}

# Correctness, not speed: the two modes must produce the same output and the
# same exit code on everything the repo can run.
Samples ()
{
    local Pass=0 Diff=0 F A B Ra Rb
    for F in $( find samples/Tests samples/Bench -name "*.vl" | sort ); do
        A=$( timeout 60 "$VOLT" run --no-lazy -i "$F" 2>&1 ); Ra=$?
        B=$( timeout 60 "$VOLT" run           -i "$F" 2>&1 ); Rb=$?
        if [ "$A" = "$B" ] && [ "$Ra" = "$Rb" ]; then
            Pass=$(( Pass + 1 ))
        else
            Diff=$(( Diff + 1 ))
            echo "DIVERGENT: $F (rc $Ra vs $Rb)"
        fi
    done
    echo "identical: $Pass   divergent: $Diff"
    # Benchmarks.vl prints tick counters, which differ between any two runs of
    # anything. It is the one expected divergence.
    [ "$Diff" -le 1 ]
}

Files ()
{
    local Reps=9 F E L
    Header "file"
    for F in "$@"; do
        read -r E L <<< "$( Compare "$F" "$Reps" )"
        Row "$( basename "$F" )" "$E" "$L"
    done
}

# --- entry -------------------------------------------------------------------

Mode=${1:-crossover}
shift || true

case "$Mode" in
    sweep)     Sweep "${1:-7}" ;;
    crossover) Crossover "${1:-7}" ;;
    samples)   Samples ;;
    files)     Files "$@" ;;
    *)
        sed -n '3,30p' "$0" | sed 's|^# \?||'
        exit 1
        ;;
esac
