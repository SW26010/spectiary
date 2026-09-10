#include "ui/source_collection_panel.h"

#include <imgui.h>
#include "imgui_widget_harness.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct AnnotationPanelFrameObservation {
    std::string logged_text;
    bool popup_open = false;
};

class AnnotationPanelFixture {
public:
    AnnotationPanelFixture()
    {
        snapshot = std::make_shared<specforge::SpectrumSnapshot>();
        snapshot->source.path = "source-a.npy";
        snapshot->collection.spectrum_count = 1;
        snapshot->collection.current_index = 0;
        view.snapshot = snapshot;
        view.current_sample_snapshot = snapshot;
        view.navigation.has_active_source = true;
        view.navigation.current_index = 0;
        view.navigation.sample_count = 1;
        view.labeling.has_active_source = true;
        view.labeling.source_identity = "source-a";
    }

    AnnotationPanelFrameObservation RenderFrame(bool = true)
    {
        ui.Frames();
        return observation;
    }

    void Render()
    {
        constexpr bool capture_text = true;
        ImGui::SetNextWindowPos(
            ImVec2(20.0f, 20.0f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(560.0f, 520.0f),
            ImGuiCond_Always);
        if (capture_text) {
            ImGui::LogToBuffer();
        }
        bool open = true;
        specforge::PanelSessionInteraction interaction(
            [this](
                specforge::SourceCollectionSessionIntent intent,
                std::optional<
                    specforge::NavigationLatencyInputKind>) {
                if (intent.intent_kind() ==
                    specforge::SourceCollectionSessionIntentKind::
                        SourceCollection) {
                    ++submit_count;
                    if (after_submit) {
                        after_submit();
                    }
                }
                specforge::SourceCollectionSessionResult result;
                result.loaded = submit_loaded;
                return result;
            },
            [this]() -> const specforge::SourceCollectionSessionView& {
                return view;
            });
        panel.RenderAnnotations(
            interaction,
            specforge::UiLanguage::English,
            &open,
            [this]() {
                ++choose_file_count;
                return selected_path;
            });

        observation = {};
        if (capture_text) {
            observation.logged_text =
                GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
        }
        observation.popup_open = ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId |
                ImGuiPopupFlags_AnyPopupLevel);

    }

    AnnotationPanelFrameObservation observation;
    specforge::test::WidgetHarness ui{[this] { Render(); }};
    std::shared_ptr<specforge::SpectrumSnapshot> snapshot;
    specforge::SourceCollectionSessionView view;
    specforge::SourceCollectionPanelUi panel;
    std::optional<std::filesystem::path> selected_path;
    std::function<void()> after_submit;
    bool submit_loaded = false;
    int choose_file_count = 0;
    int submit_count = 0;
};

std::size_t CountOccurrences(
    std::string_view text,
    std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t offset = 0;
         (offset = text.find(needle, offset)) !=
             std::string_view::npos;
         offset += needle.size()) {
        ++count;
    }
    return count;
}

specforge::SourceCollectionManifestDiagnostic
MakeAnnotationImportFailure(
    std::filesystem::path path,
    std::string detail)
{
    return {
        .kind = specforge::
            SourceCollectionManifestDiagnosticKind::
                AnnotationIgnored,
        .path = std::move(path),
        .detail = std::move(detail),
    };
}

void TestAnnotationImportFailureRendersInlineWithoutHoverOrPopup()
{
    AnnotationPanelFixture fixture;
    fixture.view.navigation.annotation_diagnostics.push_back(
        MakeAnnotationImportFailure(
            "initial-labels.csv",
            "CSV roster does not match the active source collection.\nExpected sample B at row 2."));
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);

    const AnnotationPanelFrameObservation observation =
        fixture.RenderFrame();
    Require(
        observation.logged_text.find("initial-labels.csv") !=
                std::string::npos &&
            observation.logged_text.find(
                "CSV roster does not match the active source collection.") !=
                std::string::npos &&
            observation.logged_text.find(
                "Expected sample B at row 2.") !=
                std::string::npos &&
            observation.logged_text.find("Dismiss") !=
                std::string::npos,
        "annotation import notice should draw filename, full multiline reason, and Dismiss without hover");
    Require(
        !observation.popup_open,
        "annotation import failure should remain inline rather than opening a modal");
}

