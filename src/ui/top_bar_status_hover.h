#pragma once

#include <imgui.h>

namespace spectiary {

// Call while the dock host menu bar is current. The window check supplies the
// Z-order ownership that a geometric rectangle hit alone does not provide.
[[nodiscard]] inline bool IsTopBarStatusHoverTarget(
    const ImVec2& min,
    const ImVec2& max) noexcept
{
    return ImGui::IsWindowHovered() &&
           ImGui::IsMouseHoveringRect(min, max, true);
}

[[nodiscard]] inline bool IsTopBarStatusLeftClickTarget(
    const ImVec2& min,
    const ImVec2& max) noexcept
{
    return IsTopBarStatusHoverTarget(min, max) &&
           ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

}  // namespace spectiary
