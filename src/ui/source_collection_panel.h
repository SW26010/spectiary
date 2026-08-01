#pragma once

#include "ui/panel_session_interaction.h"
#include "ui/sample_workflow_shortcut.h"
#include "ui/ui_text.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace specforge {

using SourceCollectionPathPicker = std::function<std::optional<std::filesystem::path>()>;
using SourceCollectionPathOpener = std::function<void(const std::filesystem::path&)>;
class SourceCollectionPanelUi {
public:
    [[nodiscard]] static const char* FilesWindowName();
    [[nodiscard]] static const char* NavigationWindowName();
    [[nodiscard]] static const char* AnnotationsWindowName();

    void SyncNavigationInputs(const SourceCollectionNavigationView& navigation);
    void FinalizeNavigationInputEdits(
        PanelSessionInteraction& interaction);

    void RenderFiles(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool* open,
        const SourceCollectionPathPicker& choose_source_file,
        const SourceCollectionPathPicker& choose_source_folder,
        const SourceCollectionPathOpener& open_source);

    void RenderNavigation(
        PanelSessionInteraction& interaction,
        bool live_numeric_navigation,
        bool* open,
        SampleWorkflowShortcut& shortcut);
    void RenderNavigation(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool live_numeric_navigation,
        bool* open,
        SampleWorkflowShortcut& shortcut);

    void RenderAnnotations(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool* open,
        const SourceCollectionPathPicker& choose_annotation_file);

private:
    struct NavigationNumberInputCommit {
        std::string draft;
        std::uint64_t topology_revision = 0;
    };

    struct NavigationNumberInputEdit {
        std::array<char, 32> buffer = {};
        std::optional<std::uint64_t> edit_topology_revision;
        std::optional<NavigationNumberInputCommit> blur_commit;
        std::string edit_initial_value;
        bool edit_active = false;
        bool edit_dirty = false;
        bool live_submission_enabled = false;
        bool input_rendered_since_finalize = false;
        bool reload_deactivate_pending = false;
    };

    using NavigationRequestFactory =
        SampleNavigationRequest (*)(std::size_t);

    void BeginSampleNameSearch(const SourceCollectionNavigationView& navigation);
    void ClearSampleNameSearch();
    void RestoreFailedSampleNameSearch(
        PanelSessionInteraction& interaction);
    void CommitSampleNameSearch(
        std::size_t target_row,
        const std::string& matched_name,
        PanelSessionInteraction& interaction);
    void RenderSampleNameSearch(
        SourceCollectionNavigationView navigation,
        PanelSessionInteraction& interaction,
        UiLanguage language);
    void SyncNavigationNumberInput(
        NavigationNumberInputEdit& input,
        std::string_view synchronized_value,
        bool enabled,
        bool topology_changed,
        const char* input_id);
    void RenderNavigationNumberInput(
        NavigationNumberInputEdit& input,
        const char* input_id,
        bool live_numeric_navigation,
        bool enabled,
        std::size_t target_count,
        std::uint64_t topology_revision,
        NavigationRequestFactory make_request,
        SourceCollectionNavigationView& navigation,
        PanelSessionInteraction& interaction);
    [[nodiscard]] std::optional<NavigationNumberInputCommit>
    FinalizeNavigationNumberInput(
        NavigationNumberInputEdit& input,
        const char* input_id,
        PanelSessionInteraction& interaction);
    void ApplyNavigationNumberInputCommit(
        NavigationNumberInputCommit commit,
        bool enabled,
        std::size_t target_count,
        std::uint64_t topology_revision,
        NavigationRequestFactory make_request,
        SourceCollectionNavigationView& navigation,
        PanelSessionInteraction& interaction);
    static void ResetNavigationNumberInputEdit(
        NavigationNumberInputEdit& input);
    static void ReloadNavigationNumberInputFromBuffer(
        NavigationNumberInputEdit& input,
        const char* input_id);

    NavigationNumberInputEdit source_row_input_;
    NavigationNumberInputEdit sequence_position_input_;
    std::optional<std::uint64_t>
        synchronized_navigation_topology_revision_;
    std::array<char, 128> sample_name_query_buffer_ = {};
    std::array<char, 128> annotation_display_name_buffer_ = {};
    bool sample_name_matches_open_ = false;
    bool sample_name_search_active_ = false;
    bool annotation_display_name_focus_pending_ = false;
    std::string displayed_sample_name_;
    std::string sample_name_search_restore_name_;
    std::string annotation_display_name_edit_key_;
};

}  // namespace specforge
