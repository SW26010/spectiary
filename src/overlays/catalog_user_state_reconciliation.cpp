#include "overlays/catalog_user_state_reconciliation.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

bool SameIdentity(const CatalogIdentity& left, const CatalogIdentity& right)
{
    return left.id == right.id && left.display_name == right.display_name;
}

bool SameReference(const MarkerReference& left, const MarkerReference& right)
{
    return SameIdentity(left.catalog_identity, right.catalog_identity) &&
           left.marker_id == right.marker_id;
}

template <typename Value>
std::unordered_map<std::string, const Value*> IndexById(
    const std::vector<Value>& values)
{
    std::unordered_map<std::string, const Value*> result;
    result.reserve(values.size());
    for (const Value& value : values) {
        result.emplace(value.id, &value);
    }
    return result;
}

template <typename Value>
std::vector<std::string> Ids(const std::vector<Value>& values)
{
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const Value& value : values) {
        result.push_back(value.id);
    }
    return result;
}

bool RelativeOrderChanged(
    const std::vector<std::string>& base,
    const std::vector<std::string>& candidate)
{
    const std::unordered_set<std::string> base_ids(
        base.begin(),
        base.end());
    std::vector<std::string> base_common;
    std::vector<std::string> candidate_common;
    base_common.reserve(base.size());
    candidate_common.reserve(candidate.size());

    const std::unordered_set<std::string> candidate_ids(
        candidate.begin(),
        candidate.end());
    for (const std::string& id : base) {
        if (candidate_ids.contains(id)) {
            base_common.push_back(id);
        }
    }
    for (const std::string& id : candidate) {
        if (base_ids.contains(id)) {
            candidate_common.push_back(id);
        }
    }
    return base_common != candidate_common;
}

std::string FreshId(
    std::string_view requested,
    const std::unordered_set<std::string>& occupied)
{
    std::string candidate(requested);
    int suffix = 2;
    while (occupied.contains(candidate)) {
        candidate = std::string(requested) + "-" + std::to_string(suffix++);
    }
    return candidate;
}

struct ReconciliationContext {
    // Collision checks cover live objects from all participating snapshots.
    std::unordered_set<std::string> occupied_view_ids;
    std::unordered_set<std::string> occupied_group_ids;
    std::unordered_map<std::string, std::string> local_view_id_remap;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::string>> local_group_id_remap;
};

std::string ReferenceKey(const MarkerReference& reference)
{
    return reference.catalog_identity.id + "\n" + reference.marker_id;
}

void AppendUnique(
    std::vector<std::string>& output,
    const std::unordered_set<std::string>& allowed,
    const std::string& value)
{
    if (!allowed.contains(value) ||
        std::find(output.begin(), output.end(), value) != output.end()) {
        return;
    }
    output.push_back(value);
}

