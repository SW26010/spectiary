#include "ui/spectral_lines_panel_controller.h"

#include "platform/exclusive_file_lease.h"
#include "overlays/spectral_line_user_state_cache_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

namespace specforge {
namespace {

using namespace std::chrono_literals;

constexpr auto kSaveDebounce = 500ms;
constexpr auto kSaveRetry = 10s;
constexpr auto kCommitLeaseRetryDelay = 10ms;
constexpr std::size_t kCommitLeaseMaximumAttempts = 200;

std::optional<ExclusiveFileLease> AcquireCatalogCommitLease(
    const std::filesystem::path& cache_path,
    std::string& diagnostic)
{
    const std::filesystem::path lease_path =
        CatalogUserStateCacheCommitLeasePath(cache_path);
    for (std::size_t attempt = 0;
         attempt < kCommitLeaseMaximumAttempts;
         ++attempt) {
        ExclusiveFileLeaseAcquireResult acquired =
            TryAcquireExclusiveFileLease(lease_path);
        if (acquired.status ==
            ExclusiveFileLeaseAcquireStatus::Acquired) {
            return std::move(acquired.lease);
        }
        if (acquired.status ==
            ExclusiveFileLeaseAcquireStatus::Failed) {
            diagnostic =
                acquired.error.empty()
                    ? "could not acquire catalog user-state commit lease: " +
                          lease_path.string()
                    : "could not acquire catalog user-state commit lease " +
                          lease_path.string() + ": " + acquired.error;
            return std::nullopt;
        }
        if (attempt + 1 < kCommitLeaseMaximumAttempts) {
            std::this_thread::sleep_for(kCommitLeaseRetryDelay);
        }
    }
    diagnostic =
        "catalog user-state commit lease remained busy for the bounded " +
        std::to_string(
            (kCommitLeaseMaximumAttempts - 1) *
            kCommitLeaseRetryDelay.count()) +
        " ms: " + lease_path.string();
    return std::nullopt;
}

SpectralLineCacheLoadIssueKind SpectralLineLoadIssueKind(
    CatalogUserStateCacheLoadIssueKind issue_kind)
{
    switch (issue_kind) {
    case CatalogUserStateCacheLoadIssueKind::ReadFailed:
        return SpectralLineCacheLoadIssueKind::ReadFailed;
    case CatalogUserStateCacheLoadIssueKind::InvalidDocument:
        return SpectralLineCacheLoadIssueKind::InvalidDocument;
    case CatalogUserStateCacheLoadIssueKind::
        UnsupportedFormatOrSchema:
        return SpectralLineCacheLoadIssueKind::
            UnsupportedFormatOrSchema;
    case CatalogUserStateCacheLoadIssueKind::None:
        break;
    }
    return SpectralLineCacheLoadIssueKind::None;
}

std::string TrimWhitespace(std::string_view value)
{
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string MarkerWavelengthText(const SpectralLineMarker& marker)
{
    std::array<char, 64> buffer = {};
    if (marker.kind == SpectralLineMarkerKind::Line && marker.vacuum_angstrom) {
        std::snprintf(buffer.data(), buffer.size(), "%.3f", *marker.vacuum_angstrom);
        return buffer.data();
    }
    if (marker.kind == SpectralLineMarkerKind::Band && marker.start_vacuum_angstrom &&
        marker.end_vacuum_angstrom) {
        std::snprintf(
            buffer.data(),
            buffer.size(),
            "%.3f-%.3f",
            *marker.start_vacuum_angstrom,
            *marker.end_vacuum_angstrom);
        return buffer.data();
    }
    return {};
}

bool GroupContainsMarker(
    const UserGroup& group,
    const CatalogIdentity& identity,
    std::string_view marker_id)
{
    return std::any_of(group.marker_references.begin(), group.marker_references.end(), [&](const auto& reference) {
        return SameCatalogIdentity(reference.catalog_identity, identity) && reference.marker_id == marker_id;
    });
}

}  // namespace

CatalogUserStateIntent::CatalogUserStateIntent(Kind kind) : kind_(kind) {}

CatalogUserStateIntent CatalogUserStateIntent::SetGroupingViewSearch(std::string query)
{
    CatalogUserStateIntent intent(Kind::SetGroupingViewSearch);
    intent.text_ = std::move(query);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SetMarkerLabelsVisible(bool visible)
{
    CatalogUserStateIntent intent(Kind::SetMarkerLabelsVisible);
    intent.enabled_ = visible;
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SelectGroupingView(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::SelectGroupingView);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::AcknowledgeGroupingViewSelection(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::AcknowledgeGroupingViewSelection);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::CreateUserGroupingView()
{
    return CatalogUserStateIntent(Kind::CreateUserGroupingView);
}

CatalogUserStateIntent CatalogUserStateIntent::DuplicateGroupingView(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::DuplicateGroupingView);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::RenameUserGroupingView(
    std::string view_id,
    std::string name,
    CatalogUserRenameEditState edit_state)
{
    CatalogUserStateIntent intent(Kind::RenameUserGroupingView);
    intent.view_id_ = std::move(view_id);
    intent.text_ = std::move(name);
    intent.rename_edit_state_ = edit_state;
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::DeleteUserGroupingView(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::DeleteUserGroupingView);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::AddUserGroup(std::string view_id)
{
    CatalogUserStateIntent intent(Kind::AddUserGroup);
    intent.view_id_ = std::move(view_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::MoveMarkerReferenceToNewGroup(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id)
{
    CatalogUserStateIntent intent(Kind::MoveMarkerReferenceToNewGroup);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::CopyMarkerReferenceToNewGroup(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id)
{
    CatalogUserStateIntent intent(Kind::CopyMarkerReferenceToNewGroup);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::RenameUserGroup(
    std::string view_id,
    std::string group_id,
    std::string name,
    CatalogUserRenameEditState edit_state)
{
    CatalogUserStateIntent intent(Kind::RenameUserGroup);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    intent.text_ = std::move(name);
    intent.rename_edit_state_ = edit_state;
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::DeleteUserGroup(std::string view_id, std::string group_id)
{
    CatalogUserStateIntent intent(Kind::DeleteUserGroup);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SetGroupMarkerVisibility(
    std::string view_id,
    std::string group_id,
    bool visible)
{
    CatalogUserStateIntent intent(Kind::SetGroupMarkerVisibility);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    intent.enabled_ = visible;
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SetGroupExpanded(
    std::string view_id,
    std::string group_id,
    bool expanded)
{
    CatalogUserStateIntent intent(Kind::SetGroupExpanded);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    intent.enabled_ = expanded;
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::ReorderUserGroupBefore(
    std::string view_id,
    std::string source_group_id,
    std::string target_group_id)
{
    CatalogUserStateIntent intent(Kind::ReorderUserGroupBefore);
    intent.view_id_ = std::move(view_id);
    intent.source_group_id_ = std::move(source_group_id);
    intent.target_group_id_ = std::move(target_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::MoveMarkerReference(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id,
    std::string target_group_id)
{
    CatalogUserStateIntent intent(Kind::MoveMarkerReference);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    intent.target_group_id_ = std::move(target_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::CopyMarkerReference(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id,
    std::string target_group_id)
{
    CatalogUserStateIntent intent(Kind::CopyMarkerReference);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    intent.target_group_id_ = std::move(target_group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::RemoveMarkerReference(
    std::string view_id,
    std::string marker_id,
    std::string group_id)
{
    CatalogUserStateIntent intent(Kind::RemoveMarkerReference);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.group_id_ = std::move(group_id);
    return intent;
}

CatalogUserStateIntent CatalogUserStateIntent::SetMarkerVisibility(std::string marker_id, bool visible)
{
    CatalogUserStateIntent intent(Kind::SetMarkerVisibility);
    intent.marker_id_ = std::move(marker_id);
    intent.enabled_ = visible;
    return intent;
}

SpectralLinesPanelController::SpectralLinesPanelController(
    std::filesystem::path packaged_catalog_path)
    : SpectralLinesPanelController(
          std::move(packaged_catalog_path),
          DefaultCatalogUserStateCachePath())
{
}

SpectralLinesPanelController::SpectralLinesPanelController(
    std::filesystem::path packaged_catalog_path,
    std::filesystem::path user_state_cache_path)
    : SpectralLinesPanelController(
          LoadPackagedPublicSpectralLineCatalog(packaged_catalog_path),
          PublicSpectralLineCatalogIdentity(),
          std::move(user_state_cache_path))
{
}

SpectralLinesPanelController::SpectralLinesPanelController(
    SpectralLineCatalog catalog,
    CatalogIdentity catalog_identity,
    std::filesystem::path user_state_cache_path)
    : catalog_(std::move(catalog)),
      catalog_identity_(std::move(catalog_identity)),
      catalog_grouping_view_(BuildCatalogGroupingView(catalog_, catalog_identity_)),
      user_state_cache_path_(std::move(user_state_cache_path)),
      cache_save_scheduler_(kSaveDebounce, kSaveRetry)
{
    CatalogUserStateCacheLoadResult load_result = LoadCatalogUserStateCache(user_state_cache_path_);
    if (load_result.issue_kind ==
            CatalogUserStateCacheLoadIssueKind::None &&
        load_result.requires_save) {
        CatalogUserStateCache legacy_validation;
        if (const auto match =
                load_result.cache.catalogs.find(catalog_identity_.id);
            match != load_result.cache.catalogs.end()) {
            legacy_validation.catalogs.emplace(
                catalog_identity_.id,
                match->second);
        }
        std::string legacy_validation_error;
        if (!ValidateCatalogUserStateCacheForLegacyMigration(
                legacy_validation,
                legacy_validation_error,
                load_result.schema_version)) {
            load_result.issue_kind =
                CatalogUserStateCacheLoadIssueKind::InvalidDocument;
            load_result.diagnostic_detail =
                "legacy catalog user-state cache is not trusted before "
                "canonicalization: " + legacy_validation_error;
            load_result.warning =
                "Ignored invalid legacy spectral-line grouping cache.";
            load_result.requires_save = false;
        }
    }
    const bool loaded_cache_requires_save =
        load_result.requires_save;
    const bool loaded_cache_can_rewrite =
        load_result.issue_kind ==
        CatalogUserStateCacheLoadIssueKind::None;
    user_state_cache_ = std::move(load_result.cache);
    load_issue_kind_ =
        SpectralLineLoadIssueKind(
            load_result.issue_kind);
    load_diagnostic_detail_ =
        std::move(load_result.diagnostic_detail);
    load_warning_ =
        load_diagnostic_detail_.empty()
            ? std::move(load_result.warning)
            : load_diagnostic_detail_;
    user_state_ = EnsureCatalogUserState(user_state_cache_, catalog_identity_);
    panel_state_ = EnsureCatalogPanelState(user_state_cache_, catalog_identity_);
    const CatalogUserStateCanonicalizationResult canonicalization =
        CanonicalizeCatalogUserState(
            user_state_,
            panel_state_,
            catalog_,
            catalog_identity_,
            catalog_grouping_view_);
    if (canonicalization.active_view_changed) {
        RequestGroupingViewSelection();
    }
    if (loaded_cache_can_rewrite &&
        (canonicalization.changed ||
         loaded_cache_requires_save)) {
        MarkCacheDirty();
    }

    // Capture the reconciliation base only after startup canonicalization.
    // Repairs such as trimming names or selecting a valid fallback view are
    // durable normalization, not an explicit task delta that may overwrite a
    // later peer write.
    reconciliation_base_state_ = user_state_;
    reconciliation_base_panel_state_ = panel_state_;
    next_view_sequence_ = std::max<std::uint64_t>(
        1,
        user_state_.next_view_sequence);
    next_group_sequence_ = std::max<std::uint64_t>(
        1,
        user_state_.next_group_sequence);
}

SpectralLinesPanelController::~SpectralLinesPanelController()
{
    (void)Flush();
}

CatalogUserStateResult SpectralLinesPanelController::Submit(CatalogUserStateIntent intent)
{
    switch (intent.kind_) {
    case CatalogUserStateIntent::Kind::SetGroupingViewSearch:
        if (grouping_view_search_ == intent.text_) {
            return NoChange();
        }
        grouping_view_search_ = std::move(intent.text_);
        return Applied(false);

    case CatalogUserStateIntent::Kind::SetMarkerLabelsVisible:
        if (marker_labels_visible_ == intent.enabled_) {
            return NoChange();
        }
        marker_labels_visible_ = intent.enabled_;
        return Applied(false);

    case CatalogUserStateIntent::Kind::SelectGroupingView:
        if (!ViewExists(intent.view_id_)) {
            return Rejected("Grouping view identity does not belong to this catalog user state.");
        }
        if (user_state_.active_view_id == intent.view_id_) {
            return NoChange();
        }
        user_state_.active_view_id = std::move(intent.view_id_);
        explicit_selection_intent_pending_ = true;
        return Applied(true);

    case CatalogUserStateIntent::Kind::AcknowledgeGroupingViewSelection:
        if (!grouping_view_selection_requested_ || user_state_.active_view_id != intent.view_id_) {
            return NoChange();
        }
        grouping_view_selection_requested_ = false;
        return Applied(false);

    case CatalogUserStateIntent::Kind::CreateUserGroupingView: {
        const std::string id = NextGroupingViewId();
        const std::size_t ordinal =
            user_state_.grouping_views.size() + 1;
        const std::string name =
            "Grouping " + std::to_string(ordinal);
        GeneratedNameMetadata generated_name;
        generated_name.source =
            GeneratedNameSource::DefaultGroupingView;
        generated_name.ordinal = ordinal;
        user_state_.grouping_views.push_back(
            CreateUserGroupingViewFromCatalog(
                catalog_,
                catalog_identity_,
                id,
                name,
                std::move(generated_name)));
        user_state_.active_view_id = id;
        explicit_selection_intent_pending_ = true;
        RequestGroupingViewSelection();
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::DuplicateGroupingView: {
        std::optional<GroupingView> source = EffectiveGroupingView(intent.view_id_);
        if (!source) {
            return Rejected("Grouping view identity does not belong to this catalog user state.");
        }
        const std::string id = NextGroupingViewId();
        const std::string name = source->name + " copy";
        user_state_.grouping_views.push_back(
            DuplicateGroupingView(*source, catalog_, catalog_identity_, id, name));
        user_state_.active_view_id = id;
        explicit_selection_intent_pending_ = true;
        RequestGroupingViewSelection();
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::RenameUserGroupingView: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        const std::string name = TrimWhitespace(intent.text_);
        if (view == nullptr || name.empty()) {
            return Rejected("Editable grouping view identity and a non-empty name are required.");
        }
        if (intent.rename_edit_state_ ==
            CatalogUserRenameEditState::Unedited) {
            return NoChange();
        }
        const bool name_changed =
            view->name != name;
        const bool metadata_changed =
            !(view->generated_name ==
              GeneratedNameMetadata{});
        if (!name_changed && !metadata_changed) {
            return NoChange();
        }
        if (name_changed) {
            view->name = name;
        }
        view->generated_name = {};
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::DeleteUserGroupingView: {
        const auto match = std::find_if(
            user_state_.grouping_views.begin(),
            user_state_.grouping_views.end(),
            [&](const GroupingView& view) {
                return view.id == intent.view_id_;
            });
        if (match == user_state_.grouping_views.end()) {
            return Rejected("Editable grouping view identity does not belong to this catalog user state.");
        }
        const std::string deleted_view_id = match->id;
        const bool deleted_active_view =
            user_state_.active_view_id == deleted_view_id;
        if (deleted_active_view) {
            // Applied() will canonicalize to a surviving view. That fallback
            // is derived state, not a competing explicit selection.
            explicit_selection_intent_pending_ = false;
        }
        user_state_.reserved_view_ids.insert(deleted_view_id);
        for (const UserGroup& group : match->groups) {
            if (!group.is_unassigned &&
                group.id != UnassignedUserGroupId()) {
                user_state_.reserved_group_ids.insert(group.id);
            }
        }
        user_state_.grouping_views.erase(match);
        for (auto iterator = panel_state_.expanded_group_ids.begin();
             iterator != panel_state_.expanded_group_ids.end();) {
            if (iterator->starts_with(deleted_view_id + "/")) {
                iterator = panel_state_.expanded_group_ids.erase(iterator);
            } else {
                ++iterator;
            }
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::AddUserGroup: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr) {
            return Rejected("Editable grouping view identity does not belong to this catalog user state.");
        }
        const std::string group_id = NextUserGroupId();
        const std::size_t ordinal =
            static_cast<std::size_t>(
                next_group_sequence_ - 1);
        const std::string group_name =
            "Group " + std::to_string(ordinal);
        GeneratedNameMetadata generated_name;
        generated_name.source =
            GeneratedNameSource::DefaultGroup;
        generated_name.ordinal = ordinal;
        if (!AddUserGroup(
                *view,
                group_id,
                group_name,
                std::move(generated_name))) {
            return Rejected("The user group could not be added without violating grouping view invariants.");
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::MoveMarkerReferenceToNewGroup:
    case CatalogUserStateIntent::Kind::CopyMarkerReferenceToNewGroup: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr) {
            return Rejected("Editable grouping view identity does not belong to this catalog user state.");
        }
        const UserGroup* source_group = FindUserGroup(*view, intent.source_group_id_);
        if (source_group == nullptr ||
            !GroupContainsMarker(*source_group, catalog_identity_, intent.marker_id_)) {
            return Rejected("Source group and marker reference identities do not belong to the grouping view.");
        }
        const bool copy = intent.kind_ == CatalogUserStateIntent::Kind::CopyMarkerReferenceToNewGroup;
        if (!copy && intent.source_group_id_ != UnassignedUserGroupId()) {
            return Rejected("Only an unassigned marker reference can be moved directly into a new group.");
        }

        const std::string group_id = NextUserGroupId();
        const std::size_t ordinal =
            static_cast<std::size_t>(
                next_group_sequence_ - 1);
        const std::string group_name =
            "Group " + std::to_string(ordinal);
        GeneratedNameMetadata generated_name;
        generated_name.source =
            GeneratedNameSource::DefaultGroup;
        generated_name.ordinal = ordinal;
        if (!AddUserGroup(
                *view,
                group_id,
                group_name,
                std::move(generated_name))) {
            return Rejected("The user group could not be added without violating grouping view invariants.");
        }
        const bool changed = copy
                                 ? specforge::CopyMarkerReference(
                                       *view,
                                       catalog_identity_,
                                       intent.marker_id_,
                                       group_id)
                                 : MoveMarkerReference(
                                       *view,
                                       catalog_identity_,
                                       intent.marker_id_,
                                       intent.source_group_id_,
                                       group_id);
        if (!changed) {
            (void)RemoveUserGroup(*view, group_id);
            return Rejected("The marker reference could not be placed in the new group.");
        }
        panel_state_.expanded_group_ids.insert(GroupExpansionKey(view->id, group_id));
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::RenameUserGroup: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        UserGroup* group = view == nullptr ? nullptr : FindUserGroup(*view, intent.group_id_);
        const std::string name = TrimWhitespace(intent.text_);
        if (group == nullptr || group->is_unassigned || group->id == UnassignedUserGroupId() || name.empty()) {
            return Rejected("Editable user group identities and a non-empty name are required.");
        }
        if (intent.rename_edit_state_ ==
            CatalogUserRenameEditState::Unedited) {
            return NoChange();
        }
        const bool name_changed =
            group->name != name;
        const bool metadata_changed =
            !(group->generated_name ==
              GeneratedNameMetadata{});
        if (!name_changed && !metadata_changed) {
            return NoChange();
        }
        if (name_changed) {
            group->name = name;
        }
        group->generated_name = {};
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::DeleteUserGroup: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr || !RemoveUserGroup(*view, intent.group_id_)) {
            return Rejected("Editable user group identity does not belong to the grouping view.");
        }
        user_state_.reserved_group_ids.insert(intent.group_id_);
        panel_state_.expanded_group_ids.erase(GroupExpansionKey(view->id, intent.group_id_));
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::SetGroupMarkerVisibility: {
        std::optional<GroupingView> view = EffectiveGroupingView(intent.view_id_);
        const UserGroup* group = view ? FindUserGroup(*view, intent.group_id_) : nullptr;
        if (group == nullptr) {
            return Rejected("Grouping view and group identities do not belong to this catalog user state.");
        }
        if (!grouping_view_search_.empty()) {
            return Rejected("Group marker visibility cannot be changed while grouping view search is active.");
        }
        if (!SetGroupMarkerVisibility(
                user_state_,
                *group,
                catalog_,
                catalog_identity_,
                intent.enabled_,
                false)) {
            return NoChange();
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::SetGroupExpanded: {
        std::optional<GroupingView> view = EffectiveGroupingView(intent.view_id_);
        if (!view || FindUserGroup(*view, intent.group_id_) == nullptr) {
            return Rejected("Grouping view and group identities do not belong to this catalog user state.");
        }
        const std::string key = GroupExpansionKey(intent.view_id_, intent.group_id_);
        const bool expanded = panel_state_.expanded_group_ids.contains(key);
        if (expanded == intent.enabled_) {
            return NoChange();
        }
        if (intent.enabled_) {
            panel_state_.expanded_group_ids.insert(key);
        } else {
            panel_state_.expanded_group_ids.erase(key);
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::ReorderUserGroupBefore: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr ||
            !ReorderUserGroupBefore(*view, intent.source_group_id_, intent.target_group_id_)) {
            return Rejected("User group identities cannot be reordered in this grouping view.");
        }
        explicit_group_ordering_view_ids_.insert(intent.view_id_);
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::MoveMarkerReference:
    case CatalogUserStateIntent::Kind::CopyMarkerReference: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        const UserGroup* source_group = view == nullptr ? nullptr : FindUserGroup(*view, intent.source_group_id_);
        if (view == nullptr || source_group == nullptr ||
            !GroupContainsMarker(*source_group, catalog_identity_, intent.marker_id_)) {
            return Rejected("Source group and marker reference identities do not belong to the grouping view.");
        }
        const bool copy = intent.kind_ == CatalogUserStateIntent::Kind::CopyMarkerReference;
        const bool changed = copy
                                 ? specforge::CopyMarkerReference(
                                       *view,
                                       catalog_identity_,
                                       intent.marker_id_,
                                       intent.target_group_id_)
                                 : MoveMarkerReference(
                                       *view,
                                       catalog_identity_,
                                       intent.marker_id_,
                                       intent.source_group_id_,
                                       intent.target_group_id_);
        if (!changed) {
            return Rejected("Marker reference identities cannot be moved or copied to the target group.");
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::RemoveMarkerReference: {
        GroupingView* view = FindUserGroupingView(intent.view_id_);
        if (view == nullptr || !specforge::RemoveMarkerReferenceFromGroup(
                                   *view,
                                   catalog_identity_,
                                   intent.marker_id_,
                                   intent.group_id_)) {
            return Rejected("Marker reference and group identities do not belong to the editable grouping view.");
        }
        return Applied(true);
    }

    case CatalogUserStateIntent::Kind::SetMarkerVisibility:
        if (!MarkerExists(intent.marker_id_)) {
            return Rejected("Marker identity does not belong to the current spectral-line catalog.");
        }
        if (IsMarkerVisible(user_state_, intent.marker_id_) == intent.enabled_) {
            return NoChange();
        }
        SetMarkerVisible(user_state_, intent.marker_id_, intent.enabled_);
        return Applied(true);
    }

    return Rejected("Unknown catalog user state intent.");
}

CatalogUserStateView SpectralLinesPanelController::View() const
{
    CatalogUserStateView result;
    result.catalog_id = catalog_identity_.id;
    result.catalog_display_name = catalog_identity_.display_name;
    result.catalog_load_error = catalog_.load_error;
    result.persistence.retrying =
        cache_save_status_.failed();
    result.persistence.recovered =
        cache_save_status_.recovered();
    result.persistence.load_issue =
        load_issue_kind_;
    result.persistence.load_diagnostic_detail =
        load_diagnostic_detail_;
    result.persistence.save_diagnostic_detail =
        cache_save_status_.message();
    result.grouping_view_search = grouping_view_search_;
    result.marker_labels_visible = marker_labels_visible_;
    result.has_catalog_grouping_view = catalog_grouping_view_.has_value();
    result.user_grouping_view_count = user_state_.grouping_views.size();
    result.catalog_marker_count = catalog_.markers.size();
    result.grouping_views.reserve(user_state_.grouping_views.size() + (catalog_grouping_view_ ? 1U : 0U));

    const auto append_view = [&](const GroupingView& view, bool editable) {
        SpectralLineGroupingView grouping_view;
        grouping_view.id = view.id;
        grouping_view.name = view.name;
        grouping_view.generated_name =
            view.generated_name;
        grouping_view.editable = editable;
        grouping_view.active = user_state_.active_view_id == view.id;
        grouping_view.selection_requested = grouping_view.active && grouping_view_selection_requested_;
        grouping_view.search_active = !grouping_view_search_.empty();
        grouping_view.groups.reserve(view.groups.size());

        const std::unordered_map<std::string, int> reference_counts =
            MarkerReferenceCounts(view, catalog_identity_);
        for (const UserGroup& group : view.groups) {
            SpectralLineGroupView group_view;
            group_view.id = group.id;
            group_view.name = group.name;
            group_view.generated_name =
                group.generated_name;
            group_view.is_unassigned = group.is_unassigned || group.id == UnassignedUserGroupId();
            group_view.expanded =
                panel_state_.expanded_group_ids.contains(GroupExpansionKey(view.id, group.id));
            group_view.visibility = VisibilityStateForGroup(
                user_state_,
                group,
                catalog_,
                catalog_identity_,
                grouping_view.search_active);

            for (const MarkerReference& reference : group.marker_references) {
                if (!MarkerMatchesSearch(catalog_, catalog_identity_, reference, grouping_view_search_)) {
                    continue;
                }
                const SpectralLineMarker* marker = FindCatalogMarker(catalog_, catalog_identity_, reference);
                SpectralLineMarkerReferenceView marker_view;
                marker_view.marker_id = reference.marker_id;
                marker_view.resolved = marker != nullptr;
                marker_view.visible = marker != nullptr && IsMarkerVisible(user_state_, reference.marker_id);
                marker_view.shared = IsSharedMarkerReference(reference_counts, reference);
                if (marker != nullptr) {
                    marker_view.label = marker->label;
                    marker_view.wavelength_text = MarkerWavelengthText(*marker);
                    marker_view.notes = marker->notes;
                } else {
                    marker_view.label = reference.marker_id;
                }
                group_view.marker_references.push_back(std::move(marker_view));
            }
            group_view.dimmed_by_search =
                grouping_view.search_active && group_view.marker_references.empty();
            grouping_view.groups.push_back(std::move(group_view));
        }
        result.grouping_views.push_back(std::move(grouping_view));
    };

    if (catalog_grouping_view_) {
        append_view(*catalog_grouping_view_, false);
    }
    for (const GroupingView& stored_view : user_state_.grouping_views) {
        append_view(stored_view, true);
    }
    return result;
}

SpectralLinePlotView SpectralLinesPanelController::PlotView(
    const SpectrumSnapshotHandle& snapshot) const
{
    SpectralLinePlotView result;
    result.marker_labels_visible = marker_labels_visible_;
    result.layout_scope_id = catalog_identity_.id;
    if (!snapshot || !snapshot->capabilities.can_show_spectral_lines || catalog_.markers.empty()) {
        return result;
    }
    result.visible_markers.reserve(catalog_.markers.size());
    for (const SpectralLineMarker& marker : catalog_.markers) {
        if (IsMarkerVisible(user_state_, marker.id)) {
            result.visible_markers.push_back(&marker);
        }
    }
    return result;
}

LocalUserStatePersistenceStatus
SpectralLinesPanelController::PersistenceStatus() const
{
    return {
        .retrying = cache_save_status_.failed(),
        .recovered = cache_save_status_.recovered(),
        .load_warning = load_warning_,
        .save_message = cache_save_status_.message(),
        .load_diagnostic_detail =
            load_diagnostic_detail_,
        .save_diagnostic_detail =
            cache_save_status_.message(),
    };
}

void SpectralLinesPanelController::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{
    if (cache_save_scheduler_.ShouldAttemptSave(now)) {
        (void)Flush();
    }
}

std::optional<LocalUserStateSaveScheduler::TimePoint>
SpectralLinesPanelController::NextMaintenanceDeadline() const
{
    return cache_save_scheduler_.next_attempt_time();
}

bool SpectralLinesPanelController::Flush()
{
    if (!cache_save_scheduler_.dirty()) {
        return load_issue_kind_ == SpectralLineCacheLoadIssueKind::None;
    }

    std::string error;
    std::optional<ExclusiveFileLease> commit_lease =
        AcquireCatalogCommitLease(
            user_state_cache_path_,
            error);
    if (!commit_lease) {
        cache_save_scheduler_.MarkSaveFailed(
            cache_save_status_,
            std::move(error));
        return false;
    }

    std::error_code latest_exists_error;
    (void)std::filesystem::exists(
        user_state_cache_path_,
        latest_exists_error);
    if (latest_exists_error) {
        error =
            "could not inspect latest catalog user-state cache " +
            user_state_cache_path_.string() + ": " +
            latest_exists_error.message();
        cache_save_scheduler_.MarkSaveFailed(
            cache_save_status_,
            std::move(error));
        return false;
    }

    CatalogUserStateCacheLoadResult latest_load =
        LoadCatalogUserStateCache(user_state_cache_path_);
    if (latest_load.issue_kind !=
        CatalogUserStateCacheLoadIssueKind::None) {
        error =
            "latest durable catalog user-state cache is not trusted " +
            user_state_cache_path_.string() + ": " +
            (latest_load.diagnostic_detail.empty()
                 ? latest_load.warning
                 : latest_load.diagnostic_detail);
        cache_save_scheduler_.MarkSaveFailed(
            cache_save_status_,
            std::move(error));
        return false;
    }

    CatalogUserState latest_state =
        MakeCatalogUserState(catalog_identity_);
    if (latest_load.requires_save) {
        // Schema-one/two/three state is a supported migration input only
        // when every persisted entry belongs to this controller's catalog.
        // Without the corresponding domain catalog, another legacy entry
        // cannot be canonicalized and validated before the whole document is
        // rewritten as schema four.
        const bool has_unrelated_catalog = std::any_of(
            latest_load.cache.catalogs.begin(),
            latest_load.cache.catalogs.end(),
            [&](const auto& entry) {
                return entry.first != catalog_identity_.id;
            });
        const bool has_unrelated_panel_state = std::any_of(
            latest_load.cache.catalog_panel_state.begin(),
            latest_load.cache.catalog_panel_state.end(),
            [&](const auto& entry) {
                return entry.first != catalog_identity_.id;
            });
        if (has_unrelated_catalog || has_unrelated_panel_state) {
            error =
                "legacy catalog user-state cache contains unrelated catalog "
                "entries and cannot be safely migrated to schema 4";
            cache_save_scheduler_.MarkSaveFailed(
                cache_save_status_,
                std::move(error));
            return false;
        }

        CatalogUserStateCache legacy_validation;
        if (const auto match =
                latest_load.cache.catalogs.find(catalog_identity_.id);
            match != latest_load.cache.catalogs.end()) {
            legacy_validation.catalogs.emplace(
                catalog_identity_.id,
                match->second);
        }
        if (!ValidateCatalogUserStateCacheForLegacyMigration(
                legacy_validation,
                error,
                latest_load.schema_version)) {
            error =
                "latest durable catalog user-state cache is not trusted for "
                "legacy migration before canonicalization: " + error;
            cache_save_scheduler_.MarkSaveFailed(
                cache_save_status_,
                std::move(error));
            return false;
        }

        // Legacy grouping entries predate the explicit unassigned-group flag,
        // so normalize the current catalog before validating the migration.
        if (const auto match =
                latest_load.cache.catalogs.find(catalog_identity_.id);
            match != latest_load.cache.catalogs.end()) {
            CatalogPanelState& legacy_panel_state =
                latest_load.cache.catalog_panel_state[catalog_identity_.id];
            (void)CanonicalizeCatalogUserState(
                match->second,
                legacy_panel_state,
                catalog_,
                catalog_identity_,
                catalog_grouping_view_);
        }

        legacy_validation.catalogs.clear();
        if (const auto match =
                latest_load.cache.catalogs.find(catalog_identity_.id);
            match != latest_load.cache.catalogs.end()) {
            legacy_validation.catalogs.emplace(
                catalog_identity_.id,
                match->second);
        }
        if (!ValidateCatalogUserStateCacheForReconciliation(
                legacy_validation,
                error)) {
            error =
                "latest durable catalog user-state cache is not trusted for "
                "legacy migration: " + error;
            cache_save_scheduler_.MarkSaveFailed(
                cache_save_status_,
                std::move(error));
            return false;
        }
    } else if (!ValidateCatalogUserStateCacheForReconciliation(
                   latest_load.cache,
                   error,
                   true)) {
        // Current-schema state must be trusted before every replacement,
        // including startup canonicalization and maintenance/destructor
        // flushes that carry no explicit task delta.
        error =
            "latest durable catalog user-state cache is not trusted before "
            "replacement: " + error;
        cache_save_scheduler_.MarkSaveFailed(
            cache_save_status_,
            std::move(error));
        return false;
    }
    if (const auto match = latest_load.cache.catalogs.find(catalog_identity_.id);
        match != latest_load.cache.catalogs.end()) {
        latest_state = match->second;
    }
    CatalogPanelState latest_panel_state;
    if (const auto match = latest_load.cache.catalog_panel_state.find(catalog_identity_.id);
        match != latest_load.cache.catalog_panel_state.end()) {
        latest_panel_state = match->second;
    }

    CatalogUserStateReconciliationResult reconciled;
    if (!ReconcileCatalogUserStateTask(
            reconciliation_base_state_,
            user_state_,
            latest_state,
            reconciliation_base_panel_state_,
            panel_state_,
            latest_panel_state,
            reconciled,
            error,
            explicit_selection_intent_pending_,
            explicit_group_ordering_view_ids_)) {
        cache_save_scheduler_.MarkSaveFailed(
            cache_save_status_,
            std::move(error));
        return false;
    }

    const CatalogUserStateCanonicalizationResult canonicalization =
        CanonicalizeCatalogUserState(
            reconciled.state,
            reconciled.panel_state,
            catalog_,
            catalog_identity_,
            catalog_grouping_view_);
    if (canonicalization.active_view_changed) {
        RequestGroupingViewSelection();
    }

    CatalogUserStateCache merged_cache =
        std::move(latest_load.cache);
    merged_cache.catalogs[catalog_identity_.id] =
        reconciled.state;
    merged_cache.catalog_panel_state[catalog_identity_.id] =
        reconciled.panel_state;

    if (!SaveCatalogUserStateCache(
            user_state_cache_path_,
            merged_cache,
            error)) {
        cache_save_scheduler_.MarkSaveFailed(
            cache_save_status_,
            std::move(error));
        return false;
    }

    user_state_cache_ = std::move(merged_cache);
    user_state_ = user_state_cache_.catalogs.at(catalog_identity_.id);
    panel_state_ = user_state_cache_.catalog_panel_state.at(catalog_identity_.id);
    reconciliation_base_state_ = user_state_;
    reconciliation_base_panel_state_ = panel_state_;
    next_view_sequence_ = std::max<std::uint64_t>(
        1,
        user_state_.next_view_sequence);
    next_group_sequence_ = std::max<std::uint64_t>(
        1,
        user_state_.next_group_sequence);
    load_issue_kind_ =
        SpectralLineCacheLoadIssueKind::None;
    load_warning_.clear();
    load_diagnostic_detail_.clear();
    explicit_selection_intent_pending_ = false;
    explicit_group_ordering_view_ids_.clear();
    explicit_task_delta_pending_ = false;
    cache_save_scheduler_.MarkSaveSucceeded(cache_save_status_);
    return true;
}

CatalogUserStateResult SpectralLinesPanelController::Applied(bool persistent_state_changed)
{
    if (persistent_state_changed) {
        explicit_task_delta_pending_ = true;
        const CatalogUserStateCanonicalizationResult canonicalization =
            CanonicalizeCatalogUserState(
                user_state_,
                panel_state_,
                catalog_,
                catalog_identity_,
                catalog_grouping_view_);
        if (canonicalization.active_view_changed) {
            RequestGroupingViewSelection();
        }
        MarkCacheDirty();
    }
    CatalogUserStateResult result;
    result.status = CatalogUserStateResultStatus::Applied;
    result.changed = true;
    result.persistent_state_changed = persistent_state_changed;
    return result;
}

CatalogUserStateResult SpectralLinesPanelController::NoChange()
{
    CatalogUserStateResult result;
    result.status = CatalogUserStateResultStatus::NoChange;
    return result;
}

CatalogUserStateResult SpectralLinesPanelController::Rejected(std::string message)
{
    CatalogUserStateResult result;
    result.status = CatalogUserStateResultStatus::Rejected;
    result.message = std::move(message);
    return result;
}

bool SpectralLinesPanelController::ViewExists(std::string_view view_id) const
{
    if (view_id.empty()) {
        return false;
    }
    if (catalog_grouping_view_ && catalog_grouping_view_->id == view_id) {
        return true;
    }
    return FindUserGroupingView(view_id) != nullptr;
}

bool SpectralLinesPanelController::MarkerExists(std::string_view marker_id) const
{
    return !marker_id.empty() &&
           std::any_of(catalog_.markers.begin(), catalog_.markers.end(), [&](const SpectralLineMarker& marker) {
               return marker.id == marker_id;
           });
}

GroupingView* SpectralLinesPanelController::FindUserGroupingView(std::string_view view_id)
{
    const auto match = std::find_if(
        user_state_.grouping_views.begin(),
        user_state_.grouping_views.end(),
        [&](const GroupingView& view) {
            return view.id == view_id;
        });
    return match == user_state_.grouping_views.end() ? nullptr : &(*match);
}

const GroupingView* SpectralLinesPanelController::FindUserGroupingView(std::string_view view_id) const
{
    const auto match = std::find_if(
        user_state_.grouping_views.begin(),
        user_state_.grouping_views.end(),
        [&](const GroupingView& view) {
            return view.id == view_id;
        });
    return match == user_state_.grouping_views.end() ? nullptr : &(*match);
}

std::optional<GroupingView> SpectralLinesPanelController::EffectiveGroupingView(std::string_view view_id) const
{
    if (catalog_grouping_view_ && catalog_grouping_view_->id == view_id) {
        return catalog_grouping_view_;
    }
    const GroupingView* view = FindUserGroupingView(view_id);
    if (view == nullptr) {
        return std::nullopt;
    }
    return EffectiveUserGroupingView(*view, catalog_, catalog_identity_);
}

UserGroup* SpectralLinesPanelController::FindUserGroup(GroupingView& view, std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [&](const UserGroup& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

const UserGroup* SpectralLinesPanelController::FindUserGroup(
    const GroupingView& view,
    std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [&](const UserGroup& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

std::string SpectralLinesPanelController::NextGroupingViewId()
{
    for (;;) {
        const std::uint64_t sequence = next_view_sequence_++;
        std::string id = "view-" + std::to_string(sequence);
        if (!ViewExists(id) &&
            !user_state_.reserved_view_ids.contains(id) &&
            id != CatalogGroupingViewId()) {
            user_state_.reserved_view_ids.insert(id);
            user_state_.next_view_sequence = next_view_sequence_;
            return id;
        }
    }
}

std::string SpectralLinesPanelController::NextUserGroupId()
{
    for (;;) {
        const std::uint64_t sequence = next_group_sequence_++;
        std::string id = "group-" + std::to_string(sequence);
        bool exists = false;
        for (const GroupingView& view : user_state_.grouping_views) {
            exists = exists || FindUserGroup(view, id) != nullptr;
        }
        if (!exists &&
            !user_state_.reserved_group_ids.contains(id) &&
            id != UnassignedUserGroupId()) {
            user_state_.reserved_group_ids.insert(id);
            user_state_.next_group_sequence = next_group_sequence_;
            return id;
        }
    }
}

void SpectralLinesPanelController::MarkCacheDirty()
{
    cache_save_status_.ClearRecovered();
    cache_save_scheduler_.MarkDirty();
}

void SpectralLinesPanelController::RequestGroupingViewSelection()
{
    grouping_view_selection_requested_ = true;
}

}  // namespace specforge
