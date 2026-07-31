#{
# tests/meson.build
# Dynamic, declarative, script-free test definitions for Volt.
#}

sh = find_program('sh')
fs = import('fs')

samples_root = meson.project_source_root() / 'samples'
golden_root  = meson.project_source_root() / 'tests' / 'golden' / 'samples'

# -----------------------------------------------------------------------------

samples_find = run_command(
  find_prog, samples_root, '-type', 'f', '(', '-name', '*.vl', '-o', '-name', '*.vlx', ')',
  check : true
)
sample_files = samples_find.stdout().strip().split('\n')

foreach sample_path : sample_files
  if sample_path == ''
    continue
  endif

  rel_sample = sample_path.replace(samples_root + '/', '')
  golden_path = golden_root / (rel_sample + '.golden')
  lowered_golden_path = golden_root / (rel_sample + '.lowered.golden')

  if fs.exists(golden_path)
    test('golden/' + rel_sample, sh,
      args : [
        '-c',
        'OUT="$("$1" parse --no-color -i "$2" 2>&1)"; CODE=$?; printf "%s\n--- exit %d ---\n" "$OUT" "$CODE" | diff -u "$3" -',
        'sh', volt_exe.full_path(), sample_path, golden_path
      ],
      depends : volt_exe,
      suite : 'golden'
    )
  endif

  if fs.exists(lowered_golden_path)
    test('golden-lowered/' + rel_sample, sh,
      args : [
        '-c',
        'OUT="$("$1" parse --no-color --lowered -i "$2" 2>&1)"; CODE=$?; printf "%s\n--- exit %d ---\n" "$OUT" "$CODE" | diff -u "$3" -',
        'sh', volt_exe.full_path(), sample_path, lowered_golden_path
      ],
      depends : volt_exe,
      suite : 'golden-lowered'
    )
  endif
endforeach

# -----------------------------------------------------------------------------
# 2. Sample Execution Tests (@samples/Tests)
# -----------------------------------------------------------------------------

tests_samples_root = samples_root / 'Tests'
tests_find = run_command(
  find_prog, tests_samples_root, '-type', 'f', '-name', '*.vl',
  check : true
)
exec_test_files = tests_find.stdout().strip().split('\n')

foreach test_path : exec_test_files
  if test_path == ''
    continue
  endif

  expected_path = test_path + '.expected'
  if fs.exists(expected_path)
    rel_test = test_path.replace(samples_root + '/', '')
    test('sample/' + rel_test, sh,
      args : [
        '-c',
        'TMPBIN="$(mktemp)"; "$1" build -i "$2" -o "$TMPBIN" >/dev/null 2>&1 && "$TMPBIN" >/dev/null 2>&1; CODE=$?; rm -f "$TMPBIN"; EXP="$(grep "^exit=" "$3" | cut -d= -f2)"; [ "$CODE" -eq "$EXP" ]',
        'sh', volt_exe.full_path(), test_path, expected_path
      ],
      depends : volt_exe,
      suite : 'samples'
    )
  endif
endforeach

# -----------------------------------------------------------------------------
# 3. Golden Update Target (ninja -C build golden-update)
# -----------------------------------------------------------------------------

run_target('golden-update',
  command : [
    sh, '-c', '''
      VOLT_BIN="$1"
      ROOT_DIR="$2"
      cd "$ROOT_DIR"
      find samples -type f \( -name "*.vl" -o -name "*.vlx" \) | while read -r sample; do
        golden="tests/golden/${sample}.golden"
        lowered_golden="tests/golden/${sample}.lowered.golden"
        mkdir -p "$(dirname "$golden")"
        out="$("$VOLT_BIN" parse --no-color -i "$sample" 2>&1)"
        code=$?
        if echo "$out" | grep -q "error: "; then
          echo "SKIPPING $sample due to parse error"
          continue
        fi
        printf "%s\n--- exit %d ---\n" "$out" "$code" > "$golden"

        lowered_out="$("$VOLT_BIN" parse --no-color --lowered -i "$sample" 2>&1)"
        lowered_code=$?
        printf "%s\n--- exit %d ---\n" "$lowered_out" "$lowered_code" > "$lowered_golden"
        echo "UPDATED $sample (+ lowered)"
      done
    ''', 'sh', volt_exe.full_path(), meson.project_source_root()
  ],
  depends : volt_exe
)
