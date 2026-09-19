#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spectiary {

struct SpectralLineLabelLayoutInput {
    std::string_view stable_id;
    float anchor_x = 0.0f;
    float width = 0.0f;
};

struct SpectralLineLabelLayoutResult {
    float left = 0.0f;
    std::size_t lane = 0;
};

struct SpectralLineLabelLayoutContext {
    std::string_view scope_id;
    double x_span = 0.0;
    float plot_width = 0.0f;
    float font_size = 0.0f;
    bool force_new_epoch = false;
    bool defer_lane_compaction = false;
};

struct SpectralLineLabelMetrics {
    float horizontal_gap = 0.0f;
    float lane_gap = 0.0f;
    float edge_padding = 0.0f;
    float bottom_gap = 0.0f;
    float top_legend_inset = 0.0f;
};

struct SpectralLineVerticalLayoutContext {
    float plot_top = 0.0f;
    float plot_bottom = 0.0f;
    float plot_mid_y = 0.0f;
    float top_inset = 0.0f;
    float bottom_inset = 0.0f;
    float lane_height = 0.0f;
};

struct SpectralLineVerticalLabelPlacement {
    float y = 0.0f;
    bool visible = false;
};

class SpectralLineLabelLayoutWorkspace {
public:
    std::vector<SpectralLineLabelLayoutInput> inputs;

private:
    struct AppearanceHistory {
        std::string stable_id;
        std::uint64_t committed_appearance_order = 0;
        std::uint64_t gesture_appearance_order = 0;
        std::uint64_t last_visible_layout_sequence = 0;
        std::size_t committed_lane = 0;
        // Immutable within one gesture; temporary collision fallbacks must not replace it.
        std::size_t gesture_preferred_lane = 0;
        bool has_committed_appearance_order = false;
        bool has_gesture_appearance_order = false;
        bool visible_at_gesture_start = false;
        bool has_committed_lane = false;
        bool has_gesture_preferred_lane = false;
    };

    struct PlacedInterval {
        float left = 0.0f;
        float right = 0.0f;
        std::size_t lane = 0;
    };

    std::vector<SpectralLineLabelLayoutResult> results;
    std::vector<std::size_t> order;
    std::vector<AppearanceHistory> appearance_history;
    std::map<std::string, std::size_t, std::less<>> appearance_history_index;
    std::vector<std::size_t> input_appearance_history_indices;
    std::vector<PlacedInterval> placed_intervals;
    std::uint64_t next_appearance_order = 0;
    std::uint64_t layout_sequence = 0;
    bool has_context = false;
    std::string context_scope_id;
    double context_x_span = 0.0;
    float context_plot_width = 0.0f;
    float context_font_size = 0.0f;
    bool defer_lane_compaction = false;
    bool gesture_commit_pending = false;

    friend void UpdateSpectralLineLabelLayoutContext(
        SpectralLineLabelLayoutWorkspace& workspace,
        const SpectralLineLabelLayoutContext& context);
    friend std::span<const SpectralLineLabelLayoutResult> LayoutSpectralLineLabels(
        SpectralLineLabelLayoutWorkspace& workspace,
        float plot_left,
        float plot_right,
        float horizontal_gap);
};

void UpdateSpectralLineLabelLayoutContext(
    SpectralLineLabelLayoutWorkspace& workspace,
    const SpectralLineLabelLayoutContext& context);

[[nodiscard]] SpectralLineLabelMetrics MakeSpectralLineLabelMetrics(
    float font_size,
    float text_line_height);

[[nodiscard]] bool IsSpectralLineLabelAnchorInViewport(
    double anchor_x,
    double viewport_min_x,
    double viewport_max_x) noexcept;

[[nodiscard]] SpectralLineVerticalLabelPlacement PlaceSpectralLineNameLabel(
    const SpectralLineVerticalLayoutContext& context,
    float text_height,
    std::size_t lane) noexcept;

[[nodiscard]] SpectralLineVerticalLabelPlacement PlaceSpectralLineWavelengthLabel(
    const SpectralLineVerticalLayoutContext& context,
    float text_height,
    std::size_t lane) noexcept;

[[nodiscard]] std::span<const SpectralLineLabelLayoutResult> LayoutSpectralLineLabels(
    SpectralLineLabelLayoutWorkspace& workspace,
    float plot_left,
    float plot_right,
    float horizontal_gap);

}  // namespace spectiary
