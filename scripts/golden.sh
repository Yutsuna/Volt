#!/usr/bin/env bash
# golden.sh — grammar-regression sweep over the sample corpus.
#
# Parses every samples/**/*.vl|*.vlx individually with `Volt --print` and
# compares the printed AST + diagnostics against the archived goldens in
# tests/golden/. One golden per source file catches grammar regressions
# without writing a test per node.
#
#   scripts/golden.sh            compare against goldens (CI mode)
#   scripts/golden.sh --update   (re)generate the goldens
#
# VOLT_BIN overrides the compiler binary (default: build/bin/Volt).

set -u

ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
VOLT_BIN="${VOLT_BIN:-${ROOT}/build/bin/Volt}"
GOLDEN_DIR="${ROOT}/tests/golden"
UPDATE=0
[[ "${1:-}" == "--update" ]] && UPDATE=1

if [[ ! -x "${VOLT_BIN}" ]]; then
    echo "golden.sh: compiler not found at ${VOLT_BIN} (build first, or set VOLT_BIN)" >&2
    exit 2
fi

cd "${ROOT}"

FAILED=0
TOTAL=0

while IFS= read -r SRC; do
    TOTAL=$(( TOTAL + 1 ))
    GOLDEN="${GOLDEN_DIR}/${SRC}.golden"

    # Capture the full observable surface: printed AST, diagnostics, exit code.
    ACTUAL="$(
        "${VOLT_BIN}" --print "${SRC}" 2>&1
        echo "--- exit $? ---"
    )"

    if [[ "${UPDATE}" == 1 ]]; then
        mkdir -p "$( dirname "${GOLDEN}" )"
        printf '%s\n' "${ACTUAL}" > "${GOLDEN}"
        echo "UPDATED ${SRC}"
        continue
    fi

    if [[ ! -f "${GOLDEN}" ]]; then
        echo "MISSING ${SRC} (run scripts/golden.sh --update)"
        FAILED=$(( FAILED + 1 ))
        continue
    fi

    if ! DIFF="$( printf '%s\n' "${ACTUAL}" | diff -u "${GOLDEN}" - )"; then
        echo "FAIL    ${SRC}"
        echo "${DIFF}" | head -40
        FAILED=$(( FAILED + 1 ))
    fi
done < <( find samples -type f \( -name '*.vl' -o -name '*.vlx' \) | sort )

if [[ "${UPDATE}" == 1 ]]; then
    echo "golden.sh: ${TOTAL} golden(s) written under tests/golden/"
    exit 0
fi

if [[ "${FAILED}" -gt 0 ]]; then
    echo "golden.sh: ${FAILED}/${TOTAL} file(s) diverge from tests/golden/"
    exit 1
fi
echo "golden.sh: ${TOTAL} file(s) match tests/golden/"
