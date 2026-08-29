#pragma once

#include "ui/sample_labeling_controller.h"
#include "ui/ui_text.h"

#include <string_view>

namespace specforge {

// Keep the stable UI id and localized display text in the desktop UI module.
// Session transitions carry the semantic issue enum and do not include this
// presentation mapping.
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
    case Issue::OutputPathAlreadyUsed:
        return {
            UiTextId::OutputPathAlreadyUsed,
            "Output path is already used by another local labeling task.",
            "该输出路径已被另一个本地标注任务使用。"};
    case Issue::OutputMigrationCheckpointFailed:
        return {
            UiTextId::LabelingMigrationCheckpointFailed,
            "Could not checkpoint the legacy labeling owner before migration. The legacy owner remains active.",
            "迁移前无法保存旧标注所有者的恢复检查点。旧所有者仍保持活动状态。"};
    case Issue::OutputMigrationPublicationFailed:
        return {
            UiTextId::LabelingMigrationPublicationFailed,
            "Could not publish and reopen the migrated ASDF document. The legacy owner remains active.",
            "无法发布并重新打开迁移后的 ASDF 文档。旧所有者仍保持活动状态。"};
    case Issue::OutputMigrationOwnerSwitchFailed:
        return {
            UiTextId::LabelingMigrationOwnerSwitchFailed,
            "The ASDF document was created, but SpecForge could not persist the owner switch. The legacy owner remains active; retry migration to adopt the ASDF output.",
            "ASDF 文档已创建，但 SpecForge 无法持久化所有者切换。旧所有者仍保持活动状态；请重试迁移以采用该 ASDF 输出。"};
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