void TestMissingLocalAnnotationRemovalRequiresConfirmation()
{
    AnnotationPanelFixture fixture;
    const std::filesystem::path missing_path =
        std::filesystem::path{"moved-away-labels.asdf"};
    fixture.view.navigation.current_annotations.push_back({
        .name = "Quality review",
        .path = missing_path,
        .relationship = specforge::
            SampleAnnotationWorkflowRelationship::
                LocalLabelingTask,
        .output_missing = true,
        .can_remove_annotation = true,
        .labeling_owner_format = specforge::
            SampleLabelingOutputArtifactFormat::
                CanonicalAsdf,
    });

    (void)fixture.RenderFrame();
    fixture.ui.Click("remove_annotation");

    const AnnotationPanelFrameObservation warning =
        fixture.RenderFrame();
    Require(
        fixture.submit_count == 0 &&
            warning.popup_open &&
            warning.logged_text.find(
                "Delete local task \"Quality review\"") !=
                std::string::npos &&
            warning.logged_text.find(
                "Output files are not deleted.") !=
                std::string::npos,
        "the first remove click should show an explicit warning without abandoning the task");

    fixture.ui.Click("SpecForgeConfirmRemoveMissingLocalLabelingTask");
    Require(
        fixture.submit_count == 1,
        "confirming the warning should submit the missing local task removal exactly once");
}

void TestSourceSwitchDismissesMissingLocalRemovalWarning()
{
    AnnotationPanelFixture fixture;
    fixture.view.navigation.current_annotations.push_back({
        .name = "Old source review",
        .path = "old-source-labels.asdf",
        .relationship = specforge::
            SampleAnnotationWorkflowRelationship::
                LocalLabelingTask,
        .output_missing = true,
        .can_remove_annotation = true,
        .labeling_owner_format = specforge::
            SampleLabelingOutputArtifactFormat::
                CanonicalAsdf,
    });

    (void)fixture.RenderFrame();
    fixture.ui.Click("remove_annotation");
    Require(
        fixture.RenderFrame().popup_open,
        "the old source removal warning should be open before switching sources");

    fixture.view.labeling.source_identity = "source-b";
    fixture.snapshot->source.path = "source-b.npy";
    fixture.view.navigation.current_annotations.clear();
    const AnnotationPanelFrameObservation switched =
        fixture.RenderFrame();
    Require(
        fixture.submit_count == 0 &&
            !switched.popup_open &&
            switched.logged_text.find(
                "Old source review") ==
                std::string::npos,
        "switching sources should dismiss the stale missing-task warning without submitting its deletion");
}

void TestAnnotationImportFailureDismissalTracksExactDetail()
{
    AnnotationPanelFixture fixture;
    fixture.view.navigation.annotation_diagnostics.push_back(
        MakeAnnotationImportFailure(
            "initial-labels.csv",
            "First roster mismatch detail."));
    (void)fixture.RenderFrame();
    fixture.ui.Click("SpecForgeAnnotationImportDiagnosticDismiss");
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    const AnnotationPanelFrameObservation dismissed =
        fixture.RenderFrame();
    Require(
        dismissed.logged_text.find("initial-labels.csv") ==
                std::string::npos &&
            dismissed.logged_text.find(
                "First roster mismatch detail.") ==
                std::string::npos,
        "dismissed diagnostic should stay hidden for this panel session");

    fixture.view.navigation.annotation_diagnostics.front().detail =
        "Changed roster mismatch detail.";
    const AnnotationPanelFrameObservation changed =
        fixture.RenderFrame();
    Require(
        changed.logged_text.find("initial-labels.csv") !=
                std::string::npos &&
            changed.logged_text.find(
                "Changed roster mismatch detail.") !=
                std::string::npos,
        "a changed diagnostic detail should produce a new visible dismissal key");
}

void TestReimportSameAnnotationClearsDismissalForNewFailure()
{
    AnnotationPanelFixture fixture;
    const std::filesystem::path diagnostic_path =
        std::filesystem::path{"imports"} /
        "nested" / ".." / "initial-labels.csv";
    const std::filesystem::path selected_path =
        std::filesystem::path{"imports"} /
        "initial-labels.csv";
    const auto diagnostic = MakeAnnotationImportFailure(
        diagnostic_path,
        "Roster mismatch on retry.");
    fixture.view.navigation.annotation_diagnostics.push_back(
        diagnostic);
    (void)fixture.RenderFrame();
    fixture.ui.Click("SpecForgeAnnotationImportDiagnosticDismiss");
    Require(
        fixture.RenderFrame().logged_text.find(
            "Roster mismatch on retry.") ==
            std::string::npos,
        "precondition: retry diagnostic should be dismissed");

    fixture.selected_path = selected_path;
    fixture.submit_loaded = false;
    fixture.after_submit = [&fixture, diagnostic]() {
        fixture.view.navigation.annotation_diagnostics.push_back(
            diagnostic);
    };
    fixture.ui.Click("SpecForgeAnnotationsAddFile");
    ImGui::GetIO().AddMousePosEvent(0.0f, 0.0f);
    const AnnotationPanelFrameObservation retried =
        fixture.RenderFrame();
    Require(
        fixture.choose_file_count == 1 &&
            fixture.submit_count == 1 &&
            CountOccurrences(
                retried.logged_text,
                "Roster mismatch on retry.") == 1,
        "same normalized path retry should submit once and reveal one latest failure card");

    fixture.ui.Click("SpecForgeAnnotationImportDiagnosticDismiss");
    fixture.submit_loaded = true;
    fixture.after_submit = {};
    fixture.ui.Click("SpecForgeAnnotationsAddFile");
    const AnnotationPanelFrameObservation successful =
        fixture.RenderFrame();
    Require(
        successful.logged_text.find(
            "Roster mismatch on retry.") ==
                std::string::npos &&
            !successful.popup_open,
        "successful retry should keep historical failure hidden without a modal or success toast");
}

