#pragma once

namespace specforge {

enum class SampleNavigationShortcut {
    None,
    Previous,
    Next,
};

// Call inside the owning ImGui window every frame so its route can claim the arrow keys before navigation runs.
[[nodiscard]] SampleNavigationShortcut RouteSampleNavigationShortcut(
    bool context_focused,
    bool context_hovered);

}  // namespace specforge