std::vector<std::string> MergeOrderedIds(
    const std::vector<std::string>& base,
    const std::vector<std::string>& local,
    const std::vector<std::string>& latest,
    const std::unordered_set<std::string>& final_ids,
    bool local_ordering_is_explicit = false)
{
    const bool local_reordered = RelativeOrderChanged(base, local);
    const std::unordered_set<std::string> base_ids(
        base.begin(),
        base.end());
    std::vector<std::string> result;
    result.reserve(final_ids.size());

    if (local_ordering_is_explicit) {
        const std::unordered_set<std::string> local_ids(
            local.begin(),
            local.end());
        std::vector<std::string> latest_only_additions;
        latest_only_additions.reserve(latest.size());
        for (const std::string& id : latest) {
            if (!base_ids.contains(id) && !local_ids.contains(id)) {
                latest_only_additions.push_back(id);
            }
        }

        // An explicit group reorder owns the local final order, including
        // additions. Durable-only additions remain visible, but are placed
        // before the first local addition so that the local addition's
        // position relative to existing entities survives reconciliation.
        bool inserted_latest_additions = false;
        for (const std::string& id : local) {
            if (!base_ids.contains(id) && !inserted_latest_additions) {
                for (const std::string& latest_id : latest_only_additions) {
                    AppendUnique(result, final_ids, latest_id);
                }
                inserted_latest_additions = true;
            }
            AppendUnique(result, final_ids, id);
        }
        if (!inserted_latest_additions) {
            for (const std::string& latest_id : latest_only_additions) {
                AppendUnique(result, final_ids, latest_id);
            }
        }
        for (const std::string& id : latest) {
            AppendUnique(result, final_ids, id);
        }
        return result;
    }

    const auto append_base_entities = [&](const std::vector<std::string>& order) {
        for (const std::string& id : order) {
            if (base_ids.contains(id)) {
                AppendUnique(result, final_ids, id);
            }
        }
    };
    append_base_entities(local_reordered ? local : latest);

    // A concurrent addition does not conflict with a reorder of the base
    // entities. Keep both additions, with the durable addition first and the
    // task addition after it; the next task can explicitly reorder them.
    for (const std::string& id : latest) {
        if (!base_ids.contains(id)) {
            AppendUnique(result, final_ids, id);
        }
    }
    for (const std::string& id : local) {
        if (!base_ids.contains(id)) {
            AppendUnique(result, final_ids, id);
        }
    }

    // This also handles malformed/legacy input with an id omitted from one
    // ordering vector without dropping a validated entity.
    for (const std::string& id : latest) {
        AppendUnique(result, final_ids, id);
    }
    for (const std::string& id : local) {
        AppendUnique(result, final_ids, id);
    }
    return result;
}

std::unordered_map<std::string, const MarkerReference*> IndexReferences(
    const std::vector<MarkerReference>& references)
{
    std::unordered_map<std::string, const MarkerReference*> result;
    result.reserve(references.size());
    for (const MarkerReference& reference : references) {
        result.emplace(ReferenceKey(reference), &reference);
    }
    return result;
}

std::vector<std::string> ReferenceKeys(
    const std::vector<MarkerReference>& references)
{
    std::vector<std::string> result;
    result.reserve(references.size());
    for (const MarkerReference& reference : references) {
        result.push_back(ReferenceKey(reference));
    }
    return result;
}

std::vector<MarkerReference> MergeReferences(
    const std::vector<MarkerReference>& base,
    const std::vector<MarkerReference>& local,
    const std::vector<MarkerReference>& latest)
{
    const auto base_by_id = IndexReferences(base);
    const auto local_by_id = IndexReferences(local);
    const auto latest_by_id = IndexReferences(latest);
    std::unordered_set<std::string> final_ids;
    final_ids.reserve(base.size() + local.size() + latest.size());

    for (const auto& [id, reference] : base_by_id) {
        (void)reference;
        if (local_by_id.contains(id) && latest_by_id.contains(id)) {
            final_ids.insert(id);
        }
    }
    for (const auto& [id, reference] : local_by_id) {
        (void)reference;
        if (!base_by_id.contains(id)) {
            final_ids.insert(id);
        }
    }
    for (const auto& [id, reference] : latest_by_id) {
        (void)reference;
        if (!base_by_id.contains(id)) {
            final_ids.insert(id);
        }
    }

    std::vector<MarkerReference> result;
    for (const std::string& id : MergeOrderedIds(
             ReferenceKeys(base),
             ReferenceKeys(local),
             ReferenceKeys(latest),
             final_ids)) {
        const auto local_match = local_by_id.find(id);
        const auto latest_match = latest_by_id.find(id);
        const auto base_match = base_by_id.find(id);
        if (base_match != base_by_id.end() &&
            local_match != local_by_id.end() &&
            latest_match != latest_by_id.end()) {
            // Reference identity is the ownership key. The value is normally
            // identical; prefer the task copy if an imported display identity
            // differs.
            result.push_back(
                SameReference(
                    *local_match->second,
                    *base_match->second)
                    ? *latest_match->second
                    : *local_match->second);
        } else if (local_match != local_by_id.end()) {
            result.push_back(*local_match->second);
        } else if (latest_match != latest_by_id.end()) {
            result.push_back(*latest_match->second);
        }
    }
    return result;
}

