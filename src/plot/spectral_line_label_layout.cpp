#include "plot/spectral_line_label_layout.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace spectiary {

bool UseSpectralBandEndpointLabels(
    double start_pixel_x,
    double end_pixel_x,
    float start_text_width,
    float end_text_width,
    float horizontal_gap) noexcept
{
    return std::abs(end_pixel_x - start_pixel_x) >=
           0.5 * start_text_width + 0.5 * end_text_width + horizontal_gap;
}

namespace {

bool MateriallyDifferent(double left, double right, double relative_tolerance)
{
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) > scale * relative_tolerance;
}

}  // namespace

void UpdateSpectralLineLabelLayoutContext(
    SpectralLineLabelLayoutWorkspace& workspace,
    const SpectralLineLabelLayoutContext& context)
{
    const float em = std::max(1.0f, context.font_size);
    const bool scope_changed = workspace.has_context && workspace.context_scope_id != context.scope_id;
    const bool x_scale_changed = workspace.has_context &&
                                 MateriallyDifferent(workspace.context_x_span, context.x_span, 1.0e-5);
    const bool plot_width_changed = workspace.has_context &&
                                    std::abs(workspace.context_plot_width - context.plot_width) >=
                                        std::max(1.0f, em * 0.5f);
    const bool font_changed = workspace.has_context &&
                              MateriallyDifferent(workspace.context_font_size, context.font_size, 1.0e-4);
    const bool new_epoch = context.force_new_epoch || scope_changed || x_scale_changed ||
                           plot_width_changed || font_changed;
    if (new_epoch) {
        workspace.appearance_history.clear();
        workspace.appearance_history_index.clear();
        workspace.next_appearance_order = 0;
        workspace.defer_lane_compaction = false;
        workspace.gesture_commit_pending = false;
    }

    if (!workspace.has_context || new_epoch) {
        workspace.has_context = true;
        workspace.context_scope_id = context.scope_id;
        workspace.context_x_span = context.x_span;
        workspace.context_plot_width = context.plot_width;
        workspace.context_font_size = context.font_size;
    }

    const bool gesture_started = context.defer_lane_compaction && !workspace.defer_lane_compaction;
    if (gesture_started) {
        workspace.gesture_commit_pending = false;
        for (auto& history : workspace.appearance_history) {
            history.visible_at_gesture_start =
                history.last_visible_layout_sequence == workspace.layout_sequence;
            history.has_gesture_appearance_order =
                history.visible_at_gesture_start && history.has_committed_appearance_order;
            if (history.has_gesture_appearance_order) {
                history.gesture_appearance_order = history.committed_appearance_order;
            }
            history.has_gesture_preferred_lane =
                history.visible_at_gesture_start && history.has_committed_lane;
            if (history.has_gesture_preferred_lane) {
                history.gesture_preferred_lane = history.committed_lane;
            }
        }
    }
    if (!context.defer_lane_compaction && workspace.defer_lane_compaction) {
        workspace.gesture_commit_pending = true;
    }
    workspace.defer_lane_compaction = context.defer_lane_compaction;
}

SpectralLineLabelMetrics MakeSpectralLineLabelMetrics(float font_size, float text_line_height)
{
    const float em = std::max(1.0f, font_size);
    const float line_height = std::max(1.0f, text_line_height);
    return {
        .horizontal_gap = em * 0.35f,
        .lane_gap = em * 0.20f,
        .edge_padding = em * 0.125f,
        .bottom_gap = em * 0.35f,
        .top_legend_inset = line_height * 2.0f + em * 0.50f,
    };
}

bool IsSpectralLineLabelAnchorInViewport(
    double anchor_x,
    double viewport_min_x,
    double viewport_max_x) noexcept
{
    if (!std::isfinite(anchor_x) || !std::isfinite(viewport_min_x) ||
        !std::isfinite(viewport_max_x)) {
        return false;
    }
    const auto [left, right] = std::minmax(viewport_min_x, viewport_max_x);
    return anchor_x >= left && anchor_x <= right;
}

SpectralLineVerticalLabelPlacement PlaceSpectralLineNameLabel(
    const SpectralLineVerticalLayoutContext& context,
    float text_height,
    std::size_t lane) noexcept
{
    const float y = context.plot_top + context.top_inset +
                    context.lane_height * static_cast<float>(lane);
    return {.y = y, .visible = y + text_height <= context.plot_mid_y};
}

SpectralLineVerticalLabelPlacement PlaceSpectralLineWavelengthLabel(
    const SpectralLineVerticalLayoutContext& context,
    float text_height,
    std::size_t lane) noexcept
{
    const float y = context.plot_bottom - context.bottom_inset - text_height -
                    context.lane_height * static_cast<float>(lane);
    return {.y = y, .visible = y >= context.plot_mid_y};
}

