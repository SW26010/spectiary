#pragma once

#include "ui/source_collection_session.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace specforge {

using SourceCollectionPathPicker = std::function<std::optional<std::filesystem::path>()>;

class SourceCollectionPanelUi {
public:
    [[nodiscard]] static const char* FilesWindowName();
    [[nodiscard]] static const char* NavigationWindowName();
    [[nodiscard]] static const char* AnnotationsWindowName();

    void SyncNavigationInputs(
        const SourceCollectionSessionView& session_view,
        const SourceCollectionSessionIntentSubmitter& submit);

    [[nodiscard]] SourceCollectionSessionAction RenderFiles(
        const SourceCollectionSessionView& session_view,
        const SourceCollectionSessionIntentSubmitter& submit,
        bool* open,
        const SourceCollectionPathPicker& choose_source_file,
        const SourceCollectionPathPicker& choose_source_folder);

    [[nodiscard]] SourceCollectionSessionAction RenderNavigation(
        const SourceCollectionSessionView& session_view,
        const SourceCollectionSessionIntentSubmitter& submit,
        bool* open);

    [[nodiscard]] SourceCollectionSessionAction RenderAnnotations(
        const SourceCollectionSessionView& session_view,
        const SourceCollectionSessionIntentSubmitter& submit,
        bool* open,
        const SourceCollectionPathPicker& choose_annotation_file);

private:
    void BeginSampleNameSearch(const SourceCollectionSessionView& session_view);
    void ClearSampleNameSearch();
    [[nodiscard]] SourceCollectionSessionAction RestoreFailedSampleNameSearch(
        const SourceCollectionSessionIntentSubmitter& submit);
    [[nodiscard]] SourceCollectionSessionAction CommitSampleNameSearch(
        std::size_t target_row,
        const std::string& matched_name,
        const SourceCollectionSessionIntentSubmitter& submit);
    [[nodiscard]] SourceCollectionSessionAction RenderSampleNameSearch(
        SourceCollectionSessionView session_view,
        const SourceCollectionSessionIntentSubmitter& submit);

    std::array<char, 32> row_index_buffer_ = {};
    std::array<char, 128> sample_name_query_buffer_ = {};
    bool sample_name_matches_open_ = false;
    bool sample_name_search_active_ = false;
    std::string sample_name_search_restore_name_;
};

}  // namespace specforge