std::vector<UserGroup> MergeGroups(
    const std::vector<UserGroup>& base,
    const std::vector<UserGroup>& local,
    const std::vector<UserGroup>& latest,
    std::string_view view_id,
    ReconciliationContext& context,
    bool local_ordering_is_explicit)
{
    const auto base_by_id = IndexById(base);
    const auto local_by_id = IndexById(local);
    const auto latest_by_id = IndexById(latest);
    std::unordered_set<std::string>& occupied = context.occupied_group_ids;

    auto& remap = context.local_group_id_remap[std::string(view_id)];
    std::unordered_map<std::string, std::string> local_effective_ids;
    std::vector<std::string> local_addition_ids;
    local_addition_ids.reserve(local.size());
    for (const UserGroup& group : local) {
        if (!base_by_id.contains(group.id)) {
            local_addition_ids.push_back(group.id);
        }
    }
    // The index is for lookup only. Allocate colliding requests in lexical
    // order so suffix assignment never depends on unordered_map iteration.
    std::sort(local_addition_ids.begin(), local_addition_ids.end());
    local_addition_ids.erase(
        std::unique(local_addition_ids.begin(), local_addition_ids.end()),
        local_addition_ids.end());
    for (const std::string& id : local_addition_ids) {
        if (base_by_id.contains(id)) {
            local_effective_ids.emplace(id, id);
            continue;
        }
        if (!occupied.contains(id)) {
            local_effective_ids.emplace(id, id);
            occupied.insert(id);
            continue;
        }
        const std::string fresh = FreshId(id, occupied);
        occupied.insert(fresh);
        remap[id] = fresh;
        local_effective_ids.emplace(id, fresh);
    }

    std::vector<UserGroup> remapped_local;
    remapped_local.reserve(local.size());
    for (const UserGroup& source : local) {
        UserGroup value = source;
        const auto match = local_effective_ids.find(source.id);
        if (match != local_effective_ids.end()) {
            value.id = match->second;
        }
        remapped_local.push_back(std::move(value));
    }

    const auto remapped_local_by_id = IndexById(remapped_local);
    const auto base_by_id_after = IndexById(base);
    std::unordered_set<std::string> final_ids;
    for (const auto& [id, value] : base_by_id_after) {
        (void)value;
        if (remapped_local_by_id.contains(id) && latest_by_id.contains(id)) {
            final_ids.insert(id);
        }
    }
    for (const auto& [id, value] : remapped_local_by_id) {
        (void)value;
        if (!base_by_id_after.contains(id)) {
            final_ids.insert(id);
        }
    }
    for (const auto& [id, value] : latest_by_id) {
        (void)value;
        if (!base_by_id_after.contains(id)) {
            final_ids.insert(id);
        }
    }

    std::vector<std::string> local_order;
    local_order.reserve(remapped_local.size());
    for (const UserGroup& group : remapped_local) {
        local_order.push_back(group.id);
    }
    const std::vector<std::string> order = MergeOrderedIds(
        Ids(base),
        local_order,
        Ids(latest),
        final_ids,
        local_ordering_is_explicit);
    std::vector<UserGroup> result;
    result.reserve(order.size());
    for (const std::string& id : order) {
        const auto base_match = base_by_id_after.find(id);
        const auto local_match = remapped_local_by_id.find(id);
        const auto latest_match = latest_by_id.find(id);
        if (base_match != base_by_id_after.end() &&
            local_match != remapped_local_by_id.end() &&
            latest_match != latest_by_id.end()) {
            const UserGroup& base_group = *base_match->second;
            const UserGroup& local_group = *local_match->second;
            const UserGroup& latest_group = *latest_match->second;
            UserGroup merged = local_group;
            merged.name = local_group.name == base_group.name ? latest_group.name : local_group.name;
            merged.generated_name =
                local_group.generated_name == base_group.generated_name
                    ? latest_group.generated_name
                    : local_group.generated_name;
            merged.is_unassigned =
                local_group.is_unassigned == base_group.is_unassigned
                    ? latest_group.is_unassigned
                    : local_group.is_unassigned;
            merged.marker_references = MergeReferences(
                base_group.marker_references,
                local_group.marker_references,
                latest_group.marker_references);
            result.push_back(std::move(merged));
        } else if (local_match != remapped_local_by_id.end()) {
            result.push_back(*local_match->second);
        } else if (latest_match != latest_by_id.end()) {
            result.push_back(*latest_match->second);
        }
    }
    return result;
}

