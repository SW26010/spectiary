#pragma once

#include "domain/sample_filter.h"
#include "domain/spectrum_snapshot.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_navigation_controller.h"

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

    [[nodiscard]] std::vector<SampleFilterSource> BuildFilterSources(
        const SampleNavigationController& navigation,
        const SampleLabelingController& labeling) const;

    void RenderLabeling(
        const SpectrumSnapshotHandle& snapshot,
        SampleNavigationController& navigation,
        SampleLabelingController& labeling,
        bool plot_shortcut_context_active,
        const std::function<SampleNavigationResult(const SampleNavigationRequest&)>& request_navigation,
        const std::function<void()>& apply_filters,
        const std::function<std::optional<std::filesystem::path>()>& choose_output_path);

    void RenderFilters(
        const SpectrumSnapshotHandle& snapshot,
        SampleNavigationController& navigation,
        SampleLabelingController& labeling,
        SampleFilterController& filters,
        const std::function<void()>& apply_filters);

private:
    std::array<char, 96> new_label_name_buffer_ = {};
    std::array<char, 16> new_label_code_buffer_ = {};
    std::array<char, 8> new_label_shortcut_buffer_ = {};
    std::optional<std::string> selected_labeling_filter_source_id_;
};

}  // namespace specforge
