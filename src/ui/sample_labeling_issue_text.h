#pragma once

#include "ui/sample_labeling_controller.h"
#include "ui/ui_text.h"

#include <string_view>

namespace specforge {

// Keep the semantic issue, stable UI id, and session fallback text together.
// This is header-only because specforge_sessions intentionally does not link
// the desktop text catalog, while both targets must use one mapping.
struct SampleLabelingIssueTextDescriptor {
    UiTextId text_id = UiTextId::Count;
    std::string_view english;
    std::string_view simplified_chinese;
};

[[nodiscard]] constexpr SampleLabelingIssueTextDescriptor
SampleLabelingIssueTextFor(
    SampleLabelingOperationResult::Issue issue) noexcept
{
    using Issue = SampleLabelingOperationResult::Issue;
    switch (issue) {
    case Issue::EditLeaseUnavailable:
        return {
            UiTextId::LabelingEditLeaseUnavailable,
            "This labeling target is already being edited by another SpecForge instance.",
            "此标注目标正在由另一个 SpecForge 实例编辑。"};
    case Issue::EditLeaseFailed:
        return {
            UiTextId::LabelingEditLeaseFailed,
            "SpecForge could not secure this labeling target for editing.",
            "SpecForge 无法取得此标注目标的编辑租约。"};
    case Issue::EditTargetChanged:
        return {
            UiTextId::LabelingEditTargetChanged,
            "This labeling task changed on disk and could not be activated from the stale view.",
            "此标注任务已在磁盘上发生变化，无法从过期视图激活。"};
    case Issue::None:
    default:
        return {};
    }
}

[[nodiscard]] constexpr SampleLabelingIssueTextDescriptor
SampleLabelingIssueTextForValue(int issue_value) noexcept
{
    return SampleLabelingIssueTextFor(
        static_cast<SampleLabelingOperationResult::Issue>(
            issue_value));
}

}  // namespace specforge