void RemapStandaloneViewGroups(
    GroupingView& view,
    std::string_view local_view_id,
    ReconciliationContext& context)
{
    auto& remap = context.local_group_id_remap[std::string(local_view_id)];
    for (UserGroup& group : view.groups) {
        if (group.is_unassigned || group.id == UnassignedUserGroupId()) {
            continue;
        }
        if (context.occupied_group_ids.contains(group.id)) {
            const std::string requested = group.id;
            const std::string fresh =
                FreshId(requested, context.occupied_group_ids);
            context.occupied_group_ids.insert(fresh);
            remap[requested] = fresh;
            group.id = fresh;
        } else {
            context.occupied_group_ids.insert(group.id);
        }
    }
}

GroupingView MergeView(
    const GroupingView& base,
    const GroupingView& local,
    const GroupingView& latest,
    ReconciliationContext& context,
    bool local_ordering_is_explicit)
{
    GroupingView result = local;
    result.name = local.name == base.name ? latest.name : local.name;
    result.generated_name =
        local.generated_name == base.generated_name
            ? latest.generated_name
            : local.generated_name;
    result.read_only = local.read_only == base.read_only ? latest.read_only : local.read_only;
    result.groups = MergeGroups(
        base.groups,
        local.groups,
        latest.groups,
        local.id,
        context,
        local_ordering_is_explicit);
    return result;
}

std::vector<GroupingView> MergeViews(
    const std::vector<GroupingView>& base,
    const std::vector<GroupingView>& local,
    const std::vector<GroupingView>& latest,
    ReconciliationContext& context,
    const std::unordered_set<std::string>& local_group_ordering_view_ids)
{
    const auto base_by_id = IndexById(base);
    const auto local_by_id = IndexById(local);
    const auto latest_by_id = IndexById(latest);
    std::unordered_set<std::string>& occupied = context.occupied_view_ids;

    std::unordered_map<std::string, std::string> local_effective_ids;
    std::vector<std::string> local_addition_ids;
    local_addition_ids.reserve(local.size());
    for (const GroupingView& view : local) {
        if (!base_by_id.contains(view.id)) {
            local_addition_ids.push_back(view.id);
        }
    }
    // Keep fresh view identities independent of hash-table iteration order.
    std::sort(local_addition_ids.begin(), local_addition_ids.end());
    local_addition_ids.erase(
        std::unique(local_addition_ids.begin(), local_addition_ids.end()),
        local_addition_ids.end());
    for (const std::string& id : local_addition_ids) {
        if (base_by_id.contains(id)) {
            local_effective_ids.emplace(id, id);
            continue;
        }
        if (!occupied.contains(id)) {
            local_effective_ids.emplace(id, id);
            occupied.insert(id);
            continue;
        }
        const std::string fresh = FreshId(id, occupied);
        occupied.insert(fresh);
        context.local_view_id_remap[id] = fresh;
        local_effective_ids.emplace(id, fresh);
    }

    std::vector<GroupingView> remapped_local;
    remapped_local.reserve(local.size());
    for (const GroupingView& source : local) {
        GroupingView value = source;
        const auto match = local_effective_ids.find(source.id);
        if (match != local_effective_ids.end()) {
            value.id = match->second;
        }
        if (!base_by_id.contains(source.id)) {
            RemapStandaloneViewGroups(value, source.id, context);
        }
        remapped_local.push_back(std::move(value));
    }
    const auto remapped_local_by_id = IndexById(remapped_local);

    std::unordered_set<std::string> final_ids;
    for (const auto& [id, value] : base_by_id) {
        (void)value;
        if (remapped_local_by_id.contains(id) && latest_by_id.contains(id)) {
            final_ids.insert(id);
        }
    }
    for (const auto& [id, value] : remapped_local_by_id) {
        (void)value;
        if (!base_by_id.contains(id)) {
            final_ids.insert(id);
        }
    }
    for (const auto& [id, value] : latest_by_id) {
        (void)value;
        if (!base_by_id.contains(id)) {
            final_ids.insert(id);
        }
    }

    std::vector<std::string> local_order;
    local_order.reserve(remapped_local.size());
    for (const GroupingView& view : remapped_local) {
        local_order.push_back(view.id);
    }
    const std::vector<std::string> order = MergeOrderedIds(
        Ids(base),
        local_order,
        Ids(latest),
        final_ids);
    std::vector<GroupingView> result;
    result.reserve(order.size());
    for (const std::string& id : order) {
        const auto base_match = base_by_id.find(id);
        const auto local_match = remapped_local_by_id.find(id);
        const auto latest_match = latest_by_id.find(id);
        if (base_match != base_by_id.end() &&
            local_match != remapped_local_by_id.end() &&
            latest_match != latest_by_id.end()) {
            result.push_back(MergeView(
                *base_match->second,
                *local_match->second,
                *latest_match->second,
                context,
                local_group_ordering_view_ids.contains(
                    base_match->second->id) ||
                    local_group_ordering_view_ids.contains(
                        local_match->second->id)));
        } else if (local_match != remapped_local_by_id.end()) {
            result.push_back(*local_match->second);
        } else if (latest_match != latest_by_id.end()) {
            result.push_back(*latest_match->second);
        }
    }
    return result;
}

