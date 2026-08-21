#{
# tests/meson.build
# Clean, modular, parallel test definitions for Volt.
#}

fs = import('fs')
sh = find_program('sh')
bash = find_program('bash')
update_script = files('golden-update.bash')

samples_root = meson.project_source_root() / 'samples'
golden_root = meson.project_source_root() / 'tests' / 'golden' / 'samples'

# -----------------------------------------------------------------------------

samples_find = run_command(
    find_prog,
    samples_root,
    '-type',
    'f',
    '(',
    '-name',
    '*.vl',
    '-o',
    '-name',
    '*.vlx',
    ')',
    check: true,
)
sample_files = samples_find.stdout().strip().split('\n')

golden_update_targets = []

foreach sample_path : sample_files
    if sample_path == ''
        continue
    endif

    rel_sample = sample_path.replace(samples_root + '/', '')
    root_rel_sample = 'samples' / rel_sample
    golden_path = golden_root / (rel_sample + '.golden')
    lowered_golden_path = golden_root / (rel_sample + '.lowered.golden')
    resolved_golden_path = golden_root / (rel_sample + '.resolved.golden')

    if fs.exists(golden_path)
        test(
            'golden/' + rel_sample,
            sh,
            args: [
                '-c',
                'OUT="$("$1" parse --no-color -i "$2" 2>&1)"; CODE=$?; printf "%s\n--- exit %d ---\n" "$OUT" "$CODE" | diff -u "$3" -',
                'sh',
                volt_exe.full_path(),
                root_rel_sample,
                golden_path,
            ],
            depends: volt_exe,
            workdir: meson.project_source_root(),
            suite: 'golden',
        )
    endif

    if fs.exists(lowered_golden_path)
        test(
            'golden-lowered/' + rel_sample,
            sh,
            args: [
                '-c',
                'OUT="$("$1" parse --no-color --lowered -i "$2" 2>&1)"; CODE=$?; printf "%s\n--- exit %d ---\n" "$OUT" "$CODE" | diff -u "$3" -',
                'sh',
                volt_exe.full_path(),
                root_rel_sample,
                lowered_golden_path,
            ],
            depends: volt_exe,
            workdir: meson.project_source_root(),
            suite: 'golden-lowered',
        )
    endif

    if fs.exists(resolved_golden_path)
        test(
            'golden-resolved/' + rel_sample,
            sh,
            args: [
                '-c',
                'OUT="$("$1" parse --no-color --resolved -i "$2" 2>&1)"; CODE=$?; printf "%s\n--- exit %d ---\n" "$OUT" "$CODE" | diff -u "$3" -',
                'sh',
                volt_exe.full_path(),
                root_rel_sample,
                resolved_golden_path,
            ],
            depends: volt_exe,
            workdir: meson.project_source_root(),
            suite: 'golden-resolved',
        )
    endif

    golden_update_targets += custom_target(
        'up_' + rel_sample.underscorify(),
        output: rel_sample.underscorify() + '.stamp',
        build_always_stale: true,
        command: [
            bash,
            update_script,
            meson.project_source_root(),
            volt_exe.full_path(),
            root_rel_sample,
            golden_path,
            lowered_golden_path,
            resolved_golden_path,
            '@OUTPUT@',
        ],
        depends: volt_exe,
    )
endforeach

if golden_update_targets.length() > 0
    alias_target('golden-update', golden_update_targets)
endif

# -----------------------------------------------------------------------------

meson_prog = find_program('meson')
run_target(
    'tests',
    command: [
        meson_prog,
        'test',
        '-C',
        meson.project_build_root(),
        '--print-errorlogs',
    ],
    depends: volt_exe,
)

# -----------------------------------------------------------------------------

tests_samples_root = samples_root / 'Tests'
tests_find = run_command(
    find_prog,
    tests_samples_root,
    '-type',
    'f',
    '-name',
    '*.vl',
    check: true,
)
exec_test_files = tests_find.stdout().strip().split('\n')

foreach test_path : exec_test_files
    if test_path == ''
        continue
    endif

    expected_path = test_path + '.expected'
    if fs.exists(expected_path)
        rel_test = test_path.replace(samples_root + '/', '')
        root_rel_test = 'samples' / rel_test
        test(
            'sample/' + rel_test,
            sh,
            args: [
                '-c',
                'TMPBIN="$(mktemp)"; "$1" build -i "$2" -o "$TMPBIN" >/dev/null 2>&1 && "$TMPBIN" >/dev/null 2>&1; CODE=$?; rm -f "$TMPBIN"; EXP="$(grep "^exit=" "$3" | cut -d= -f2)"; [ "$CODE" -eq "$EXP" ]',
                'sh',
                volt_exe.full_path(),
                root_rel_test,
                expected_path,
            ],
            depends: volt_exe,
            workdir: meson.project_source_root(),
            suite: 'samples',
        )
    endif
endforeach

if jit_enabled
    foreach test_path : exec_test_files
        if test_path == ''
            continue
        endif

        expected_path = test_path + '.expected'
        if fs.exists(expected_path)
            rel_test = test_path.replace(samples_root + '/', '')
            root_rel_test = 'samples' / rel_test
            test(
                'jit/' + rel_test,
                sh,
                args: [
                    '-c',
                    '"$1" run -i "$2" >/dev/null 2>&1; CODE=$?; EXP="$(grep "^exit=" "$3" | cut -d= -f2)"; [ "$CODE" -eq "$EXP" ]',
                    'sh',
                    volt_exe.full_path(),
                    root_rel_test,
                    expected_path,
                ],
                depends: volt_exe,
                workdir: meson.project_source_root(),
                suite: 'jit',
            )

            test(
                'jit-whole/' + rel_test,
                sh,
                args: [
                    '-c',
                    '"$1" run --whole-module -i "$2" >/dev/null 2>&1; CODE=$?; EXP="$(grep "^exit=" "$3" | cut -d= -f2)"; [ "$CODE" -eq "$EXP" ]',
                    'sh',
                    volt_exe.full_path(),
                    root_rel_test,
                    expected_path,
                ],
                depends: volt_exe,
                workdir: meson.project_source_root(),
                suite: 'jit-whole',
            )
            test(
                'jit-indirect/' + rel_test,
                sh,
                args: [
                    '-c',
                    '"$1" run --indirect -i "$2" >/dev/null 2>&1; CODE=$?; EXP="$(grep "^exit=" "$3" | cut -d= -f2)"; [ "$CODE" -eq "$EXP" ]',
                    'sh',
                    volt_exe.full_path(),
                    root_rel_test,
                    expected_path,
                ],
                depends: volt_exe,
                workdir: meson.project_source_root(),
                suite: 'jit-indirect',
            )
        endif
    endforeach

    reload_root = meson.current_source_dir() / 'reload'
    reload_sh = reload_root / 'reload-test.sh'

    reload_cases = [
        ['Ok/BodyChanged', 'exit 7'],
        ['Ok/CalleeChanged', 'exit 10'],
        ['Refused/SignatureChanged', 'changed shape'],
        ['Refused/LayoutChanged', 'changed size, alignment or existence'],
        ['Refused/DoesNotCompile', 'keeping the running code'],
    ]

    foreach reload_case : reload_cases
        test(
            'jit-reload/' + reload_case[0],
            sh,
            args: [
                reload_sh,
                volt_exe.full_path(),
                reload_root / (reload_case[0] + '.before.vl'),
                reload_root / (reload_case[0] + '.after.vl'),
                reload_case[1],
            ],
            depends: volt_exe,
            workdir: meson.project_source_root(),
            suite: 'jit-reload',
            timeout: 60,
        )
    endforeach
endif
