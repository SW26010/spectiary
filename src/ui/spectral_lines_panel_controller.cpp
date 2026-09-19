#include "ui/spectral_lines_panel_controller.h"
#include "domain/uuid_v4.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace spectiary {
namespace {
SpectralLineCacheLoadIssueKind PanelLoadIssue(VersionedJsonCacheLoadIssueKind issue)
{
    switch (issue) {
    case VersionedJsonCacheLoadIssueKind::None:
        return SpectralLineCacheLoadIssueKind::None;
    case VersionedJsonCacheLoadIssueKind::ReadFailed:
        return SpectralLineCacheLoadIssueKind::ReadFailed;
    case VersionedJsonCacheLoadIssueKind::InvalidDocument:
        return SpectralLineCacheLoadIssueKind::InvalidDocument;
    case VersionedJsonCacheLoadIssueKind::UnsupportedFormatOrSchema:
        return SpectralLineCacheLoadIssueKind::UnsupportedFormatOrSchema;
    }
    return SpectralLineCacheLoadIssueKind::InvalidDocument;
}
std::string Trim(std::string value) {
    const auto blank = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && blank(value.front())) value.erase(value.begin());
    while (!value.empty() && blank(value.back())) value.pop_back();
    return value;
}
std::string WavelengthText(const line_list::Marker& marker) {
    std::array<char, 96> text{};
    if (marker.coordinate) std::snprintf(text.data(), text.size(), "%.3f", *marker.coordinate);
    else std::snprintf(text.data(), text.size(), "%.3f-%.3f", *marker.start, *marker.end);
    return text.data();
}
bool Matches(std::string text, std::string query) {
    const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::transform(text.begin(), text.end(), text.begin(), lower);
    std::transform(query.begin(), query.end(), query.begin(), lower);
    return text.find(query) != std::string::npos;
}
GeneratedNameMetadata NameMetadata(const std::unordered_map<std::string, GeneratedNameMetadata>& names, const std::string& id) {
    const auto found = names.find(id);
    return found == names.end() ? GeneratedNameMetadata{} : found->second;
}
} // namespace
SpectralLineStateIntent::SpectralLineStateIntent(Kind kind) : kind_(kind) {}