template <typename Value>
void MergeMarkerFields(
    const std::unordered_map<std::string, Value>& base,
    const std::unordered_map<std::string, Value>& local,
    const std::unordered_map<std::string, Value>& latest,
    std::unordered_map<std::string, Value>& result)
{
    result.clear();
    std::unordered_set<std::string> keys;
    for (const auto& [key, value] : base) {
        (void)value;
        keys.insert(key);
    }
    for (const auto& [key, value] : local) {
        (void)value;
        keys.insert(key);
    }
    for (const auto& [key, value] : latest) {
        (void)value;
        keys.insert(key);
    }
    for (const std::string& key : keys) {
        const auto base_match = base.find(key);
        const auto local_match = local.find(key);
        const auto latest_match = latest.find(key);
        const bool local_changed =
            (base_match == base.end()) != (local_match == local.end()) ||
            (base_match != base.end() &&
             local_match != local.end() &&
             base_match->second != local_match->second);
        if (local_changed) {
            if (local_match != local.end()) {
                result.emplace(key, local_match->second);
            }
        } else if (latest_match != latest.end()) {
            result.emplace(key, latest_match->second);
        }
    }
}

void RemapExpansionKey(
    std::string& key,
    const ReconciliationContext& context)
{
    const std::size_t separator = key.find('/');
    if (separator == std::string::npos) {
        return;
    }
    const std::string local_view_id = key.substr(0, separator);
    const std::string local_group_id = key.substr(separator + 1);
    std::string effective_view_id = local_view_id;
    if (const auto view_match = context.local_view_id_remap.find(local_view_id);
        view_match != context.local_view_id_remap.end()) {
        effective_view_id = view_match->second;
    }
    std::string effective_group_id = local_group_id;
    const auto group_map = context.local_group_id_remap.find(local_view_id);
    if (group_map != context.local_group_id_remap.end()) {
        if (const auto group_match = group_map->second.find(local_group_id);
            group_match != group_map->second.end()) {
            effective_group_id = group_match->second;
        }
    }
    key = GroupExpansionKey(effective_view_id, effective_group_id);
}

