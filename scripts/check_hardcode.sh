#!/usr/bin/env bash
# check_hardcode.sh — the zero-hardcode guard.
#
# The C++ compiler must never mention a Volt type by name: `Int`, `String`,
# `Array` are pure Volt, resolved to Memory Layouts via the stdlib +
# annotations. This grep fails the build when such an identifier appears in
# Frontend/ or Sema/ code (comments and *SelfCheck.cpp test files excluded).

set -u

ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
cd "${ROOT}"

# Whole words only: StringLiteral / ArrayLit / IntLiteral do not match.
PATTERN='\b(String|Array|Int|Int32|Int64|UInt8|UInt32|UInt64|Float32|Float64|Bool|Char|Nil|Hash|Symbol)\b'

HITS="$(
    grep -rnE "${PATTERN}" \
        source/Volt/Frontend source/Volt/Sema \
        --include='*.hpp' --include='*.cpp' --include='*.inl' \
        | grep -v 'SelfCheck\.cpp' \
        | grep -vE ':[0-9]+: *(//|\*|/\*)' \
        | grep -vE '\b(Symbol|SymbolList|SymbolLiteral)\b.*\b(Symbol)\b'
)"

# `Symbol` is also the C++ interner handle type — allow it when used as the
# Core type, i.e. lines that reference Symbol as an identifier type.
HITS="$( printf '%s' "${HITS}" | grep -vE '\bSymbol\b' )"

if [[ -n "${HITS}" ]]; then
    echo "check_hardcode.sh: Volt type names leaked into the C++ compiler:" >&2
    printf '%s\n' "${HITS}" >&2
    exit 1
fi
echo "check_hardcode.sh: no hardcoded Volt type names in Frontend/Sema"
