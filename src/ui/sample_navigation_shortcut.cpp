#include "ui/sample_navigation_shortcut.h"

#include <imgui.h>

namespace specforge {
namespace {

SampleNavigationShortcut ResolveShortcut(bool previous_pressed, bool next_pressed)
{
    if (previous_pressed == next_pressed) {
        return SampleNavigationShortcut::None;
    }

    return previous_pressed ? SampleNavigationShortcut::Previous : SampleNavigationShortcut::Next;
}

}  // namespace

SampleNavigationShortcut RouteSampleNavigationShortcut(bool context_focused, bool context_hovered)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool popup_open = ImGui::IsPopupOpen(
        nullptr,
        ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!context_focused && !context_hovered) {
        return SampleNavigationShortcut::None;
    }
    if (!context_focused && ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) {
        return SampleNavigationShortcut::None;
    }
    if (io.WantTextInput || popup_open) {
        return SampleNavigationShortcut::None;
    }

    const ImGuiInputFlags route_flags = context_focused
        ? ImGuiInputFlags_RouteFocused
        : ImGuiInputFlags_RouteGlobal;
    return ResolveShortcut(
        ImGui::Shortcut(ImGuiKey_LeftArrow, route_flags),
        ImGui::Shortcut(ImGuiKey_RightArrow, route_flags));
}

}  // namespace specforge
