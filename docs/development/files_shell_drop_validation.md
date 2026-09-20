# Files and Annotations shell-drop validation (#116 and follow-up)

The Win32 adapter registers one shared `IDropTarget` on the viewports currently
hosting Files or eligible Annotations panels, deduplicating shared HWNDs.
It initializes OLE on the UI thread, revokes registration when
the viewport changes or receives `WM_NCDESTROY`, and releases OLE before UI
shutdown. Callbacks copy `CF_HDROP` paths into one bounded pending batch. The
ordinary application message loop dispatches the captured release destination
to `ShellUi::OpenSource` or `ShellUi::OpenAnnotation`. Annotation import uses the
same preparation, session command and inline diagnostic completion as the file
picker. No source probing, enumeration or loading happens in COM callbacks.

Annotations accepts files only. `SHCreateShellItemArrayFromDataObject` and
`IShellItemArray::GetAttributes(SIATTRIBFLAGS_OR, SFGAO_FOLDER)` obtain the shell's
folder classification for hover feedback; unknown attributes, directories and
file/folder mixtures are declined by the annotation target. The UI rechecks for
directories before any annotation import, outside OLE callbacks. Files retains
its file/folder behavior. Native window hit testing identifies the viewport
before panel hit testing, so overlapping detached panels cannot steal a drop.

Hit testing uses the shell's screen position, the visible Files content bounds,
the hosting HWND and ImGui's window/popup hit testing. The temporary mouse
viewport used by the ImGui query is restored immediately. This is isolated in
the existing panel implementation that already depends on `imgui_internal.h`;
the ImGui 1.92.8 widget tests cover it. No ImGui drag/drop payload or backend
implementation is modified.

The adapter advertises copy acceptance only; source files are never moved or
deleted. Up to 256 paths, each at most 32767 UTF-16 code units, are accepted per
batch. Oversized/malformed transfers are rejected atomically. Ordinary source
errors remain owned by the existing source activation transaction.

## Automated coverage

- `spectiary_win32_file_drop_target_tests`: actual HWND/OLE registration,
  `IDataObject` / `CF_HDROP` transfer, file/folder/unsupported paths, Unicode and
  spaces, ordering, pending-batch and size bounds, transfer failure, leave,
  release-time hit testing, destination capture, shared/separate HWNDs, copy-only
  acceptance and window destruction. Real Shell data objects cover annotation
  file acceptance and rejection of folder-only and mixed selections.
- `spectiary_source_files_widget_tests`: Files content/title/exterior, stale
  hidden/collapsed target, detached viewport with stale mouse state, overlap,
  modal blocking and active outline, plus existing Files interactions.
- `spectiary_source_annotation_widget_tests`: shared viewport destination
  separation, detached Annotations, active feedback and absent/ineligible source,
  collapsed/hidden targets, plus existing import/diagnostic interactions.
- `spectiary_shell_source_load_activation_tests`: production file/folder loading
  with mixed failures, path-specific diagnostics, independent source ordering,
  duplicate reuse and session usability with external folder settings enabled;
  real CSV annotation imports, failed-file diagnostics, duplicate handling,
  atomic folder-mixture rejection and preservation of the current source/spectrum.
- `spectiary_ui_text_tests`: English and Simplified Chinese drop instructions.

Local validation on 2026-09-20, rerun after the Annotations follow-up: the full Ninja/MSVC Debug build through
`scripts/build-ninja-msvc-debug.ps1` succeeded; `ctest --preset fast` passed
92/92 and `ctest --preset extended` passed 9/9. Final review covered COM and
HWND lifetime, UI hit testing, current-instance routing, source publication and
failure isolation, with no unresolved P0/P1 findings. Workflow triggers were
unchanged. Real Explorer mouse-gesture checks below were not performed.

The automated COM tests drive the real receiver, but do not simulate an Explorer
mouse gesture. For a manual release check, drag a supported file and folder from
Explorer into both docked and detached Files panels; repeat with multiple paths,
one unsupported member, Unicode/spaces, an overlapping panel and a modal dialog.
Check the copy cursor/outline, normal diagnostics, unchanged instance count and
usable session. Repeat with both external instance policies and parent-folder
settings. Drag over other panels and cancel with Escape: neither should open a
source. Also exercise existing internal spectral-line drag/drop.
For Annotations, repeat file drops with an active source, confirm folders and
file/folder mixtures show a refusal cursor, and check that valid imports do not
add a source or change the current spectrum.

Platform references: [RegisterDragDrop](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-registerdragdrop),
[IDropTarget](https://learn.microsoft.com/en-us/windows/win32/api/oleidl/nn-oleidl-idroptarget),
[DragQueryFileW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-dragqueryfilew),
[Shell item arrays from data objects](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-shcreateshellitemarrayfromdataobject).
