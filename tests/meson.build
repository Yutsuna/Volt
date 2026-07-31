#{
# tests/meson.build
# Clean, modular, parallel test definitions for Volt.
#}

fs = import('fs')
sh = find_program('sh')
bash = find_program('bash')
update_script = files('golden-update.bash')

samples_root = meson.project_source_root() / 'samples'
golden_root  = meson.project_source_root() / 'tests' / 'golden' / 'samples'

# -----------------------------------------------------------------------------

samples_find = run_command(
  find_prog, samples_root, '-type', 'f', '(', '-name', '*.vl', '-o', '-name', '*.vlx', ')',
  check : true
)
sample_files = samples_find.stdout().strip().split('\n')

golden_update_targets = []

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

  golden_update_targets += custom_target('up_' + rel_sample.underscorify(),
    output : rel_sample.underscorify() + '.stamp',
    command : [bash, update_script, volt_exe.full_path(), sample_path, golden_path, lowered_golden_path, '@OUTPUT@'],
    depends : volt_exe
  )
endforeach

if golden_update_targets.length() > 0
  alias_target('golden-update', golden_update_targets)
endif

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
