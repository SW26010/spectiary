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
