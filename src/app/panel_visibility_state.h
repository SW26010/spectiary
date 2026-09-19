#pragma once

namespace spectiary {

// Semantic application state shared by settings, automation observations, and
// the persistence codec. The codec itself remains owned by the sessions
// module.
struct PanelVisibilityState {
    bool files = true;
    bool navigation = true;
    bool annotations = true;
    bool labeling = true;
    bool filters = true;
    bool sorting = true;
    bool smoothing = true;
    bool information = true;
    bool spectral_lines = true;

    [[nodiscard]] bool operator==(const PanelVisibilityState&) const = default;
};

}  // namespace spectiary
