# Repository agent instructions

## Windows native build

- For Ninja/MSVC configure and build operations, always use
  `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-ninja-msvc-debug.ps1`.
  Add `-Configure`, `-Target <target>`, and `-TimeoutSec <seconds>` as needed.
- Do not run `ninja` or `cmake --build --preset ninja-msvc-debug` directly from an ordinary
  PowerShell or a restricted agent shell.
- In restricted environments such as Codex, run the wrapper with sandbox escalation so MSVC and
  vcpkg can access their caches and the wrapper can terminate a timed-out process tree.
- See [Ninja/MSVC 卡住排查](docs/engineering_setup.md#ninjamsvc-卡住排查) for the rationale and
  recovery procedure.

## Agent skills

### Issue tracker

Issues and PRDs for this repository live in GitHub Issues at `SW26010/SpecForge`; use `gh` from the repository checkout. See `docs/agents/issue-tracker.md`.

### Triage labels

Use the repository's standard `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, and `wontfix` labels. See `docs/agents/triage-labels.md`.

### Domain docs

This is a single-context repository using root `CONTEXT.md` and `docs/adr/`. See `docs/agents/domain.md`.
