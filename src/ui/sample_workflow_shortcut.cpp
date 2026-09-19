#include "ui/sample_workflow_shortcut.h"

#include <imgui.h>

#include <cctype>
#include <optional>

namespace spectiary {
namespace {

bool HoverFallbackActive(const SampleWorkflowShortcutContext& context)
{
    return !context.focused && context.hovered && context.allow_hover_fallback &&
           !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);
}

bool ShortcutContextActive(
    const SampleWorkflowShortcutContext& context,
    bool hover_fallback_active,
    bool input_blocked)
{
    if (context.blocked || (!context.focused && !hover_fallback_active)) {
        return false;
    }

    return !input_blocked;
}

ImGuiInputFlags ShortcutRouteFlags(bool hover_fallback_active)
{
    return hover_fallback_active ? ImGuiInputFlags_RouteGlobal : ImGuiInputFlags_RouteFocused;
}

std::optional<char> ShortcutCharacterForKey(ImGuiKey key)
{
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        return static_cast<char>('a' + (key - ImGuiKey_A));
    }
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        return static_cast<char>('0' + (key - ImGuiKey_0));
    }
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
        return static_cast<char>('0' + (key - ImGuiKey_Keypad0));
    }
    return std::nullopt;
}

bool IsModifierKey(ImGuiKey key)
{
    return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
           key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
           key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
           key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper;
}

bool SampleLabelShortcutPressed(char shortcut, ImGuiInputFlags route_flags)
{
    const char normalized = NormalizeSampleLabelShortcut(shortcut);
    if (normalized >= 'a' && normalized <= 'z') {
        const ImGuiKey key = static_cast<ImGuiKey>(ImGuiKey_A + (normalized - 'a'));
        return ImGui::Shortcut(key, route_flags);
    }
    if (normalized >= '0' && normalized <= '9') {
        const ImGuiKey top_row_key = static_cast<ImGuiKey>(ImGuiKey_0 + (normalized - '0'));
        const ImGuiKey keypad_key = static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (normalized - '0'));
        const bool top_row_pressed = ImGui::Shortcut(top_row_key, route_flags);
        const bool keypad_pressed = ImGui::Shortcut(keypad_key, route_flags);
        return top_row_pressed || keypad_pressed;
    }
    return false;
}

}  // namespace

SampleWorkflowShortcut RouteSampleWorkflowShortcut(
    const SampleWorkflowShortcutContext& context,
    const SampleLabelSet& label_set)
{
    const bool hover_fallback_active = HoverFallbackActive(context);
    const bool popup_open = ImGui::IsPopupOpen(
        nullptr,
        ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool input_blocked = ImGui::GetIO().WantTextInput || popup_open;
    const ImGuiInputFlags route_flags = ShortcutRouteFlags(hover_fallback_active);

    const bool undo_pressed = context.labeling_enabled &&
                              ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route_flags);

    bool previous_pressed = false;
    bool next_pressed = false;
    if (context.navigation_enabled) {
        previous_pressed = ImGui::Shortcut(ImGuiKey_LeftArrow, route_flags);
        next_pressed = ImGui::Shortcut(ImGuiKey_RightArrow, route_flags);
    }

    std::optional<int> pressed_label_code;
    if (context.labeling_enabled) {
        for (const SampleLabelDefinition& label : label_set.labels) {
            const bool label_pressed = SampleLabelShortcutPressed(label.shortcut, route_flags);
            if (label_pressed && !pressed_label_code) {
                pressed_label_code = label.code;
            }
        }
    }

    if (!ShortcutContextActive(context, hover_fallback_active, input_blocked)) {
        return {};
    }
    if (undo_pressed) {
        return {SampleWorkflowShortcutKind::UndoLabelWrite};
    }
    if (previous_pressed) {
        return {SampleWorkflowShortcutKind::PreviousSample};
    }
    if (next_pressed) {
        return {SampleWorkflowShortcutKind::NextSample};
    }
    if (pressed_label_code) {
        return {SampleWorkflowShortcutKind::AssignLabel, *pressed_label_code};
    }
    return {};
}

SampleLabelShortcutCapture CaptureSampleLabelShortcut()
{
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        return {SampleLabelShortcutCaptureKind::Cancelled};
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        return {SampleLabelShortcutCaptureKind::Cleared};
    }

    const ImGuiIO& io = ImGui::GetIO();
    for (int key_value = ImGuiKey_NamedKey_BEGIN; key_value < ImGuiKey_NamedKey_END; ++key_value) {
        const ImGuiKey key = static_cast<ImGuiKey>(key_value);
        if (IsModifierKey(key) || !ImGui::IsKeyPressed(key, false)) {
            continue;
        }
        const std::optional<char> shortcut = ShortcutCharacterForKey(key);
        if (!shortcut || io.KeyCtrl || io.KeyShift || io.KeyAlt || io.KeySuper) {
            return {SampleLabelShortcutCaptureKind::Unsupported};
        }
        return {SampleLabelShortcutCaptureKind::Captured, *shortcut};
    }
    return {};
}

SampleLabelShortcutSelection ResolveSampleLabelShortcutSelection(
    char captured_shortcut,
    int edited_label_code,
    char pending_conflicting_shortcut,
    const SampleLabelSet& label_set)
{
    const char normalized = NormalizeSampleLabelShortcut(captured_shortcut);
    for (const SampleLabelDefinition& label : label_set.labels) {
        if (label.code == edited_label_code || label.shortcut != normalized) {
            continue;
        }
        return {
            normalized == NormalizeSampleLabelShortcut(pending_conflicting_shortcut)
                ? SampleLabelShortcutSelectionKind::Accepted
                : SampleLabelShortcutSelectionKind::ConflictRequiresRepeat,
            normalized,
            label.code};
    }
    return {SampleLabelShortcutSelectionKind::Accepted, normalized};
}

std::string FormatSampleLabelShortcut(char shortcut)
{
    const char normalized = NormalizeSampleLabelShortcut(shortcut);
    if (normalized == '\0') {
        return "None";
    }
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(normalized))));
}

}  // namespace spectiary
