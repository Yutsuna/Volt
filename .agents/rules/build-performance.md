# Build Rules & Instructions

## Build Commands
- **Debug (g3)**: `ninja -C build`
- **ASAN + Debug (long, au besoin)**: `ninja -C build-asan`

## Concurrency Constraint
- **NE JAMAIS LANCER PLUSIEURS BUILDS EN MÊME TEMPS (JAMAIS).**
- Always wait for any active build process to finish before launching another build command. Never execute multiple `ninja` or compiler build tasks concurrently.
- **NE JAMAIS UTILISER MANAGE_TASK OU SCHEDULE.** Ne plus jamais appeler `manage_task` ou `schedule`.

## Strategy
- Non-unity builds by default for instant 1s incremental local edits.
- `unity` flag enabled for clean CI/Release builds.
