#pragma once
#include "overlays/spectral_line_session_state.h"
#include <set>

namespace spectiary {
struct BuiltInSpectralLineTaskIntent {
    bool view_selection = false;
    bool color_selection = false;
    bool whole_color_collection = false;
    std::unordered_set<std::string> group_ordering_view_ids;
    std::set<std::pair<std::string, std::string>> marker_colors;
};
[[nodiscard]] bool ReconcileBuiltInSpectralLineTask(const SpectralLineList& packaged,
    const BuiltInSpectralLineState& base, const BuiltInSpectralLineState& local,
    const BuiltInSpectralLineState& latest, const BuiltInSpectralLineTaskIntent& intent,
    BuiltInSpectralLineState& result, std::string& error);
} // namespace spectiary
