#!/bin/sh
#
# purity-test.sh — only ReplTui is allowed to write anything.
#
# The whole REPL tree is built on that rule: ReplDoc, ReplSyntax, ReplEval,
# ReplQuery, ReplComplete and ReplCore return values, and a value can be tested
# with no terminal in the room. The rule is only worth having if something
# fails when it stops being true, so this is that something.
#
# ReplCommand.cpp lives in source/Volt/Volt/ rather than under REPL/, so it is
# outside this check by construction: it is the front end, and a front end
# writes.

set -u

ROOT="${1:-source/Volt/REPL}"
STATUS=0

# What "writing" is, in the terms a grep can check.
PATTERN='<iostream>|<cstdio>|std::cout|std::cerr|std::clog|printf|::write|::read|fwrite|fputs|puts\(|termios|ioctl'

FOUND="$(
    grep -rnE "$PATTERN" "$ROOT" \
        --include='*.cpp' --include='*.hpp' --include='*.inl' \
        | grep -v "^$ROOT/ReplTui/" \
        | grep -vE '^[^:]*:[0-9]+: *(//|\*|/\*)'
)"

if [ -n "$FOUND" ]; then
    echo "I/O escaped ReplTui:"
    echo "$FOUND"
    STATUS=1
fi

# The other half of the same rule, in the other direction: nothing under REPL/
# may name a concrete LLVM type. BackendJIT keeps LLVM behind its pimpl, and a
# REPL that reached past it would put an LLVM header into the CLI's build.
LLVM="$( grep -rn 'llvm::\|<llvm/' "$ROOT" --include='*.cpp' --include='*.hpp' | grep -v 'LLVM IR' )"
if [ -n "$LLVM" ]; then
    echo "LLVM escaped the JIT's pimpl:"
    echo "$LLVM"
    STATUS=1
fi

if [ "$STATUS" -eq 0 ]; then
    echo "REPL purity: only ReplTui writes."
fi
exit "$STATUS"
