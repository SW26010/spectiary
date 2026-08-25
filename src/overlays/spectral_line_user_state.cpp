#include "overlays/spectral_line_user_state.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kPublicCatalogIdentity = "specforge.public";
constexpr const char* kPublicCatalogDisplayName = "Public catalog";
constexpr const char* kCatalogGroupingViewId = "__catalog_grouping_view__";
constexpr const char* kUnassignedUserGroupId = "__unassigned__";

std::uint64_t NextSequenceAfterGeneratedId(
    std::string_view id,
    std::string_view prefix)
{
    if (!id.starts_with(prefix) || id.size() == prefix.size()) {
        return 0;
    }

    const std::string_view digits = id.substr(prefix.size());
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(
        digits.data(),
        digits.data() + digits.size(),
        value);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
        return 0;
    }
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        return value;
    }
    return value + 1;
}

std::optional<std::uint64_t> ParseGeneratedIdentitySequence(
    std::string_view id,
    std::string_view prefix)
{
    if (!id.starts_with(prefix) || id.size() == prefix.size()) {
        return std::nullopt;
    }
    const std::string_view digits = id.substr(prefix.size());
    for (const char digit : digits) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
    }
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(
        digits.data(),
        digits.data() + digits.size(),
        value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

void RaiseSequenceHighWaterMark(
    std::uint64_t& sequence,
    std::uint64_t required)
{
    if (sequence == 0) {
        sequence = 1;
    }
    if (required > sequence) {
        sequence = required;
    }
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool ContainsCaseInsensitive(std::string_view text, std::string_view pattern)
{
    if (pattern.empty()) {
        return true;
    }
    return LowerAscii(std::string(text)).find(LowerAscii(std::string(pattern))) != std::string::npos;
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

std::string SanitizeId(std::string_view value)
{
    std::string id;
    id.reserve(value.size());
    for (const char character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (std::isalnum(unsigned_character)) {
            id.push_back(static_cast<char>(std::tolower(unsigned_character)));
        } else if (!id.empty() && id.back() != '_') {
            id.push_back('_');
        }
    }
    while (!id.empty() && id.back() == '_') {
        id.pop_back();
    }
    return id.empty() ? "group" : id;
}

std::string UniqueCatalogGroupId(std::string_view group_name, std::unordered_set<std::string>& used_group_ids)
{
    const std::string base_id = "catalog_" + SanitizeId(group_name);
    std::string candidate = base_id;
    int suffix = 2;
    while (!used_group_ids.insert(candidate).second) {
        candidate = base_id + "_" + std::to_string(suffix++);
    }
    return candidate;
}

MarkerReference MakeReference(const CatalogIdentity& identity, std::string marker_id)
{
    MarkerReference reference;
    reference.catalog_identity = identity;
    reference.marker_id = std::move(marker_id);
    return reference;
}

bool SameReference(const MarkerReference& reference, const CatalogIdentity& identity, std::string_view marker_id)
{
    return SameCatalogIdentity(reference.catalog_identity, identity) && reference.marker_id == marker_id;
}

bool ContainsReference(
    const std::vector<MarkerReference>& references,
    const CatalogIdentity& identity,
    std::string_view marker_id)
{
    return std::any_of(references.begin(), references.end(), [&identity, marker_id](const auto& reference) {
        return SameReference(reference, identity, marker_id);
    });
}

std::vector<MarkerReference>::iterator FindReference(
    std::vector<MarkerReference>& references,
    const CatalogIdentity& identity,
    std::string_view marker_id)
{
    return std::find_if(references.begin(), references.end(), [&identity, marker_id](const auto& reference) {
        return SameReference(reference, identity, marker_id);
    });
}

UserGroup* FindGroup(GroupingView& view, std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [group_id](const auto& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

const UserGroup* FindGroup(const GroupingView& view, std::string_view group_id)
{
    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [group_id](const auto& group) {
        return group.id == group_id;
    });
    return match == view.groups.end() ? nullptr : &(*match);
}

bool IsUnassignedGroup(const UserGroup& group)
{
    return group.is_unassigned || group.id == kUnassignedUserGroupId;
}

bool SameIdentityValue(const CatalogIdentity& left, const CatalogIdentity& right)
{
    return left.id == right.id && left.display_name == right.display_name;
}

bool SameReferenceValue(const MarkerReference& left, const MarkerReference& right)
{
    return SameIdentityValue(left.catalog_identity, right.catalog_identity) &&
           left.marker_id == right.marker_id;
}

bool SameGroupValue(const UserGroup& left, const UserGroup& right)
{
    return left.id == right.id && left.name == right.name &&
           left.generated_name == right.generated_name &&
           left.is_unassigned == right.is_unassigned &&
           left.marker_references.size() == right.marker_references.size() &&
           std::equal(
               left.marker_references.begin(),
               left.marker_references.end(),
               right.marker_references.begin(),
               SameReferenceValue);
}

bool SameGroupingViewValue(const GroupingView& left, const GroupingView& right)
{
    return left.id == right.id && left.name == right.name &&
           left.generated_name == right.generated_name &&
           left.read_only == right.read_only &&
           left.groups.size() == right.groups.size() &&
           std::equal(
               left.groups.begin(),
               left.groups.end(),
               right.groups.begin(),
               SameGroupValue);
}

UserGroup& EnsureUnassignedGroup(GroupingView& view)
{
    for (UserGroup& group : view.groups) {
        if (group.is_unassigned || group.id == kUnassignedUserGroupId) {
            group.id = kUnassignedUserGroupId;
            group.name = "Unassigned";
            group.generated_name = {};
            group.is_unassigned = true;
            return group;
        }
    }

    UserGroup group;
    group.id = kUnassignedUserGroupId;
    group.name = "Unassigned";
    group.is_unassigned = true;
    view.groups.push_back(std::move(group));
    return view.groups.back();
}

GeneratedNameMetadata CanonicalGroupingViewGeneratedName(
    const GeneratedNameMetadata& source)
{
    GeneratedNameMetadata result = source;
    if (result.copy_count >
        kMaximumGeneratedNameCopyCount) {
        return {};
    }
    if (result.source == GeneratedNameSource::DefaultGroup ||
        (result.source == GeneratedNameSource::DefaultGroupingView &&
         result.ordinal == 0)) {
        return {};
    }
    if (result.source == GeneratedNameSource::None) {
        result.ordinal = 0;
        if (result.copy_count == 0) {
            result.copy_base_name.clear();
        } else if (result.copy_base_name.empty()) {
            return {};
        }
    } else {
        result.copy_base_name.clear();
    }
    return result;
}

GeneratedNameMetadata CanonicalUserGroupGeneratedName(
    const GeneratedNameMetadata& source)
{
    if (source.source != GeneratedNameSource::DefaultGroup ||
        source.ordinal == 0 ||
        source.copy_count != 0 ||
        !source.copy_base_name.empty()) {
        return {};
    }
    return source;
}

GeneratedNameMetadata DuplicateGeneratedName(
    std::string_view source_name,
    const GeneratedNameMetadata& source)
{
    GeneratedNameMetadata result =
        CanonicalGroupingViewGeneratedName(source);
    if (result.source == GeneratedNameSource::None &&
        result.copy_count == 0) {
        result.copy_base_name = source_name;
    }
    if (result.copy_count >=
        kMaximumGeneratedNameCopyCount) {
        return {};
    }
    ++result.copy_count;
    return result;
}

bool ContainsReferenceInOrdinaryGroups(const GroupingView& view, const MarkerReference& target)
{
    return std::any_of(view.groups.begin(), view.groups.end(), [&target](const UserGroup& group) {
        if (IsUnassignedGroup(group)) {
            return false;
        }
        return ContainsReference(group.marker_references, target.catalog_identity, target.marker_id);
    });
}

void AddReferenceToUnassignedIfAbsent(GroupingView& view, const MarkerReference& reference)
{
    UserGroup& unassigned = EnsureUnassignedGroup(view);
    if (!ContainsReference(unassigned.marker_references, reference.catalog_identity, reference.marker_id)) {
        unassigned.marker_references.push_back(reference);
    }
}

void PreserveReferenceIfNoOrdinaryGroup(GroupingView& view, const MarkerReference& reference)
{
    if (!ContainsReferenceInOrdinaryGroups(view, reference)) {
        AddReferenceToUnassignedIfAbsent(view, reference);
    }
}

void SortReferencesByCatalogPosition(
    std::vector<MarkerReference>& references,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity)
{
    std::stable_sort(references.begin(), references.end(), [&catalog, &identity](const auto& left, const auto& right) {
        const SpectralLineMarker* left_marker = FindCatalogMarker(catalog, identity, left);
        const SpectralLineMarker* right_marker = FindCatalogMarker(catalog, identity, right);
        if (left_marker != nullptr && right_marker != nullptr) {
            return SpectralLineMarkerPosition(*left_marker) < SpectralLineMarkerPosition(*right_marker);
        }
        if (left_marker != nullptr) {
            return true;
        }
        if (right_marker != nullptr) {
            return false;
        }
        return left.marker_id < right.marker_id;
    });
}

}  // namespace

const CatalogIdentity& PublicSpectralLineCatalogIdentity()
{
    static const CatalogIdentity identity{kPublicCatalogIdentity, kPublicCatalogDisplayName};
    return identity;
}

const char* CatalogGroupingViewId()
{
    return kCatalogGroupingViewId;
}

const char* UnassignedUserGroupId()
{
    return kUnassignedUserGroupId;
}

bool SameCatalogIdentity(const CatalogIdentity& left, const CatalogIdentity& right)
{
    return !left.id.empty() && left.id == right.id;
}

CatalogUserState MakeCatalogUserState(const CatalogIdentity& identity)
{
    CatalogUserState state;
    state.catalog_identity = identity;
    state.active_view_id = kCatalogGroupingViewId;
    return state;
}

CatalogUserState& EnsureCatalogUserState(CatalogUserStateCache& cache, const CatalogIdentity& identity)
{
    auto [iterator, inserted] = cache.catalogs.emplace(identity.id, MakeCatalogUserState(identity));
    if (!inserted && iterator->second.catalog_identity.id.empty()) {
        iterator->second.catalog_identity = identity;
    }
    return iterator->second;
}

CatalogPanelState& EnsureCatalogPanelState(CatalogUserStateCache& cache, const CatalogIdentity& identity)
{
    return cache.catalog_panel_state[identity.id];
}

bool ValidateCatalogUserStateCacheForLegacyMigration(
    const CatalogUserStateCache& cache,
    std::string& diagnostic,
    int schema_version)
{
    diagnostic.clear();
    const auto fail = [&](std::string message) {
        diagnostic =
            "legacy catalog user-state identity violation: " +
            std::move(message);
        return false;
    };

    for (const auto& [catalog_id, state] : cache.catalogs) {
        if (catalog_id.empty() || state.catalog_identity.id.empty()) {
            return fail("catalog identity must not be empty");
        }
        if (state.catalog_identity.id != catalog_id) {
            return fail(
                "catalog identity " + state.catalog_identity.id +
                " does not match cache key " + catalog_id);
        }

        std::unordered_set<std::string> view_ids;
        std::unordered_set<std::string> global_group_ids;
        for (const GroupingView& view : state.grouping_views) {
            if (view.id.empty()) {
                return fail("grouping view identity must not be empty");
            }
            if (!view_ids.insert(view.id).second) {
                return fail("duplicate grouping view identity " + view.id);
            }

            std::unordered_set<std::string> view_group_ids;
            for (const UserGroup& group : view.groups) {
                if (group.id.empty()) {
                    return fail(
                        "group identity must not be empty in view " +
                        view.id);
                }
                if (!view_group_ids.insert(group.id).second) {
                    return fail(
                        "duplicate group identity " + group.id +
                        " in view " + view.id);
                }
                if (group.id != UnassignedUserGroupId() &&
                    !global_group_ids.insert(group.id).second) {
                    return fail(
                        "group identity " + group.id +
                        " is reused across grouping views");
                }

                std::unordered_set<std::string> reference_ids;
                for (const MarkerReference& reference : group.marker_references) {
                    if (reference.marker_id.empty() ||
                        reference.catalog_identity.id.empty()) {
                        return fail(
                            "marker reference identity must not be empty in group " +
                            group.id);
                    }
                    if (reference.catalog_identity.id != catalog_id) {
                        return fail(
                            "marker reference catalog identity " +
                            reference.catalog_identity.id +
                            " does not match catalog " + catalog_id);
                    }
                    if (!reference_ids.insert(reference.marker_id).second) {
                        return fail(
                            "duplicate marker reference identity " +
                            reference.marker_id + " in group " + group.id);
                    }
                }
                if (schema_version >= 3 &&
                    group.is_unassigned !=
                        (group.id == UnassignedUserGroupId())) {
                    return fail(
                        "unassigned group identity/flag mismatch in view " +
                        view.id + " for group " + group.id);
                }
            }
        }

        for (const std::string& id : state.reserved_view_ids) {
            if (id.empty() || id == CatalogGroupingViewId()) {
                return fail("reserved view identity must be a user identity");
            }
        }
        for (const std::string& id : state.reserved_group_ids) {
            if (id.empty() || id == UnassignedUserGroupId()) {
                return fail(
                    "reserved group identity must be an ordinary identity");
            }
        }
    }
    return true;
}

bool ValidateCatalogUserStateCacheForReconciliation(
    const CatalogUserStateCache& cache,
    std::string& diagnostic,
    bool require_allocator_history)
{
    diagnostic.clear();
    const auto fail = [&](std::string message) {
        diagnostic =
            "latest catalog user-state semantic identity violation: " +
            std::move(message);
        return false;
    };

    for (const auto& [catalog_id, state] : cache.catalogs) {
        if (catalog_id.empty() || state.catalog_identity.id.empty()) {
            return fail("catalog identity must not be empty");
        }
        if (state.catalog_identity.id != catalog_id) {
            return fail(
                "catalog identity " + state.catalog_identity.id +
                " does not match cache key " + catalog_id);
        }

        std::unordered_set<std::string> view_ids;
        std::unordered_set<std::string> global_group_ids;
        for (const GroupingView& view : state.grouping_views) {
            if (view.id.empty()) {
                return fail("grouping view identity must not be empty");
            }
            if (view.id == CatalogGroupingViewId()) {
                return fail(
                    "user grouping views must not use the catalog grouping identity");
            }
            if (!view_ids.insert(view.id).second) {
                return fail("duplicate grouping view identity " + view.id);
            }

            std::size_t unassigned_count = 0;
            std::unordered_set<std::string> view_group_ids;
            for (const UserGroup& group : view.groups) {
                if (group.id.empty()) {
                    return fail(
                        "group identity must not be empty in view " + view.id);
                }
                const bool is_unassigned_identity =
                    group.id == UnassignedUserGroupId();
                if (group.is_unassigned != is_unassigned_identity) {
                    return fail(
                        "unassigned group identity/flag mismatch in view " +
                        view.id);
                }
                if (is_unassigned_identity) {
                    ++unassigned_count;
                } else {
                    if (!view_group_ids.insert(group.id).second) {
                        return fail(
                            "duplicate group identity " + group.id +
                            " in view " + view.id);
                    }
                    if (!global_group_ids.insert(group.id).second) {
                        return fail(
                            "group identity " + group.id +
                            " is reused across grouping views");
                    }
                }

                std::unordered_set<std::string> reference_ids;
                for (const MarkerReference& reference : group.marker_references) {
                    if (reference.marker_id.empty() ||
                        reference.catalog_identity.id.empty()) {
                        return fail(
                            "marker reference identity must not be empty in group " +
                            group.id);
                    }
                    if (!SameIdentityValue(
                            reference.catalog_identity,
                            state.catalog_identity)) {
                        return fail(
                            "marker reference catalog identity does not match " +
                            state.catalog_identity.id);
                    }
                    if (!reference_ids.insert(reference.marker_id).second) {
                        return fail(
                            "duplicate marker reference identity " +
                            reference.marker_id + " in group " + group.id);
                    }
                }
            }
            if (unassigned_count != 1) {
                return fail(
                    "view " + view.id +
                    " must contain exactly one unassigned group");
            }
        }

        if (!state.active_view_id.empty() &&
            state.active_view_id != CatalogGroupingViewId() &&
            !view_ids.contains(state.active_view_id)) {
            return fail(
                "active view identity " + state.active_view_id +
                " does not belong to the catalog");
        }
        for (const auto& [marker_id, visible] : state.marker_visibility) {
            (void)visible;
            if (marker_id.empty()) {
                return fail("marker visibility identity must not be empty");
            }
        }
        for (const auto& [marker_id, color] : state.marker_colors) {
            if (marker_id.empty()) {
                return fail("marker color identity must not be empty");
            }
            if (color.mode() != PlotSeriesColorMode::ExplicitColor ||
                !color.explicit_color()) {
                return fail("marker color overrides must be explicit colors");
            }
            if (!IsValidRgbaColor(*color.explicit_color())) {
                return fail("marker color channels must be finite values from zero to one");
            }
        }
        for (const std::string& id : state.reserved_view_ids) {
            if (id.empty() || id == CatalogGroupingViewId()) {
                return fail("reserved view identity must be a user identity");
            }
        }
        for (const std::string& id : state.reserved_group_ids) {
            if (id.empty() || id == UnassignedUserGroupId()) {
                return fail(
                    "reserved group identity must be an ordinary identity");
            }
        }

        if (!require_allocator_history) {
            continue;
        }
        if (state.next_view_sequence == 0 ||
            state.next_group_sequence == 0) {
            return fail(
                "allocator history high-water marks must be greater than zero");
        }

        const auto validate_generated_identity =
            [&](std::string_view id,
                std::string_view prefix,
                std::uint64_t next_sequence,
                std::string_view kind) -> bool {
            const std::optional<std::uint64_t> sequence =
                ParseGeneratedIdentitySequence(id, prefix);
            if (!sequence) {
                return true;
            }
            if (*sequence == 0 || *sequence >= next_sequence) {
                return fail(
                    std::string(kind) + " allocator history does not cover " +
                    std::string(id) + " before next sequence " +
                    std::to_string(next_sequence));
            }
            return true;
        };

        for (const GroupingView& view : state.grouping_views) {
            if (view.id != CatalogGroupingViewId() &&
                !state.reserved_view_ids.contains(view.id)) {
                return fail(
                    "view identity " + view.id +
                    " is not covered by reserved allocator history");
            }
            if (!validate_generated_identity(
                    view.id,
                    "view-",
                    state.next_view_sequence,
                    "view")) {
                return false;
            }
            for (const UserGroup& group : view.groups) {
                if (group.id == UnassignedUserGroupId()) {
                    continue;
                }
                if (!state.reserved_group_ids.contains(group.id)) {
                    return fail(
                        "group identity " + group.id +
                        " is not covered by reserved allocator history");
                }
                if (!validate_generated_identity(
                        group.id,
                        "group-",
                        state.next_group_sequence,
                        "group")) {
                    return false;
                }
            }
        }
        for (const std::string& id : state.reserved_view_ids) {
            if (!validate_generated_identity(
                    id,
                    "view-",
                    state.next_view_sequence,
                    "view")) {
                return false;
            }
        }
        for (const std::string& id : state.reserved_group_ids) {
            if (!validate_generated_identity(
                    id,
                    "group-",
                    state.next_group_sequence,
                    "group")) {
                return false;
            }
        }
    }
    return true;
}

const SpectralLineMarker* FindCatalogMarker(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    const MarkerReference& reference)
{
    if (!SameCatalogIdentity(reference.catalog_identity, identity)) {
        return nullptr;
    }
    for (const SpectralLineMarker& marker : catalog.markers) {
        if (marker.id == reference.marker_id) {
            return &marker;
        }
    }
    return nullptr;
}

bool CatalogHasGrouping(const SpectralLineCatalog& catalog)
{
    return std::any_of(catalog.markers.begin(), catalog.markers.end(), [](const auto& marker) {
        return !marker.group.empty();
    });
}

std::optional<GroupingView> BuildCatalogGroupingView(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity)
{
    if (!CatalogHasGrouping(catalog)) {
        return std::nullopt;
    }

    std::map<std::string, std::vector<MarkerReference>> grouped_references;
    for (const SpectralLineMarker& marker : catalog.markers) {
        if (marker.group.empty()) {
            continue;
        }
        grouped_references[marker.group].push_back(MakeReference(identity, marker.id));
    }

    GroupingView view;
    view.id = kCatalogGroupingViewId;
    view.name = "Catalog grouping view";
    view.generated_name.source =
        GeneratedNameSource::CatalogGroupingView;
    view.read_only = true;
    std::unordered_set<std::string> group_ids;
    for (auto& [group_name, references] : grouped_references) {
        UserGroup group;
        group.id = UniqueCatalogGroupId(group_name, group_ids);
        group.name = group_name;
        group.marker_references = std::move(references);
        view.groups.push_back(std::move(group));
    }
    return view;
}

GroupingView CreateUserGroupingViewFromCatalog(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    std::string id,
    std::string name,
    GeneratedNameMetadata generated_name)
{
    GroupingView view;
    view.id = std::move(id);
    view.name = std::move(name);
    view.generated_name =
        CanonicalGroupingViewGeneratedName(generated_name);
    view.read_only = false;

    UserGroup unassigned;
    unassigned.id = kUnassignedUserGroupId;
    unassigned.name = "Unassigned";
    unassigned.is_unassigned = true;
    unassigned.marker_references.reserve(catalog.markers.size());
    for (const SpectralLineMarker& marker : catalog.markers) {
        unassigned.marker_references.push_back(MakeReference(identity, marker.id));
    }
    view.groups.push_back(std::move(unassigned));
    return view;
}

GroupingView DuplicateGroupingView(
    const GroupingView& source,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    std::string id,
    std::string name)
{
    GroupingView view = source;
    view.id = std::move(id);
    view.name = std::move(name);
    view.generated_name =
        DuplicateGeneratedName(
            source.name,
            source.generated_name);
    view.read_only = false;
    return EffectiveUserGroupingView(view, catalog, identity);
}

GroupingView EffectiveUserGroupingView(
    const GroupingView& source,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity)
{
    GroupingView view = source;
    view.read_only = false;
    UserGroup unassigned = EnsureUnassignedGroup(view);

    std::unordered_set<std::string> ordinary_marker_ids;
    for (const UserGroup& group : view.groups) {
        if (group.is_unassigned) {
            continue;
        }
        for (const MarkerReference& reference : group.marker_references) {
            if (SameCatalogIdentity(reference.catalog_identity, identity)) {
                ordinary_marker_ids.insert(reference.marker_id);
            }
        }
    }

    std::vector<MarkerReference> unassigned_references;
    std::unordered_set<std::string> unassigned_marker_ids;
    for (const MarkerReference& reference : unassigned.marker_references) {
        if (!SameCatalogIdentity(reference.catalog_identity, identity)) {
            continue;
        }
        if (ordinary_marker_ids.find(reference.marker_id) != ordinary_marker_ids.end()) {
            continue;
        }
        if (unassigned_marker_ids.insert(reference.marker_id).second) {
            unassigned_references.push_back(reference);
        }
    }
    for (const SpectralLineMarker& marker : catalog.markers) {
        if (ordinary_marker_ids.find(marker.id) == ordinary_marker_ids.end() &&
            unassigned_marker_ids.insert(marker.id).second) {
            unassigned_references.push_back(MakeReference(identity, marker.id));
        }
    }
    SortReferencesByCatalogPosition(unassigned_references, catalog, identity);

    for (UserGroup& group : view.groups) {
        if (group.is_unassigned) {
            group.id = kUnassignedUserGroupId;
            group.name = "Unassigned";
            group.marker_references = unassigned_references;
        } else {
            SortReferencesByCatalogPosition(group.marker_references, catalog, identity);
        }
    }
    return view;
}

CatalogUserStateCanonicalizationResult CanonicalizeCatalogUserState(
    CatalogUserState& state,
    CatalogPanelState& panel_state,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    const std::optional<GroupingView>& catalog_grouping_view)
{
    CatalogUserStateCanonicalizationResult result;
    if (!SameIdentityValue(state.catalog_identity, identity)) {
        state.catalog_identity = identity;
        result.changed = true;
    }

    std::vector<GroupingView> normalized_views;
    normalized_views.reserve(state.grouping_views.size());
    std::unordered_set<std::string> view_ids;
    for (const GroupingView& source_view : state.grouping_views) {
        if (source_view.id.empty() ||
            source_view.id == CatalogGroupingViewId() ||
            !view_ids.insert(source_view.id).second) {
            result.changed = true;
            continue;
        }

        GroupingView normalized_view;
        normalized_view.id = source_view.id;
        normalized_view.name = TrimWhitespace(source_view.name);
        normalized_view.generated_name =
            CanonicalGroupingViewGeneratedName(
                source_view.generated_name);
        if (normalized_view.name.empty()) {
            const std::size_t ordinal =
                normalized_views.size() + 1;
            normalized_view.name =
                "Grouping " + std::to_string(ordinal);
            normalized_view.generated_name.source =
                GeneratedNameSource::DefaultGroupingView;
            normalized_view.generated_name.ordinal =
                ordinal;
        }
        normalized_view.read_only = false;

        std::unordered_set<std::string> group_ids;
        std::unordered_set<std::string> unassigned_marker_ids;
        UserGroup unassigned_group;
        unassigned_group.id = UnassignedUserGroupId();
        unassigned_group.name = "Unassigned";
        unassigned_group.is_unassigned = true;

        for (const UserGroup& source_group : source_view.groups) {
            const bool is_unassigned =
                source_group.is_unassigned ||
                source_group.id == UnassignedUserGroupId();
            if (is_unassigned) {
                for (const MarkerReference& reference :
                     source_group.marker_references) {
                    if (SameCatalogIdentity(
                            reference.catalog_identity,
                            identity) &&
                        !reference.marker_id.empty() &&
                        unassigned_marker_ids.insert(reference.marker_id)
                            .second) {
                        unassigned_group.marker_references.push_back(
                            MarkerReference{identity, reference.marker_id});
                    }
                }
                continue;
            }
            if (source_group.id.empty() ||
                !group_ids.insert(source_group.id).second) {
                result.changed = true;
                continue;
            }

            UserGroup group;
            group.id = source_group.id;
            group.name = TrimWhitespace(source_group.name);
            group.generated_name =
                CanonicalUserGroupGeneratedName(
                    source_group.generated_name);
            if (group.name.empty()) {
                const std::size_t ordinal =
                    normalized_view.groups.size() + 1;
                group.name =
                    "Group " +
                    std::to_string(ordinal);
                group.generated_name.source =
                    GeneratedNameSource::DefaultGroup;
                group.generated_name.ordinal =
                    ordinal;
            }
            std::unordered_set<std::string> marker_ids;
            for (const MarkerReference& reference :
                 source_group.marker_references) {
                if (SameCatalogIdentity(
                        reference.catalog_identity,
                        identity) &&
                    !reference.marker_id.empty() &&
                    marker_ids.insert(reference.marker_id).second) {
                    group.marker_references.push_back(
                        MarkerReference{identity, reference.marker_id});
                }
            }
            normalized_view.groups.push_back(std::move(group));
        }
        normalized_view.groups.push_back(std::move(unassigned_group));
        normalized_view =
            EffectiveUserGroupingView(normalized_view, catalog, identity);
        if (!SameGroupingViewValue(source_view, normalized_view)) {
            result.changed = true;
        }
        normalized_views.push_back(std::move(normalized_view));
    }
    state.grouping_views = std::move(normalized_views);

    std::unordered_set<std::string> normalized_reserved_view_ids;
    for (const std::string& id : state.reserved_view_ids) {
        if (!id.empty() && id != CatalogGroupingViewId()) {
            normalized_reserved_view_ids.insert(id);
        }
    }
    for (const GroupingView& view : state.grouping_views) {
        normalized_reserved_view_ids.insert(view.id);
    }
    if (normalized_reserved_view_ids != state.reserved_view_ids) {
        state.reserved_view_ids = std::move(normalized_reserved_view_ids);
        result.changed = true;
    }

    std::unordered_set<std::string> normalized_reserved_group_ids;
    for (const std::string& id : state.reserved_group_ids) {
        if (!id.empty() && id != UnassignedUserGroupId()) {
            normalized_reserved_group_ids.insert(id);
        }
    }
    for (const GroupingView& view : state.grouping_views) {
        for (const UserGroup& group : view.groups) {
            if (!group.is_unassigned && group.id != UnassignedUserGroupId()) {
                normalized_reserved_group_ids.insert(group.id);
            }
        }
    }
    if (normalized_reserved_group_ids != state.reserved_group_ids) {
        state.reserved_group_ids = std::move(normalized_reserved_group_ids);
        result.changed = true;
    }

    std::uint64_t required_next_view_sequence = 1;
    std::uint64_t required_next_group_sequence = 1;
    for (const GroupingView& view : state.grouping_views) {
        RaiseSequenceHighWaterMark(
            required_next_view_sequence,
            NextSequenceAfterGeneratedId(view.id, "view-"));
        for (const UserGroup& group : view.groups) {
            RaiseSequenceHighWaterMark(
                required_next_group_sequence,
                NextSequenceAfterGeneratedId(group.id, "group-"));
        }
    }
    for (const std::string& id : state.reserved_view_ids) {
        RaiseSequenceHighWaterMark(
            required_next_view_sequence,
            NextSequenceAfterGeneratedId(id, "view-"));
    }
    for (const std::string& id : state.reserved_group_ids) {
        RaiseSequenceHighWaterMark(
            required_next_group_sequence,
            NextSequenceAfterGeneratedId(id, "group-"));
    }
    const std::uint64_t previous_next_view_sequence =
        state.next_view_sequence;
    const std::uint64_t previous_next_group_sequence =
        state.next_group_sequence;
    RaiseSequenceHighWaterMark(
        state.next_view_sequence,
        required_next_view_sequence);
    RaiseSequenceHighWaterMark(
        state.next_group_sequence,
        required_next_group_sequence);
    result.changed = result.changed ||
                     state.next_view_sequence != previous_next_view_sequence ||
                     state.next_group_sequence != previous_next_group_sequence;

    for (auto iterator = state.marker_visibility.begin();
         iterator != state.marker_visibility.end();) {
        if (iterator->first.empty()) {
            iterator = state.marker_visibility.erase(iterator);
            result.changed = true;
        } else {
            ++iterator;
        }
    }

    for (auto iterator = state.marker_colors.begin();
         iterator != state.marker_colors.end();) {
        if (iterator->first.empty() ||
            iterator->second.mode() == PlotSeriesColorMode::Auto) {
            iterator = state.marker_colors.erase(iterator);
            result.changed = true;
        } else {
            ++iterator;
        }
    }

    std::unordered_set<std::string> valid_expansion_keys;
    if (catalog_grouping_view) {
        for (const UserGroup& group : catalog_grouping_view->groups) {
            valid_expansion_keys.insert(
                GroupExpansionKey(catalog_grouping_view->id, group.id));
        }
    }
    for (const GroupingView& view : state.grouping_views) {
        for (const UserGroup& group : view.groups) {
            valid_expansion_keys.insert(
                GroupExpansionKey(view.id, group.id));
        }
    }
    for (auto iterator = panel_state.expanded_group_ids.begin();
         iterator != panel_state.expanded_group_ids.end();) {
        if (!valid_expansion_keys.contains(*iterator)) {
            iterator = panel_state.expanded_group_ids.erase(iterator);
            result.changed = true;
        } else {
            ++iterator;
        }
    }

    const bool active_view_exists =
        (!state.active_view_id.empty() && catalog_grouping_view &&
         catalog_grouping_view->id == state.active_view_id) ||
        std::any_of(
            state.grouping_views.begin(),
            state.grouping_views.end(),
            [&](const GroupingView& view) {
                return view.id == state.active_view_id;
            });
    if (!active_view_exists) {
        const std::string previous_active_view_id = state.active_view_id;
        if (catalog_grouping_view) {
            state.active_view_id = catalog_grouping_view->id;
        } else if (!state.grouping_views.empty()) {
            state.active_view_id = state.grouping_views.front().id;
        } else {
            state.active_view_id.clear();
        }
        result.active_view_changed =
            state.active_view_id != previous_active_view_id;
        result.changed = result.changed || result.active_view_changed;
    }

    return result;
}

bool IsMarkerVisible(const CatalogUserState& state, const std::string& marker_id)
{
    const auto match = state.marker_visibility.find(marker_id);
    return match == state.marker_visibility.end() ? true : match->second;
}

void SetMarkerVisible(CatalogUserState& state, const std::string& marker_id, bool visible)
{
    if (!marker_id.empty()) {
        state.marker_visibility[marker_id] = visible;
    }
}

PlotSeriesColor MarkerColor(
    const CatalogUserState& state,
    std::string_view marker_id)
{
    const auto match = state.marker_colors.find(std::string(marker_id));
    return match == state.marker_colors.end()
        ? PlotSeriesColor::Auto()
        : match->second;
}

void SetMarkerColor(
    CatalogUserState& state,
    std::string marker_id,
    PlotSeriesColor color)
{
    if (marker_id.empty()) {
        return;
    }
    if (color.mode() == PlotSeriesColorMode::Auto) {
        state.marker_colors.erase(marker_id);
        return;
    }
    state.marker_colors.insert_or_assign(
        std::move(marker_id),
        std::move(color));
}

GroupVisibilityState VisibilityStateForGroup(
    const CatalogUserState& state,
    const UserGroup& group,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    bool search_active)
{
    if (search_active) {
        return GroupVisibilityState::SearchFiltered;
    }

    bool has_resolved = false;
    bool any_visible = false;
    bool any_hidden = false;
    for (const MarkerReference& reference : group.marker_references) {
        if (FindCatalogMarker(catalog, identity, reference) == nullptr) {
            continue;
        }
        has_resolved = true;
        if (IsMarkerVisible(state, reference.marker_id)) {
            any_visible = true;
        } else {
            any_hidden = true;
        }
    }

    if (!has_resolved) {
        return GroupVisibilityState::Empty;
    }
    if (any_visible && any_hidden) {
        return GroupVisibilityState::Mixed;
    }
    return any_visible ? GroupVisibilityState::AllVisible : GroupVisibilityState::AllHidden;
}

bool SetGroupMarkerVisibility(
    CatalogUserState& state,
    const UserGroup& group,
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    bool visible,
    bool search_active)
{
    if (search_active) {
        return false;
    }

    bool changed = false;
    for (const MarkerReference& reference : group.marker_references) {
        if (FindCatalogMarker(catalog, identity, reference) == nullptr) {
            continue;
        }
        if (IsMarkerVisible(state, reference.marker_id) != visible) {
            SetMarkerVisible(state, reference.marker_id, visible);
            changed = true;
        }
    }
    return changed;
}

bool MarkerMatchesSearch(
    const SpectralLineCatalog& catalog,
    const CatalogIdentity& identity,
    const MarkerReference& reference,
    std::string_view search)
{
    if (search.empty()) {
        return true;
    }

    const SpectralLineMarker* marker = FindCatalogMarker(catalog, identity, reference);
    if (marker == nullptr) {
        return ContainsCaseInsensitive(reference.marker_id, search);
    }
    return ContainsCaseInsensitive(marker->id, search) || ContainsCaseInsensitive(marker->label, search) ||
           ContainsCaseInsensitive(marker->group, search) || ContainsCaseInsensitive(marker->display_label, search);
}

std::unordered_map<std::string, int> MarkerReferenceCounts(
    const GroupingView& view,
    const CatalogIdentity& identity)
{
    std::unordered_map<std::string, int> counts;
    for (const UserGroup& group : view.groups) {
        for (const MarkerReference& reference : group.marker_references) {
            if (SameCatalogIdentity(reference.catalog_identity, identity)) {
                ++counts[reference.marker_id];
            }
        }
    }
    return counts;
}

bool IsSharedMarkerReference(
    const std::unordered_map<std::string, int>& counts,
    const MarkerReference& reference)
{
    const auto match = counts.find(reference.marker_id);
    return match != counts.end() && match->second > 1;
}

bool AddUserGroup(
    GroupingView& view,
    std::string id,
    std::string name,
    GeneratedNameMetadata generated_name)
{
    if (id.empty() || name.empty() || FindGroup(view, id) != nullptr) {
        return false;
    }

    UserGroup group;
    group.id = std::move(id);
    group.name = std::move(name);
    group.generated_name =
        CanonicalUserGroupGeneratedName(generated_name);

    const auto unassigned = std::find_if(view.groups.begin(), view.groups.end(), [](const auto& existing) {
        return existing.is_unassigned || existing.id == kUnassignedUserGroupId;
    });
    view.groups.insert(unassigned, std::move(group));
    return true;
}

bool RemoveUserGroup(GroupingView& view, std::string_view group_id)
{
    if (group_id.empty()) {
        return false;
    }

    const auto match = std::find_if(view.groups.begin(), view.groups.end(), [group_id](const auto& group) {
        return group.id == group_id;
    });
    if (match == view.groups.end() || IsUnassignedGroup(*match)) {
        return false;
    }

    const std::vector<MarkerReference> removed_references = match->marker_references;
    view.groups.erase(match);
    for (const MarkerReference& reference : removed_references) {
        PreserveReferenceIfNoOrdinaryGroup(view, reference);
    }
    return true;
}

bool ReorderUserGroupBefore(
    GroupingView& view,
    std::string_view source_group_id,
    std::string_view target_group_id)
{
    if (source_group_id.empty() || target_group_id.empty() || source_group_id == target_group_id) {
        return false;
    }

    const auto find_index = [&view](std::string_view group_id) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < view.groups.size(); ++index) {
            if (view.groups[index].id == group_id) {
                return index;
            }
        }
        return std::nullopt;
    };

    std::optional<std::size_t> source_index = find_index(source_group_id);
    std::optional<std::size_t> target_index = find_index(target_group_id);
    if (!source_index || !target_index || IsUnassignedGroup(view.groups[*source_index])) {
        return false;
    }

    std::size_t insert_index = *target_index;
    if (*source_index < *target_index) {
        --insert_index;
    }
    if (insert_index == *source_index) {
        return false;
    }

    UserGroup moving_group = std::move(view.groups[*source_index]);
    view.groups.erase(view.groups.begin() + static_cast<std::ptrdiff_t>(*source_index));

    const auto unassigned = std::find_if(view.groups.begin(), view.groups.end(), IsUnassignedGroup);
    if (unassigned != view.groups.end()) {
        const auto unassigned_index = static_cast<std::size_t>(std::distance(view.groups.begin(), unassigned));
        if (insert_index > unassigned_index) {
            insert_index = unassigned_index;
        }
    }

    view.groups.insert(view.groups.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(moving_group));
    return true;
}

bool MoveMarkerReference(
    GroupingView& view,
    const CatalogIdentity& identity,
    std::string_view marker_id,
    std::string_view source_group_id,
    std::string_view target_group_id)
{
    if (marker_id.empty() || source_group_id == target_group_id) {
        return false;
    }

    UserGroup* target_group = FindGroup(view, target_group_id);
    if (target_group == nullptr) {
        return false;
    }

    bool removed_from_source = false;
    if (UserGroup* source_group = FindGroup(view, source_group_id)) {
        const auto reference = FindReference(source_group->marker_references, identity, marker_id);
        if (reference != source_group->marker_references.end()) {
            source_group->marker_references.erase(reference);
            removed_from_source = true;
        }
    }

    if (!ContainsReference(target_group->marker_references, identity, marker_id)) {
        target_group->marker_references.push_back(MakeReference(identity, std::string(marker_id)));
        return true;
    }
    return removed_from_source;
}

bool RemoveMarkerReferenceFromGroup(
    GroupingView& view,
    const CatalogIdentity& identity,
    std::string_view marker_id,
    std::string_view group_id)
{
    if (marker_id.empty() || group_id.empty()) {
        return false;
    }

    UserGroup* group = FindGroup(view, group_id);
    if (group == nullptr || IsUnassignedGroup(*group)) {
        return false;
    }

    const auto reference = FindReference(group->marker_references, identity, marker_id);
    if (reference == group->marker_references.end()) {
        return false;
    }

    const MarkerReference removed_reference = *reference;
    group->marker_references.erase(reference);
    PreserveReferenceIfNoOrdinaryGroup(view, removed_reference);
    return true;
}

bool CopyMarkerReference(
    GroupingView& view,
    const CatalogIdentity& identity,
    std::string_view marker_id,
    std::string_view target_group_id)
{
    if (marker_id.empty()) {
        return false;
    }
    UserGroup* target_group = FindGroup(view, target_group_id);
    if (target_group == nullptr || IsUnassignedGroup(*target_group) ||
        ContainsReference(target_group->marker_references, identity, marker_id)) {
        return false;
    }
    target_group->marker_references.push_back(MakeReference(identity, std::string(marker_id)));
    return true;
}

std::string GroupExpansionKey(std::string_view view_id, std::string_view group_id)
{
    std::string key;
    key.reserve(view_id.size() + group_id.size() + 1);
    key.append(view_id);
    key.push_back('/');
    key.append(group_id);
    return key;
}

}  // namespace specforge
