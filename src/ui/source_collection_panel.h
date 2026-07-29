#pragma once

#include "ui/panel_session_interaction.h"
#include "ui/sample_workflow_shortcut.h"
#include "ui/ui_text.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace specforge {

using SourceCollectionPathPicker = std::function<std::optional<std::filesystem::path>()>;
using SourceCollectionPathOpener = std::function<void(const std::filesystem::path&)>;
class SourceCollectionPanelUi {
public:
    [[nodiscard]] static const char* FilesWindowName();
    [[nodiscard]] static const char* NavigationWindowName();
    [[nodiscard]] static const char* AnnotationsWindowName();

    void SyncNavigationInputs(const SourceCollectionNavigationView& navigation);

    void RenderFiles(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool* open,
        const SourceCollectionPathPicker& choose_source_file,
        const SourceCollectionPathPicker& choose_source_folder,
        const SourceCollectionPathOpener& open_source);

    void RenderNavigation(
        PanelSessionInteraction& interaction,
        bool* open,
        SampleWorkflowShortcut& shortcut);
    void RenderNavigation(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool* open,
        SampleWorkflowShortcut& shortcut);

    void RenderAnnotations(
        PanelSessionInteraction& interaction,
        UiLanguage language,
        bool* open,
        const SourceCollectionPathPicker& choose_annotation_file);

private:
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

    std::array<char, 32> row_index_buffer_ = {};
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
