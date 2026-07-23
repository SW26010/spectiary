#pragma once

#include <imgui.h>

namespace specforge {

// Call while the dock host menu bar is current. The window check supplies the
// Z-order ownership that a geometric rectangle hit alone does not provide.
[[nodiscard]] inline bool IsTopBarStatusHoverTarget(
    const ImVec2& min,
    const ImVec2& max) noexcept
{
    return ImGui::IsWindowHovered() &&
           ImGui::IsMouseHoveringRect(min, max, true);
}

}  // namespace specforge
