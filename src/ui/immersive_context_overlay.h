#pragma once

#include "ui/source_collection_session_types.h"
#include "ui/ui_text.h"

#include <imgui.h>

#include <optional>
#include <string>

namespace spectiary {

struct ImmersiveContextOverlayView {
    std::string sequence_position_text;
    std::string labeling_context_text;
    bool presents_previous_label = false;
};

struct ImmersiveContextOverlayRenderResult {
    bool rendered = false;
    ImVec2 min;
    ImVec2 max;
};

[[nodiscard]] std::optional<ImmersiveContextOverlayView>
BuildImmersiveContextOverlayView(
    bool immersive_mode,
    const SourceCollectionSessionView& session,
    UiLanguage language);

// Draw-list-only rendering intentionally registers no ImGui item or window, so
// plot pan, zoom, hover, and touchpad input remain owned by the plot beneath it.
[[nodiscard]] ImmersiveContextOverlayRenderResult
RenderImmersiveContextOverlay(
    const ImmersiveContextOverlayView& view);

}  // namespace spectiary