void MergeExpandedGroups(
    const std::unordered_set<std::string>& base,
    const std::unordered_set<std::string>& local,
    const std::unordered_set<std::string>& latest,
    const ReconciliationContext& context,
    std::unordered_set<std::string>& result)
{
    std::unordered_set<std::string> remapped_local = local;
    std::unordered_set<std::string> remapped_keys;
    remapped_keys.reserve(remapped_local.size());
    for (const std::string& key : remapped_local) {
        std::string remapped_key = key;
        RemapExpansionKey(remapped_key, context);
        remapped_keys.insert(std::move(remapped_key));
    }
    remapped_local = std::move(remapped_keys);

    result.clear();
    std::unordered_set<std::string> keys;
    keys.insert(base.begin(), base.end());
    keys.insert(remapped_local.begin(), remapped_local.end());
    keys.insert(latest.begin(), latest.end());
    for (const std::string& key : keys) {
        const bool base_present = base.contains(key);
        const bool local_present = remapped_local.contains(key);
        const bool latest_present = latest.contains(key);
        if (local_present != base_present) {
            if (local_present) {
                result.insert(key);
            }
        } else if (latest_present) {
            result.insert(key);
        }
    }
}

}  // namespace

bool ReconcileCatalogUserStateTask(
    const CatalogUserState& base,
    const CatalogUserState& local,
    const CatalogUserState& latest,
    const CatalogPanelState& base_panel_state,
    const CatalogPanelState& local_panel_state,
    const CatalogPanelState& latest_panel_state,
    CatalogUserStateReconciliationResult& result,
    std::string& diagnostic,
    std::optional<bool> local_selection_is_explicit,
    std::unordered_set<std::string> local_group_ordering_view_ids)
{
    diagnostic.clear();
    if (local.catalog_identity.id.empty() ||
        base.catalog_identity.id.empty() ||
        latest.catalog_identity.id.empty()) {
        diagnostic =
            "catalog user-state reconciliation requires a non-empty catalog identity";
        return false;
    }

    ReconciliationContext context;
    for (const GroupingView& view : base.grouping_views) {
        context.occupied_view_ids.insert(view.id);
        for (const UserGroup& group : view.groups) {
            if (!group.is_unassigned &&
                group.id != UnassignedUserGroupId()) {
                context.occupied_group_ids.insert(group.id);
            }
        }
    }
    for (const GroupingView& view : latest.grouping_views) {
        context.occupied_view_ids.insert(view.id);
        for (const UserGroup& group : view.groups) {
            if (!group.is_unassigned &&
                group.id != UnassignedUserGroupId()) {
                context.occupied_group_ids.insert(group.id);
            }
        }
    }
    result.state = local;
    result.state.catalog_identity =
        local.catalog_identity.id == base.catalog_identity.id
            ? latest.catalog_identity
            : local.catalog_identity;
    const bool local_selection_changed =
        local_selection_is_explicit.value_or(
            local.active_view_id != base.active_view_id);
    result.state.active_view_id = local_selection_changed
        ? local.active_view_id
        : latest.active_view_id;
    result.state.marker_visibility.clear();
    MergeMarkerFields(
        base.marker_visibility,
        local.marker_visibility,
        latest.marker_visibility,
        result.state.marker_visibility);
    result.state.marker_colors.clear();
    MergeMarkerFields(
        base.marker_colors,
        local.marker_colors,
        latest.marker_colors,
        result.state.marker_colors);
    result.state.grouping_views = MergeViews(
        base.grouping_views,
        local.grouping_views,
        latest.grouping_views,
        context,
        local_group_ordering_view_ids);
    if (local_selection_changed) {
        if (const auto view_match = context.local_view_id_remap.find(
                result.state.active_view_id);
            view_match != context.local_view_id_remap.end()) {
            result.state.active_view_id = view_match->second;
        }
    }

    MergeExpandedGroups(
        base_panel_state.expanded_group_ids,
        local_panel_state.expanded_group_ids,
        latest_panel_state.expanded_group_ids,
        context,
        result.panel_state.expanded_group_ids);
    return true;
}

}  // namespace specforge
