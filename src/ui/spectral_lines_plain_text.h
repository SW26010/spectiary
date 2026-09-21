#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <string_view>

namespace spectiary {
// Authored names are plain UTF-8. Submit only stable hidden labels to widgets,
// then render the full text range without changing their last-item identity.
// RenderTextClipped (without Ex) still interprets ## even with an explicit end.
inline bool SpectralLineTextSelectable(const char* identity, std::string_view text,
    bool selected = false, ImGuiSelectableFlags flags = 0, ImVec2 size = {})
{
    const auto text_size = ImGui::CalcTextSize(text.data(), text.data() + text.size(), false);
    auto pos = ImGui::GetCursorScreenPos();
    pos.y += ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
    if (size.x == 0.0f) {
        // Keep full-text measurement for popup sizing and the default full-row hit area.
        size.x = text_size.x;
        flags |= ImGuiSelectableFlags_SpanAvailWidth;
    }
    if (size.y == 0.0f) size.y = text_size.y;
    const bool chosen = ImGui::Selectable(identity, selected, flags, size);
    if (ImGui::IsItemVisible()) {
        const ImRect clip(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::RenderTextClippedEx(ImGui::GetWindowDrawList(), pos, ImVec2(clip.Max.x, pos.y + size.y),
            text.data(), text.data() + text.size(), &text_size,
            ImGui::GetStyle().SelectableTextAlign, &clip);
        if (GImGui->LogEnabled) ImGui::LogRenderedText(&pos, text.data(), text.data() + text.size());
    }
    return chosen;
}

// Call after the matching BeginCombo(..., nullptr, CustomPreview)/EndCombo.
inline void SpectralLineTextComboPreview(std::string_view text)
{
    if (ImGui::BeginComboPreview()) {
        const auto item = GImGui->LastItemData;
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::EndComboPreview();
        GImGui->LastItemData = item;
    }
}

inline bool BeginSpectralLineTextTab(const char* identity, std::string_view text,
    ImGuiTabItemFlags flags)
{
    const auto text_size = ImGui::CalcTextSize(text.data(), text.data() + text.size(), false);
    const auto padding = ImGui::GetStyle().FramePadding;
    const auto id = ImGui::GetID(identity);
    ImGui::SetNextItemWidth(text_size.x + 2.0f * padding.x);
    const bool active = ImGui::BeginTabItem(identity, nullptr, flags | ImGuiTabItemFlags_NoTooltip);
    if (ImGui::GetItemID() == id && ImGui::IsItemVisible()) {
        const ImRect bounds(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImRect clip = bounds;
        const auto* bar = GImGui->CurrentTabBar;
        if ((flags & (ImGuiTabItemFlags_Leading | ImGuiTabItemFlags_Trailing)) == 0) {
            clip.Min.x = ImMax(clip.Min.x, bar->ScrollingRectMinX);
            clip.Max.x = ImMin(clip.Max.x, bar->ScrollingRectMaxX);
        }
        ImGui::RenderTextClippedEx(ImGui::GetWindowDrawList(), ImVec2(bounds.Min.x + padding.x, bounds.Min.y + padding.y),
            ImVec2(bounds.Max.x - padding.x, bounds.Max.y), text.data(), text.data() + text.size(),
            &text_size, ImVec2(0, 0), &clip);
        if (GImGui->LogEnabled) ImGui::LogRenderedText(&clip.Min, text.data(), text.data() + text.size());
        ImGui::SetItemTooltip("%.*s", static_cast<int>(text.size()), text.data());
    }
    return active;
}
} // namespace spectiary
