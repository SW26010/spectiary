#pragma once

#include "domain/sample_labeling.h"

#include <string>

namespace spectiary {

enum class SampleWorkflowShortcutKind {
    None,
    PreviousSample,
    NextSample,
    UndoLabelWrite,
    AssignLabel,
};

struct SampleWorkflowShortcut {
    SampleWorkflowShortcutKind kind = SampleWorkflowShortcutKind::None;
    int label_code = kUnlabeledSampleLabelCode;
};

struct SampleWorkflowShortcutContext {
    bool focused = false;
    bool hovered = false;
    bool allow_hover_fallback = false;
    bool navigation_enabled = false;
    bool labeling_enabled = false;
    bool blocked = false;
};

enum class SampleLabelShortcutCaptureKind {
    None,
    Captured,
    Cleared,
    Cancelled,
    Unsupported,
};

struct SampleLabelShortcutCapture {
    SampleLabelShortcutCaptureKind kind = SampleLabelShortcutCaptureKind::None;
    char shortcut = '\0';
};

enum class SampleLabelShortcutSelectionKind {
    Accepted,
    ConflictRequiresRepeat,
};

struct SampleLabelShortcutSelection {
    SampleLabelShortcutSelectionKind kind = SampleLabelShortcutSelectionKind::Accepted;
    char shortcut = '\0';
    int conflicting_label_code = kUnlabeledSampleLabelCode;
};

// Call inside an owning ImGui window every frame so focused routing can arbitrate
// workflow commands before ShellUi dispatches the single winning command.
[[nodiscard]] SampleWorkflowShortcut RouteSampleWorkflowShortcut(
    const SampleWorkflowShortcutContext& context,
    const SampleLabelSet& label_set = {});

// Read one non-repeating key event while the explicit label-shortcut editor is armed.
[[nodiscard]] SampleLabelShortcutCapture CaptureSampleLabelShortcut();

[[nodiscard]] SampleLabelShortcutSelection ResolveSampleLabelShortcutSelection(
    char captured_shortcut,
    int edited_label_code,
    char pending_conflicting_shortcut,
    const SampleLabelSet& label_set);

// Label shortcuts are stored canonically as lowercase ASCII and displayed as keyboard legends.
[[nodiscard]] std::string FormatSampleLabelShortcut(char shortcut);

}  // namespace spectiary
