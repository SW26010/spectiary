#pragma once

#include "domain/sample_filter.h"
#include "domain/spectrum_snapshot.h"
#include "ui/source_collection_session.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

class SampleWorkflowPanelUi {
public:
    [[nodiscard]] static const char* LabelingWindowName();
    [[nodiscard]] static const char* FiltersWindowName();

    void ResetForSampleWorkflow();

    [[nodiscard]] SourceCollectionSessionAction RenderLabeling(
        const SourceCollectionSessionView& session_view,
        const SourceCollectionSessionIntentSubmitter& submit,
        bool plot_shortcut_context_active,
        bool* open,
        const std::function<std::optional<std::filesystem::path>()>& choose_output_path);

    [[nodiscard]] SourceCollectionSessionAction RenderFilters(
        const SourceCollectionSessionView& session_view,
        const SourceCollectionSessionIntentSubmitter& submit,
        bool* open);

private:
    std::array<char, 96> new_label_name_buffer_ = {};
    std::array<char, 16> new_label_code_buffer_ = {};
    std::array<char, 8> new_label_shortcut_buffer_ = {};
};

}  // namespace specforge