SpectralLineStateIntent SpectralLineStateIntent::SetGroupingViewSearch(std::string query)
{
    SpectralLineStateIntent intent(Kind::SetGroupingViewSearch);
    intent.text_ = std::move(query);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::SetMarkerLabelsVisible(bool visible)
{
    SpectralLineStateIntent intent(Kind::SetMarkerLabelsVisible);
    intent.enabled_ = visible;
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::SelectGroupingView(std::string view_id)
{
    SpectralLineStateIntent intent(Kind::SelectGroupingView);
    intent.view_id_ = std::move(view_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::AcknowledgeGroupingViewSelection(std::string view_id)
{
    SpectralLineStateIntent intent(Kind::AcknowledgeGroupingViewSelection);
    intent.view_id_ = std::move(view_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::CreateUserGroupingView()
{
    return SpectralLineStateIntent(Kind::CreateUserGroupingView);
}

SpectralLineStateIntent SpectralLineStateIntent::DuplicateGroupingView(std::string view_id)
{
    SpectralLineStateIntent intent(Kind::DuplicateGroupingView);
    intent.view_id_ = std::move(view_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::RenameUserGroupingView(
    std::string view_id,
    std::string name,
    SpectralLineRenameEditState edit_state)
{
    SpectralLineStateIntent intent(Kind::RenameUserGroupingView);
    intent.view_id_ = std::move(view_id);
    intent.text_ = std::move(name);
    intent.rename_edit_state_ = edit_state;
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::DeleteUserGroupingView(std::string view_id)
{
    SpectralLineStateIntent intent(Kind::DeleteUserGroupingView);
    intent.view_id_ = std::move(view_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::AddUserGroup(std::string view_id)
{
    SpectralLineStateIntent intent(Kind::AddUserGroup);
    intent.view_id_ = std::move(view_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::MoveMarkerReferenceToNewGroup(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id)
{
    SpectralLineStateIntent intent(Kind::MoveMarkerReferenceToNewGroup);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::CopyMarkerReferenceToNewGroup(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id)
{
    SpectralLineStateIntent intent(Kind::CopyMarkerReferenceToNewGroup);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::RenameUserGroup(
    std::string view_id,
    std::string group_id,
    std::string name,
    SpectralLineRenameEditState edit_state)
{
    SpectralLineStateIntent intent(Kind::RenameUserGroup);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    intent.text_ = std::move(name);
    intent.rename_edit_state_ = edit_state;
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::DeleteUserGroup(std::string view_id, std::string group_id)
{
    SpectralLineStateIntent intent(Kind::DeleteUserGroup);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::SetGroupMarkerVisibility(
    std::string view_id,
    std::string group_id,
    bool visible)
{
    SpectralLineStateIntent intent(Kind::SetGroupMarkerVisibility);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    intent.enabled_ = visible;
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::SetGroupExpanded(
    std::string view_id,
    std::string group_id,
    bool expanded)
{
    SpectralLineStateIntent intent(Kind::SetGroupExpanded);
    intent.view_id_ = std::move(view_id);
    intent.group_id_ = std::move(group_id);
    intent.enabled_ = expanded;
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::ReorderUserGroupBefore(
    std::string view_id,
    std::string source_group_id,
    std::string target_group_id)
{
    SpectralLineStateIntent intent(Kind::ReorderUserGroupBefore);
    intent.view_id_ = std::move(view_id);
    intent.source_group_id_ = std::move(source_group_id);
    intent.target_group_id_ = std::move(target_group_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::MoveMarkerReference(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id,
    std::string target_group_id)
{
    SpectralLineStateIntent intent(Kind::MoveMarkerReference);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    intent.target_group_id_ = std::move(target_group_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::CopyMarkerReference(
    std::string view_id,
    std::string marker_id,
    std::string source_group_id,
    std::string target_group_id)
{
    SpectralLineStateIntent intent(Kind::CopyMarkerReference);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.source_group_id_ = std::move(source_group_id);
    intent.target_group_id_ = std::move(target_group_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::RemoveMarkerReference(
    std::string view_id,
    std::string marker_id,
    std::string group_id)
{
    SpectralLineStateIntent intent(Kind::RemoveMarkerReference);
    intent.view_id_ = std::move(view_id);
    intent.marker_id_ = std::move(marker_id);
    intent.group_id_ = std::move(group_id);
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::SetMarkerVisibility(std::string marker_id, bool visible)
{
    SpectralLineStateIntent intent(Kind::SetMarkerVisibility);
    intent.marker_id_ = std::move(marker_id);
    intent.enabled_ = visible;
    return intent;
}

SpectralLineStateIntent SpectralLineStateIntent::SetMarkerColor(
    std::string marker_id,
    PlotSeriesColor color)
{
    SpectralLineStateIntent intent(Kind::SetMarkerColor);
    intent.marker_id_ = std::move(marker_id);
    intent.marker_color_ = std::move(color);
    return intent;
}

SpectralLinesPanelController::SpectralLinesPanelController(std::filesystem::path path)
    : SpectralLinesPanelController(std::move(path), std::filesystem::path{}) {}
SpectralLinesPanelController::SpectralLinesPanelController(std::filesystem::path path, std::filesystem::path state)
    : SpectralLinesPanelController(LoadPackagedPublicSpectralLineList(path), std::move(state)) {}
SpectralLinesPanelController::SpectralLinesPanelController(SpectralLineList list, std::filesystem::path state)
    : SpectralLinesPanelController(SpectralLineListParseResult{std::move(list), {}}, std::move(state)) {}
SpectralLinesPanelController::SpectralLinesPanelController(SpectralLineListParseResult parsed, std::filesystem::path state)
    : adapter_(std::move(parsed), std::move(state)),
      cache_persistence_(std::chrono::milliseconds(500), std::chrono::seconds(10))
{
    for (auto id : {kRawSpectrumPlotSeriesId, kGaussianSmoothingPlotSeriesId, kMedianSmoothingPlotSeriesId})
        (void)marker_color_assignments_.SlotFor(id);
    for (const auto& marker : adapter_.effective().markers)
        marker_auto_slots_[marker.id] = marker_color_assignments_.SlotFor(adapter_.effective().id + ".marker." + marker.id);
    cache_persistence_.SetLoadWarning(adapter_.load_error(), adapter_.load_error());
    if (adapter_.requires_save()) cache_persistence_.MarkDirty();
}
SpectralLinesPanelController::~SpectralLinesPanelController() { (void)Flush(); }

const line_list::GroupingView* SpectralLinesPanelController::FindView(std::string_view id) const
{
    for (const auto& view : adapter_.effective().grouping_views) if (view.id == id) return &view;
    return nullptr;
}
const line_list::Marker* SpectralLinesPanelController::FindMarker(std::string_view id) const
{
    for (const auto& marker : adapter_.effective().markers) if (marker.id == id) return &marker;
    return nullptr;
}
bool SpectralLinesPanelController::Visible(std::string_view id) const
{
    const auto& visibility = adapter_.session().marker_visibility;
    auto found = visibility.find(std::string(id));
    return found == visibility.end() || found->second;
}
std::string SpectralLinesPanelController::UnassignedId(const line_list::GroupingView& view) const
{
    // UI-only identity; authored IDs (including the old spelling) are legal.
    std::string id = "__unassigned__";
    while (std::any_of(view.groups.begin(), view.groups.end(), [&](const auto& group) { return group.id == id; })) id += "_";
    return id;
}
std::vector<std::string> SpectralLinesPanelController::GroupMembers(const line_list::GroupingView& view, std::string_view id) const
{
    for (const auto& group : view.groups) if (group.id == id) return group.marker_ids;
    if (id != UnassignedId(view)) return {};
    std::unordered_set<std::string> assigned;
    for (const auto& group : view.groups) assigned.insert(group.marker_ids.begin(), group.marker_ids.end());
    std::vector<std::string> result;
    for (const auto& marker : adapter_.effective().markers) if (!assigned.contains(marker.id)) result.push_back(marker.id);
    return result;
}
std::string SpectralLinesPanelController::NextId(bool group) const
{
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto id = GenerateUuidV4();
        if (!id) return {};
        bool exists = FindView(*id) != nullptr;
        if (group) {
            exists = false;
            for (const auto& view : adapter_.effective().grouping_views)
                for (const auto& value : view.groups) exists |= value.id == *id;
        }
        if (!exists) return *id;
    }
    return {};
}

SpectralLineStateResult SpectralLinesPanelController::Submit(SpectralLineStateIntent intent)
{
    using Kind = SpectralLineStateIntent::Kind;
    auto& session = adapter_.session();
    std::string error;
    if (intent.kind_ == Kind::SetGroupingViewSearch) {
        if (grouping_view_search_ == intent.text_) return NoChange();
        grouping_view_search_ = std::move(intent.text_); return Applied(false);
    }
    if (intent.kind_ == Kind::SetMarkerLabelsVisible) {
        if (marker_labels_visible_ == intent.enabled_) return NoChange();
        marker_labels_visible_ = intent.enabled_; return Applied(false);
    }
    if (intent.kind_ == Kind::SelectGroupingView) {
        if (!FindView(intent.view_id_)) return Rejected("Grouping view does not exist.");
        if (session.active_view_id == intent.view_id_) return NoChange();
        session.active_view_id = intent.view_id_; adapter_.intent().view_selection = true;
        return Applied(true);
    }
    if (intent.kind_ == Kind::AcknowledgeGroupingViewSelection) {
        if (!grouping_view_selection_requested_ || session.active_view_id != intent.view_id_) return NoChange();
        grouping_view_selection_requested_ = false; return Applied(false);
    }
    if (intent.kind_ == Kind::SetMarkerVisibility) {
        if (!FindMarker(intent.marker_id_)) return Rejected("Marker does not exist.");
        if (Visible(intent.marker_id_) == intent.enabled_) return NoChange();
        session.marker_visibility[intent.marker_id_] = intent.enabled_; return Applied(true);
    }
    if (intent.kind_ == Kind::SetMarkerColor) {
        if (!FindMarker(intent.marker_id_)) return Rejected("Marker does not exist.");
        auto color = intent.marker_color_;
        if (color.explicit_color()) {
            if (!IsValidRgbaColor(*color.explicit_color())) return Rejected("Marker color channels must be finite values from zero to one.");
            color = DecodeLineListColor(EncodeLineListColor(*color.explicit_color()));
        }
        if (adapter_.Color(intent.marker_id_) == color) return NoChange();
        if (!adapter_.SetColor(intent.marker_id_, color, error)) return Rejected(error);
        return Applied(true);
    }
    if (intent.kind_ == Kind::CreateUserGroupingView || intent.kind_ == Kind::DuplicateGroupingView) {
        line_list::GroupingView view;
        GeneratedNameMetadata metadata;
        std::unordered_map<std::string, GeneratedNameMetadata> group_names;
        if (intent.kind_ == Kind::DuplicateGroupingView) {
            const auto* source = FindView(intent.view_id_);
            if (!source) return Rejected("Grouping view does not exist.");
            view = *source;
            metadata = NameMetadata(session.view_names, view.id);
            if (adapter_.IsBaseView(view.id) && view.id == "__catalog_grouping_view__") metadata.source = GeneratedNameSource::CatalogGroupingView;
            if (metadata.source == GeneratedNameSource::None && metadata.copy_count == 0) metadata.copy_base_name = view.name;
            if (metadata.copy_count < kMaximumGeneratedNameCopyCount) ++metadata.copy_count; else metadata = {};
            view.name += " copy";
            std::unordered_set<std::string> generated;
            for (auto& group : view.groups) {
                auto old = group.id; group.id = NextId(true);
                if (group.id.empty() || !generated.insert(group.id).second) return Rejected("Could not generate distinct group identities.");
                group_names[group.id] = NameMetadata(session.group_names, old);
            }
        } else {
            std::size_t count = 1;
            for (const auto& candidate : adapter_.effective().grouping_views) if (adapter_.CanEditView(candidate.id)) ++count;
            view.name = "Grouping " + std::to_string(count);
            metadata.source = GeneratedNameSource::DefaultGroupingView; metadata.ordinal = count;
        }
        view.id = NextId(false);
        if (view.id.empty()) return Rejected("Could not generate grouping view identity.");
        const auto id = view.id;
        if (!adapter_.PutView(std::move(view), error)) return Rejected(error);
        session.view_names[id] = metadata;
        for (const auto& [group, name] : group_names) session.group_names[group] = name;
        session.active_view_id = id; adapter_.intent().view_selection = true;
        grouping_view_selection_requested_ = true; return Applied(true);
    }
    const auto* found = FindView(intent.view_id_);
    if (!found) return Rejected("Grouping view does not exist.");
    auto view = *found;
    const auto unassigned = UnassignedId(view);
    const auto find_group = [&](std::string_view id) -> line_list::Group* {
        for (auto& group : view.groups) if (group.id == id) return &group;
        return nullptr;
    };
    const auto group_exists = [&](std::string_view id) { return id == unassigned || find_group(id) != nullptr; };
    if (intent.kind_ == Kind::SetGroupExpanded) {
        if (!group_exists(intent.group_id_)) return Rejected("Group does not exist.");
        const auto key = view.id + "/" + intent.group_id_;
        bool changed = intent.enabled_ ? session.expanded_group_ids.insert(key).second : session.expanded_group_ids.erase(key) != 0;
        return changed ? Applied(true) : NoChange();
    }
    if (intent.kind_ == Kind::SetGroupMarkerVisibility) {
        if (!group_exists(intent.group_id_)) return Rejected("Group does not exist.");
        if (!grouping_view_search_.empty()) return Rejected("Group marker visibility cannot be changed during grouping view search.");
        bool changed = false;
        for (const auto& id : GroupMembers(view, intent.group_id_)) {
            if (Visible(id) != intent.enabled_) { session.marker_visibility[id] = intent.enabled_; changed = true; }
        }
        return changed ? Applied(true) : NoChange();
    }
    if (!adapter_.CanEditView(view.id)) return Rejected("Base grouping views are read-only.");
    if (intent.kind_ == Kind::DeleteUserGroupingView) {
        const bool active = session.active_view_id == view.id;
        if (!adapter_.DeleteView(view.id, error)) return Rejected(error);
        if (active) { adapter_.intent().view_selection = false; grouping_view_selection_requested_ = true; }
        return Applied(true);
    }
    if (intent.kind_ == Kind::RenameUserGroupingView) {
        const auto name = Trim(intent.text_);
        if (name.empty()) return Rejected("A non-empty name is required.");
        if (intent.rename_edit_state_ == SpectralLineRenameEditState::Unedited) return NoChange();
        if (view.name == name && NameMetadata(session.view_names, view.id) == GeneratedNameMetadata{}) return NoChange();
        view.name = name;
        if (!adapter_.PutView(view, error)) return Rejected(error);
        session.view_names.erase(view.id); return Applied(true);
    }
    if (intent.kind_ == Kind::RenameUserGroup) {
        auto* group = find_group(intent.group_id_); const auto name = Trim(intent.text_);
        if (!group || name.empty()) return Rejected("Editable group and non-empty name are required.");
        if (intent.rename_edit_state_ == SpectralLineRenameEditState::Unedited) return NoChange();
        if (group->name == name && NameMetadata(session.group_names, group->id) == GeneratedNameMetadata{}) return NoChange();
        group->name = name;
        if (!adapter_.PutView(view, error)) return Rejected(error);
        session.group_names.erase(intent.group_id_); return Applied(true);
    }
    if (intent.kind_ == Kind::DeleteUserGroup) {
        if (!std::erase_if(view.groups, [&](const auto& group) { return group.id == intent.group_id_; })) return Rejected("Editable group does not exist.");
        if (!adapter_.PutView(view, error)) return Rejected(error);
        session.expanded_group_ids.erase(view.id + "/" + intent.group_id_); return Applied(true);
    }
    if (intent.kind_ == Kind::ReorderUserGroupBefore) {
        if (!find_group(intent.source_group_id_) || !group_exists(intent.target_group_id_) || intent.source_group_id_ == intent.target_group_id_)
            return Rejected("Invalid group reorder.");
        const auto prior = view;
        auto moving = *find_group(intent.source_group_id_);
        std::erase_if(view.groups, [&](const auto& group) { return group.id == intent.source_group_id_; });
        auto target = std::find_if(view.groups.begin(), view.groups.end(), [&](const auto& group) { return group.id == intent.target_group_id_; });
        view.groups.insert(target, std::move(moving));
        if (view == prior) return NoChange();
        if (!adapter_.PutView(view, error)) return Rejected(error);
        adapter_.intent().group_ordering_view_ids.insert(view.id); return Applied(true);
    }
    const bool new_group = intent.kind_ == Kind::AddUserGroup || intent.kind_ == Kind::MoveMarkerReferenceToNewGroup || intent.kind_ == Kind::CopyMarkerReferenceToNewGroup;
    if (new_group) {
        if (intent.kind_ != Kind::AddUserGroup) {
            const auto members = GroupMembers(view, intent.source_group_id_);
            if (!FindMarker(intent.marker_id_) || std::find(members.begin(), members.end(), intent.marker_id_) == members.end())
                return Rejected("Source marker reference does not exist.");
            if (intent.kind_ == Kind::MoveMarkerReferenceToNewGroup && intent.source_group_id_ != unassigned)
                return Rejected("Only an unassigned marker can be moved directly to a new group.");
        }
        const auto id = NextId(true);
        if (id.empty()) return Rejected("Could not generate group identity.");
        std::unordered_set<std::string> names;
        for (const auto& v : adapter_.effective().grouping_views) for (const auto& g : v.groups) names.insert(g.name);
        std::size_t ordinal = 1;
        while (names.contains("Group " + std::to_string(ordinal))) ++ordinal;
        line_list::Group group{id, "Group " + std::to_string(ordinal), {}};
        if (intent.kind_ != Kind::AddUserGroup) group.marker_ids.push_back(intent.marker_id_);
        view.groups.push_back(std::move(group));
        if (!adapter_.PutView(view, error)) return Rejected(error);
        session.group_names[id] = {GeneratedNameSource::DefaultGroup, ordinal, 0, {}};
        if (intent.kind_ != Kind::AddUserGroup) session.expanded_group_ids.insert(view.id + "/" + id);
        return Applied(true);
    }
    if (!FindMarker(intent.marker_id_)) return Rejected("Marker does not exist.");
    const auto prior = view;
    if (intent.kind_ == Kind::RemoveMarkerReference) {
        auto* group = find_group(intent.group_id_);
        if (!group || !std::erase(group->marker_ids, intent.marker_id_)) return Rejected("Editable marker reference does not exist.");
    } else if (intent.kind_ == Kind::MoveMarkerReference || intent.kind_ == Kind::CopyMarkerReference) {
        if (!group_exists(intent.target_group_id_)) return Rejected("Target group does not exist.");
        const auto members = GroupMembers(view, intent.source_group_id_);
        if (std::find(members.begin(), members.end(), intent.marker_id_) == members.end()) return Rejected("Source reference does not exist.");
        if (intent.kind_ == Kind::MoveMarkerReference) {
            if (intent.source_group_id_ == intent.target_group_id_) return NoChange();
            if (auto* source = find_group(intent.source_group_id_)) std::erase(source->marker_ids, intent.marker_id_);
        }
        if (intent.target_group_id_ == unassigned) {
            if (intent.kind_ == Kind::CopyMarkerReference) return Rejected("Cannot copy into derived Unassigned area.");
        } else {
            auto& ids = find_group(intent.target_group_id_)->marker_ids;
            if (std::find(ids.begin(), ids.end(), intent.marker_id_) == ids.end()) ids.push_back(intent.marker_id_);
        }
    } else return Rejected("Unknown spectral-line operation.");
    if (view == prior) return NoChange();
    if (!adapter_.PutView(std::move(view), error)) return Rejected(error);
    return Applied(true);
}

SpectralLinePanelView SpectralLinesPanelController::View() const
{
    SpectralLinePanelView result;
    const auto& list = adapter_.effective(); const auto& session = adapter_.session();
    const auto persistence = cache_persistence_.PersistenceStatus();
    result.line_list_id = list.id; result.line_list_display_name = list.name; result.line_list_load_error = adapter_.base_error();
    result.persistence.retrying = persistence.retrying; result.persistence.recovered = persistence.recovered;
    result.persistence.load_issue = PanelLoadIssue(adapter_.load_issue());
    result.persistence.load_diagnostic_detail = persistence.load_diagnostic_detail;
    result.persistence.save_diagnostic_detail = persistence.save_diagnostic_detail;
    result.grouping_view_search = grouping_view_search_; result.marker_labels_visible = marker_labels_visible_;
    result.line_list_marker_count = list.markers.size();
    for (const auto& view : list.grouping_views) {
        SpectralLineGroupingView output;
        output.id = view.id; output.name = view.name; output.editable = adapter_.CanEditView(view.id);
        result.has_base_grouping_view |= adapter_.IsBaseView(view.id);
        if (output.editable) ++result.user_grouping_view_count;
        output.generated_name = NameMetadata(session.view_names, view.id);
        if (adapter_.IsBaseView(view.id) && view.id == "__catalog_grouping_view__") output.generated_name.source = GeneratedNameSource::CatalogGroupingView;
        output.active = session.active_view_id == view.id;
        output.selection_requested = output.active && grouping_view_selection_requested_;
        output.search_active = !grouping_view_search_.empty();
        std::unordered_map<std::string, int> counts;
        for (const auto& group : view.groups) for (const auto& id : group.marker_ids) ++counts[id];
        const auto append = [&](std::string id, std::string name, const std::vector<std::string>& members, bool unassigned) {
            SpectralLineGroupView group;
            group.id = std::move(id); group.name = std::move(name); group.is_unassigned = unassigned;
            group.generated_name = NameMetadata(session.group_names, group.id);
            group.expanded = session.expanded_group_ids.contains(view.id + "/" + group.id);
            std::size_t visible = 0;
            auto displayed = members;
            std::stable_sort(displayed.begin(), displayed.end(), [&](const auto& left, const auto& right) {
                return SpectralLineMarkerPosition(*FindMarker(left)) < SpectralLineMarkerPosition(*FindMarker(right));
            });
            for (const auto& marker_id : displayed) {
                const auto* marker = FindMarker(marker_id); if (!marker) continue;
                visible += Visible(marker_id) ? 1U : 0U;
                const auto wavelength = WavelengthText(*marker);
                if (!Matches(marker->name + " " + marker->id + " " + marker->note.value_or("") + " " + wavelength, grouping_view_search_)) continue;
                group.marker_references.push_back({marker->id, marker->name, wavelength, marker->note.value_or(""), true,
                    Visible(marker_id), counts[marker_id] > 1, adapter_.Color(marker_id), marker_auto_slots_.at(marker_id)});
            }
            group.visibility = output.search_active ? GroupVisibilityState::SearchFiltered : members.empty() ? GroupVisibilityState::Empty :
                visible == 0 ? GroupVisibilityState::AllHidden : visible == members.size() ? GroupVisibilityState::AllVisible : GroupVisibilityState::Mixed;
            group.dimmed_by_search = output.search_active && group.marker_references.empty();
            output.groups.push_back(std::move(group));
        };
        for (const auto& group : view.groups) append(group.id, group.name, group.marker_ids, false);
        const auto unassigned = UnassignedId(view);
        append(unassigned, "Unassigned", GroupMembers(view, unassigned), true);
        result.grouping_views.push_back(std::move(output));
    }
    return result;
}
SpectralLinePlotView SpectralLinesPanelController::PlotView(const SpectrumSnapshotHandle& snapshot) const
{
    SpectralLinePlotView result;
    result.marker_labels_visible = marker_labels_visible_; result.layout_scope_id = adapter_.effective().id;
    if (!snapshot || !snapshot->capabilities.can_show_spectral_lines) return result;
    for (const auto& marker : adapter_.effective().markers) if (Visible(marker.id))
        result.visible_markers.push_back({&marker, adapter_.Color(marker.id), marker_auto_slots_.at(marker.id)});
    return result;
}
LocalUserStatePersistenceLifecycle::SaveResult SpectralLinesPanelController::SaveState()
{
    std::string error;
    const bool saved = adapter_.Save(error);
    if (saved) grouping_view_selection_requested_ = true;
    return {.saved = saved, .error = std::move(error)};
}
LocalUserStatePersistenceStatus SpectralLinesPanelController::PersistenceStatus() const { return cache_persistence_.PersistenceStatus(); }
void SpectralLinesPanelController::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{ (void)cache_persistence_.RunMaintenance(now, [this] { return SaveState(); }); }
std::optional<LocalUserStateSaveScheduler::TimePoint> SpectralLinesPanelController::NextMaintenanceDeadline() const
{ return cache_persistence_.NextMaintenanceDeadline(); }
bool SpectralLinesPanelController::Flush()
{
    return cache_persistence_.Flush([this] { return SaveState(); }) != LocalUserStatePersistenceLifecycle::FlushOutcome::Failed &&
        adapter_.load_issue() == VersionedJsonCacheLoadIssueKind::None;
}
SpectralLineStateResult SpectralLinesPanelController::Applied(bool persistent)
{
    if (persistent) { adapter_.NormalizeSession(); cache_persistence_.MarkDirty(); }
    return {SpectralLineStateResultStatus::Applied, true, persistent, {}};
}
SpectralLineStateResult SpectralLinesPanelController::NoChange() { return {}; }
SpectralLineStateResult SpectralLinesPanelController::Rejected(std::string message)
{ return {SpectralLineStateResultStatus::Rejected, false, false, std::move(message)}; }

} // namespace spectiary
