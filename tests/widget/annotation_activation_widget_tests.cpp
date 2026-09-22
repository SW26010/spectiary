#include "imgui_widget_harness.h"
#include "ui/source_collection_panel.h"
#include "ui/sample_workflow_panel.h"

#include <iostream>
#include <stdexcept>

namespace spectiary {
struct SourceCollectionPanelUiTestAccess {
    static bool IsAnnotationActivation(const SourceCollectionSessionIntent& intent,
        const std::filesystem::path& path)
    {
        return intent.kind == SourceCollectionSessionIntentKind::ActiveSampleWorkflow &&
            intent.active_sample_workflow.kind == ActiveSampleWorkflowIntentKind::ActivateLabelingTaskFromAnnotation &&
            intent.active_sample_workflow.path == path;
    }
};
}
namespace {
using namespace spectiary;
using test::WidgetHarness;
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void TestCanonicalAnnotationConfirmation()
{
    SourceCollectionSessionView view;
    auto snapshot = std::make_shared<SpectrumSnapshot>();
    snapshot->source.path = "source.npy";
    snapshot->collection.spectrum_count = 1;
    view.snapshot = snapshot;
    view.current_sample_snapshot = snapshot;
    view.navigation.has_active_source = true;
    view.navigation.sample_count = 1;
    view.navigation.current_index = 0;
    view.labeling.has_active_source = true;
    view.labeling.source_identity = "source";
    SourceCollectionAnnotationValueView annotation;
    annotation.name = "Canonical quality";
    annotation.path = "quality.asdf";
    annotation.relationship = SampleAnnotationWorkflowRelationship::ExternalLabelResult;
    annotation.labeling_owner_format = SampleLabelingOutputArtifactFormat::CanonicalAsdf;
    annotation.can_activate_labeling = true;
    view.navigation.current_annotations.push_back(annotation);
    SourceCollectionPanelUi source_panel;
    SampleWorkflowPanelUi workflow_panel;
    int submissions = 0;
    std::string text;
    PanelSessionInteraction interaction{
        [&](SourceCollectionSessionIntent intent, std::optional<NavigationLatencyInputKind>) {
            ++submissions;
            Require(SourceCollectionPanelUiTestAccess::IsAnnotationActivation(intent, annotation.path),
                "Confirmation must activate the dropped annotation path");
            return SourceCollectionSessionResult{};
        },
        [&]() -> const SourceCollectionSessionView& { return view; }};
    WidgetHarness ui{[&] {
        bool open = true;
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(440, 500), ImGuiCond_Always);
        source_panel.RenderAnnotations(interaction, UiLanguage::English, &open,
            []() -> std::optional<std::filesystem::path> { return {}; });
        ImGui::SetNextWindowPos(ImVec2(480, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(440, 500), ImGuiCond_Always);
        SampleWorkflowShortcut shortcut;
        ImGui::LogToBuffer();
        workflow_panel.RenderLabeling(interaction, &open,
            [](std::string_view) -> std::optional<std::filesystem::path> { return {}; },
            [](SampleLabelExportFormat, std::string_view) -> std::optional<std::filesystem::path> { return {}; },
            shortcut);
        text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    }};
    ui.Frames(3);
    const auto drop = [&] {
        const auto start = ui.Find("annotation_name").bounds.GetCenter();
        const auto target = ui.Find("##labeling_task_selector").bounds.GetCenter();
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(start.x, start.y); ui.Frames(2);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); ui.Frames();
        io.AddMousePosEvent(start.x + 15, start.y); ui.Frames(2);
        io.AddMousePosEvent(target.x, target.y); ui.Frames(3);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); ui.Frames(3);
        (void)ui.Find("ConfirmUseAnnotation");
        Require(text.find("Canonical quality") != std::string::npos && text.find("ASDF") != std::string::npos,
            "Canonical confirmation must identify the annotation and its ASDF owner");
        Require(text.find(UiText(UiLanguage::English, UiTextId::EditAnnotationInPlaceWarning)) != std::string::npos &&
            text.find(UiText(UiLanguage::English, UiTextId::ExistingLabelMetadataReused)) == std::string::npos &&
            text.find(UiText(UiLanguage::English, UiTextId::ImportedTaskSaveAsHint)) == std::string::npos,
            "Canonical confirmation must warn about in-place editing without legacy sidecar details");
        Require(submissions == 0, "Dropping an external annotation must wait for confirmation");
    };
    drop();
    ui.Click("CancelUseAnnotation");
    Require(submissions == 0 && !ui.Observe("ConfirmUseAnnotation"),
        "Cancel must close confirmation without activating the annotation");
    drop();
    ui.Click("ConfirmUseAnnotation");
    ui.Frames(3);
    Require(submissions == 1 && !ui.Observe("ConfirmUseAnnotation"),
        "Confirm must activate exactly once and close the dialog");
}
}
int main()
{
    try { TestCanonicalAnnotationConfirmation(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