std::span<const SpectralLineLabelLayoutResult> LayoutSpectralLineLabels(
    SpectralLineLabelLayoutWorkspace& workspace,
    float plot_left,
    float plot_right,
    float horizontal_gap)
{
    const std::uint64_t current_layout_sequence = ++workspace.layout_sequence;
    const bool commit_gesture = workspace.gesture_commit_pending;
    workspace.results.assign(workspace.inputs.size(), {});

    workspace.order.resize(workspace.inputs.size());
    std::iota(workspace.order.begin(), workspace.order.end(), std::size_t{0});
    std::stable_sort(workspace.order.begin(), workspace.order.end(), [&](std::size_t left, std::size_t right) {
        return workspace.inputs[left].anchor_x < workspace.inputs[right].anchor_x;
    });

    workspace.input_appearance_history_indices.resize(workspace.inputs.size());
    for (const std::size_t index : workspace.order) {
        const std::string_view stable_id = workspace.inputs[index].stable_id;
        const auto existing = workspace.appearance_history_index.find(stable_id);
        if (existing != workspace.appearance_history_index.end()) {
            workspace.input_appearance_history_indices[index] = existing->second;
            auto& history = workspace.appearance_history[existing->second];
            const bool continuously_visible =
                history.last_visible_layout_sequence + 1 == current_layout_sequence;
            if (workspace.defer_lane_compaction) {
                if (!history.has_gesture_appearance_order) {
                    history.gesture_appearance_order = workspace.next_appearance_order++;
                    history.has_gesture_appearance_order = true;
                }
            } else if (commit_gesture) {
                if (!history.visible_at_gesture_start || !history.has_committed_appearance_order) {
                    if (history.has_gesture_appearance_order) {
                        history.committed_appearance_order = history.gesture_appearance_order;
                        workspace.next_appearance_order = std::max(
                            workspace.next_appearance_order,
                            history.committed_appearance_order + 1);
                    } else {
                        history.committed_appearance_order = workspace.next_appearance_order++;
                    }
                    history.has_committed_appearance_order = true;
                }
            } else if (!continuously_visible || !history.has_committed_appearance_order) {
                history.committed_appearance_order = workspace.next_appearance_order++;
                history.has_committed_appearance_order = true;
            }
            history.last_visible_layout_sequence = current_layout_sequence;
        } else {
            const std::size_t history_index = workspace.appearance_history.size();
            auto& history = workspace.appearance_history.emplace_back();
            history.stable_id = stable_id;
            history.last_visible_layout_sequence = current_layout_sequence;
            if (workspace.defer_lane_compaction) {
                history.gesture_appearance_order = workspace.next_appearance_order++;
                history.has_gesture_appearance_order = true;
            } else {
                history.committed_appearance_order = workspace.next_appearance_order++;
                history.has_committed_appearance_order = true;
            }
            workspace.appearance_history_index.emplace(
                history.stable_id,
                history_index);
            workspace.input_appearance_history_indices[index] = history_index;
        }
    }
    std::stable_sort(workspace.order.begin(), workspace.order.end(), [&](std::size_t left, std::size_t right) {
        const auto& left_history =
            workspace.appearance_history[workspace.input_appearance_history_indices[left]];
        const auto& right_history =
            workspace.appearance_history[workspace.input_appearance_history_indices[right]];
        const std::uint64_t left_order = workspace.defer_lane_compaction
                                             ? left_history.gesture_appearance_order
                                             : left_history.committed_appearance_order;
        const std::uint64_t right_order = workspace.defer_lane_compaction
                                              ? right_history.gesture_appearance_order
                                              : right_history.committed_appearance_order;
        return left_order < right_order;
    });

    if (commit_gesture) {
        for (auto& history : workspace.appearance_history) {
            history.has_gesture_appearance_order = false;
            history.visible_at_gesture_start = false;
            history.has_gesture_preferred_lane = false;
        }
        workspace.gesture_commit_pending = false;
    }

    if (workspace.inputs.empty() || plot_right <= plot_left) {
        return workspace.results;
    }

    const float plot_width = plot_right - plot_left;
    const float gap = std::max(0.0f, horizontal_gap);
    workspace.placed_intervals.clear();
    for (const std::size_t index : workspace.order) {
        const float width = std::clamp(workspace.inputs[index].width, 0.0f, plot_width);
        const float desired_left = workspace.inputs[index].anchor_x - width * 0.5f;
        const float left = std::clamp(desired_left, plot_left, plot_right - width);
        const float right = left + width;

        const auto lane_is_free = [&](std::size_t lane) {
            return std::none_of(
                workspace.placed_intervals.begin(),
                workspace.placed_intervals.end(),
                [&](const auto& placed) {
                    return placed.lane == lane &&
                           left < placed.right + gap && right + gap > placed.left;
                });
        };

        auto& history = workspace.appearance_history[workspace.input_appearance_history_indices[index]];
        const bool can_use_gesture_preference =
            workspace.defer_lane_compaction && history.has_gesture_preferred_lane &&
            lane_is_free(history.gesture_preferred_lane);
        std::size_t lane = can_use_gesture_preference ? history.gesture_preferred_lane : 0;
        if (!can_use_gesture_preference) {
            while (!lane_is_free(lane)) {
                ++lane;
            }
        }
        if (workspace.defer_lane_compaction && !history.has_gesture_preferred_lane) {
            history.gesture_preferred_lane = lane;
            history.has_gesture_preferred_lane = true;
        } else if (!workspace.defer_lane_compaction) {
            history.committed_lane = lane;
            history.has_committed_lane = true;
        }
        workspace.placed_intervals.push_back({left, right, lane});
        workspace.results[index] = {left, lane};
    }
    return workspace.results;
}

}  // namespace spectiary
