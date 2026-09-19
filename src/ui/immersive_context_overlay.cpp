#include "ui/immersive_context_overlay.h"

#include "ui/sample_label_presentation.h"
#include "ui/theme.h"

#include <algorithm>

namespace spectiary {
namespace {

std::string LabelingContextText(
    UiLanguage language,
    UiTextId prefix,
    const SampleLabelSet& label_set,
    int value)
{
    std::string result(UiText(language, prefix));
    result += LocalizedCompactSampleLabelValue(
        language,
        label_set,
        value);
    return result;
}

}  // namespace

std::optional<ImmersiveContextOverlayView>
BuildImmersiveContextOverlayView(
    bool immersive_mode,
    const SourceCollectionSessionView& session,
    UiLanguage language)
{
    if (!immersive_mode) {
        return std::nullopt;
    }

    ImmersiveContextOverlayView result;
    const SourceCollectionResolvedSequencePositionView& position =
        session.navigation.resolved_sequence_position;
    if (session.current_sample_snapshot &&
        position.zero_based_position &&
        *position.zero_based_position < position.sequence_length) {
        result.sequence_position_text =
            std::to_string(*position.zero_based_position + 1) +
            " / " +
            std::to_string(position.sequence_length);
    }

    const SourceCollectionLabelingView& labeling =
        session.labeling;
    if (labeling.has_active_task && labeling.current_index) {
        const bool present_previous_label =
            labeling.auto_advance &&
            session.sample_transition &&
            session.sample_transition->reason ==
                SourceCollectionSampleTransitionReason::LabelingAutoAdvance &&
            session.sample_transition->accepted_label_value.has_value();
        if (present_previous_label) {
            result.labeling_context_text = LabelingContextText(
                language,
                UiTextId::ImmersivePreviousLabelPrefix,
                labeling.label_set,
                *session.sample_transition->accepted_label_value);
            result.presents_previous_label = true;
        } else {
            result.labeling_context_text = LabelingContextText(
                language,
                UiTextId::ImmersiveLabelPrefix,
                labeling.label_set,
                labeling.current_code);
        }
    }

    if (result.sequence_position_text.empty() &&
        result.labeling_context_text.empty()) {
        return std::nullopt;
    }
    return result;
}

ImmersiveContextOverlayRenderResult
RenderImmersiveContextOverlay(
    const ImmersiveContextOverlayView& view)
{
    ImmersiveContextOverlayRenderResult result;
    const bool has_position =
        !view.sequence_position_text.empty();
    const bool has_labeling =
        !view.labeling_context_text.empty();
    if (!has_position && !has_labeling) {
        return result;
    }

    const float font_size = ImGui::GetFontSize();
    const float horizontal_padding =
        std::max(8.0f, font_size * 0.65f);
    const float vertical_padding =
        std::max(6.0f, font_size * 0.45f);
    const float line_gap =
        has_position && has_labeling
            ? std::max(2.0f, font_size * 0.18f)
            : 0.0f;
    const float edge_padding =
        std::max(10.0f, font_size * 0.75f);
    const ImVec2 position_size = has_position
        ? ImGui::CalcTextSize(
              view.sequence_position_text.data(),
              view.sequence_position_text.data() +
                  view.sequence_position_text.size())
        : ImVec2{};
    const ImVec2 labeling_size = has_labeling
        ? ImGui::CalcTextSize(
              view.labeling_context_text.data(),
              view.labeling_context_text.data() +
                  view.labeling_context_text.size())
        : ImVec2{};
    const float content_width =
        std::max(position_size.x, labeling_size.x);
    const float content_height =
        position_size.y + labeling_size.y + line_gap;
    const ImVec2 window_pos = ImGui::GetWindowPos();
    result.min = ImVec2(
        window_pos.x + edge_padding,
        window_pos.y + edge_padding);
    result.max = ImVec2(
        result.min.x + content_width + horizontal_padding * 2.0f,
        result.min.y + content_height + vertical_padding * 2.0f);

    const SemanticPalette& palette = ActiveSemanticPalette();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float rounding =
        std::max(4.0f, font_size * 0.35f);
    draw_list->AddRectFilled(
        result.min,
        result.max,
        ImGui::GetColorU32(palette.overlay_background),
        rounding);
    draw_list->AddRect(
        result.min,
        result.max,
        ImGui::GetColorU32(palette.muted),
        rounding);

    ImVec2 text_pos(
        result.min.x + horizontal_padding,
        result.min.y + vertical_padding);
    if (has_position) {
        draw_list->AddText(
            text_pos,
            ImGui::GetColorU32(palette.overlay_text),
            view.sequence_position_text.data(),
            view.sequence_position_text.data() +
                view.sequence_position_text.size());
        text_pos.y += position_size.y + line_gap;
    }
    if (has_labeling) {
        draw_list->AddText(
            text_pos,
            ImGui::GetColorU32(palette.overlay_text),
            view.labeling_context_text.data(),
            view.labeling_context_text.data() +
                view.labeling_context_text.size());
    }
    result.rendered = true;
    return result;
}

}  // namespace spectiary