void TestAnnotationDismissalsResetAcrossSourceSwitch()
{
    AnnotationPanelFixture fixture;
    const auto source_a_failure = MakeAnnotationImportFailure(
        "source-a.csv",
        "Source A failure.");
    fixture.view.navigation.annotation_diagnostics = {
        source_a_failure};
    (void)fixture.RenderFrame();
    fixture.ui.Click("SpecForgeAnnotationImportDiagnosticDismiss");

    fixture.view.labeling.source_identity = "source-b";
    fixture.snapshot->source.path = "source-b.npy";
    fixture.view.navigation.annotation_diagnostics = {
        MakeAnnotationImportFailure(
            "source-b.csv",
            "Source B failure.")};
    const AnnotationPanelFrameObservation source_b =
        fixture.RenderFrame();
    Require(
        source_b.logged_text.find("Source B failure.") !=
                std::string::npos &&
            source_b.logged_text.find("Source A failure.") ==
                std::string::npos,
        "source switch should show only the active source diagnostic");

    fixture.view.labeling.source_identity = "source-a";
    fixture.snapshot->source.path = "source-a.npy";
    fixture.view.navigation.annotation_diagnostics = {
        source_a_failure};
    const AnnotationPanelFrameObservation source_a_again =
        fixture.RenderFrame();
    Require(
        source_a_again.logged_text.find("Source A failure.") !=
            std::string::npos,
        "switching sources should clear prior-source transient dismissal state");
}

void TestHiddenAnnotationPanelObservesIntermediateSourceSwitch()
{
    AnnotationPanelFixture fixture;
    const auto source_a_failure = MakeAnnotationImportFailure(
        "source-a.csv",
        "Source A hidden-panel failure.");
    fixture.view.navigation.annotation_diagnostics = {
        source_a_failure};
    (void)fixture.RenderFrame();
    fixture.ui.Click("SpecForgeAnnotationImportDiagnosticDismiss");
    Require(
        fixture.RenderFrame().logged_text.find(
            "Source A hidden-panel failure.") ==
            std::string::npos,
        "precondition: source A diagnostic should be dismissed");

    fixture.view.labeling.source_identity = "source-b";
    fixture.snapshot->source.path = "source-b.npy";
    fixture.view.navigation.annotation_diagnostics = {
        MakeAnnotationImportFailure(
            "source-b.csv",
            "Source B hidden-panel failure.")};
    fixture.panel.SyncAnnotationDiagnosticSource(
        fixture.view);

    fixture.view.labeling.source_identity = "source-a";
    fixture.snapshot->source.path = "source-a.npy";
    fixture.view.navigation.annotation_diagnostics = {
        source_a_failure};
    fixture.panel.SyncAnnotationDiagnosticSource(
        fixture.view);

    const AnnotationPanelFrameObservation source_a_again =
        fixture.RenderFrame();
    Require(
        source_a_again.logged_text.find(
            "Source A hidden-panel failure.") !=
            std::string::npos,
        "an unrendered A-to-B-to-A source transition should clear source A dismissal state");
}

}

int main()
{
    TestAnnotationImportFailureRendersInlineWithoutHoverOrPopup();
    TestMissingLocalAnnotationRemovalRequiresConfirmation();
    TestSourceSwitchDismissesMissingLocalRemovalWarning();
    TestAnnotationImportFailureDismissalTracksExactDetail();
    TestReimportSameAnnotationClearsDismissalForNewFailure();
    TestAnnotationDismissalsResetAcrossSourceSwitch();
    TestHiddenAnnotationPanelObservesIntermediateSourceSwitch();
    return 0;
}
