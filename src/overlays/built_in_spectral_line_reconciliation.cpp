#include "overlays/built_in_spectral_line_reconciliation.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace spectiary {
namespace {
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

std::string ReferenceKey(const std::string& reference)
{
    return reference;
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

std::unordered_map<std::string, const std::string*> IndexReferences(
    const std::vector<std::string>& references)
{
    std::unordered_map<std::string, const std::string*> result;
    result.reserve(references.size());
    for (const std::string& reference : references) {
        result.emplace(ReferenceKey(reference), &reference);
    }
    return result;
}

std::vector<std::string> ReferenceKeys(
    const std::vector<std::string>& references)
{
    std::vector<std::string> result;
    result.reserve(references.size());
    for (const std::string& reference : references) {
        result.push_back(ReferenceKey(reference));
    }
    return result;
}

std::vector<std::string> MergeReferences(
    const std::vector<std::string>& base,
    const std::vector<std::string>& local,
    const std::vector<std::string>& latest)
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

    std::vector<std::string> result;
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
                (*local_match->second == *base_match->second)
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

std::vector<line_list::Group> MergeGroups(
    const std::vector<line_list::Group>& base,
    const std::vector<line_list::Group>& local,
    const std::vector<line_list::Group>& latest,
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
    for (const line_list::Group& group : local) {
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

    std::vector<line_list::Group> remapped_local;
    remapped_local.reserve(local.size());
    for (const line_list::Group& source : local) {
        line_list::Group value = source;
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
    for (const line_list::Group& group : remapped_local) {
        local_order.push_back(group.id);
    }
    const std::vector<std::string> order = MergeOrderedIds(
        Ids(base),
        local_order,
        Ids(latest),
        final_ids,
        local_ordering_is_explicit);
    std::vector<line_list::Group> result;
    result.reserve(order.size());
    for (const std::string& id : order) {
        const auto base_match = base_by_id_after.find(id);
        const auto local_match = remapped_local_by_id.find(id);
        const auto latest_match = latest_by_id.find(id);
        if (base_match != base_by_id_after.end() &&
            local_match != remapped_local_by_id.end() &&
            latest_match != latest_by_id.end()) {
            const line_list::Group& base_group = *base_match->second;
            const line_list::Group& local_group = *local_match->second;
            const line_list::Group& latest_group = *latest_match->second;
            line_list::Group merged = local_group;
            merged.name = local_group.name == base_group.name ? latest_group.name : local_group.name;
            merged.marker_ids = MergeReferences(
                base_group.marker_ids,
                local_group.marker_ids,
                latest_group.marker_ids);
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
    line_list::GroupingView& view,
    std::string_view local_view_id,
    ReconciliationContext& context)
{
    auto& remap = context.local_group_id_remap[std::string(local_view_id)];
    for (line_list::Group& group : view.groups) {

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

line_list::GroupingView MergeView(
    const line_list::GroupingView& base,
    const line_list::GroupingView& local,
    const line_list::GroupingView& latest,
    ReconciliationContext& context,
    bool local_ordering_is_explicit)
{
    line_list::GroupingView result = local;
    result.name = local.name == base.name ? latest.name : local.name;
    result.groups = MergeGroups(
        base.groups,
        local.groups,
        latest.groups,
        local.id,
        context,
        local_ordering_is_explicit);
    return result;
}

std::vector<line_list::GroupingView> MergeViews(
    const std::vector<line_list::GroupingView>& base,
    const std::vector<line_list::GroupingView>& local,
    const std::vector<line_list::GroupingView>& latest,
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
    for (const line_list::GroupingView& view : local) {
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

    std::vector<line_list::GroupingView> remapped_local;
    remapped_local.reserve(local.size());
    for (const line_list::GroupingView& source : local) {
        line_list::GroupingView value = source;
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
    for (const line_list::GroupingView& view : remapped_local) {
        local_order.push_back(view.id);
    }
    const std::vector<std::string> order = MergeOrderedIds(
        Ids(base),
        local_order,
        Ids(latest),
        final_ids);
    std::vector<line_list::GroupingView> result;
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

template <typename Map>
void MergeMarkerFields(
    const Map& base,
    const Map& local,
    const Map& latest,
    Map& result)
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
    key = effective_view_id + "/" + effective_group_id;
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


} // namespace

bool ReconcileBuiltInSpectralLineTask(const SpectralLineList& packaged,
    const BuiltInSpectralLineState& base, const BuiltInSpectralLineState& local,
    const BuiltInSpectralLineState& latest, const BuiltInSpectralLineTaskIntent& intent,
    BuiltInSpectralLineState& result, std::string& error)
{
    error.clear();
    const auto before = ComposeBuiltInSpectralLineList(packaged, base.overlay);
    const auto current = ComposeBuiltInSpectralLineList(packaged, local.overlay);
    const auto durable = ComposeBuiltInSpectralLineList(packaged, latest.overlay);
    for (const auto* value : {&before, &current, &durable}) {
        if (!value->list) { error = value->error; return false; }
    }
    const auto conflict = [&](const auto& b, const auto& l, const auto& r) {
        const auto lb = IndexById(l), rb = IndexById(r);
        for (const auto& value : b) {
            if (lb.contains(value.id) && !rb.contains(value.id) && *lb.at(value.id) != value) return true;
        }
        return false;
    };
    if (conflict(base.overlay.grouping_views, local.overlay.grouping_views, latest.overlay.grouping_views)) {
        error = "edited grouping view was deleted by another writer"; return false;
    }
    const auto local_views = IndexById(local.overlay.grouping_views);
    const auto latest_views = IndexById(latest.overlay.grouping_views);
    for (const auto& view : base.overlay.grouping_views) {
        if (local_views.contains(view.id) && latest_views.contains(view.id) &&
            conflict(view.groups, local_views.at(view.id)->groups, latest_views.at(view.id)->groups)) {
            error = "edited group was deleted by another writer"; return false;
        }
    }
    BuiltInSpectralLineState merged = latest;
    ReconciliationContext context;
    for (const auto* views : {&packaged.grouping_views, &base.overlay.grouping_views, &latest.overlay.grouping_views}) {
        for (const auto& view : *views) {
            context.occupied_view_ids.insert(view.id);
            for (const auto& group : view.groups) context.occupied_group_ids.insert(group.id);
        }
    }
    merged.overlay.grouping_views = MergeViews(base.overlay.grouping_views, local.overlay.grouping_views,
        latest.overlay.grouping_views, context, intent.group_ordering_view_ids);
    if (intent.whole_color_collection) merged.overlay.color_schemes = local.overlay.color_schemes;
    else {
        const auto& b = before.list->color_schemes;
        const auto& l = current.list->color_schemes;
        const auto& r = durable.list->color_schemes;
        auto bi = IndexById(b), li = IndexById(l), ri = IndexById(r);
        if (conflict(b, l, r)) { error = "edited color scheme was deleted by another writer"; return false; }
        std::vector<line_list::ColorScheme> schemes = r;
        bool changed = false;
        for (const auto& old : b) {
            if (!li.contains(old.id)) {
                std::erase_if(schemes, [&](const auto& s) { return s.id == old.id; });
                changed = true;
            }
        }
        for (const auto& value : l) {
            auto target = std::find_if(schemes.begin(), schemes.end(), [&](const auto& s) { return s.id == value.id; });
            if (!bi.contains(value.id)) {
                if (target == schemes.end()) schemes.push_back(value);
                else {
                    std::map<std::string, std::string> empty;
                    auto colors = target->colors;
                    MergeMarkerFields(empty, value.colors, target->colors, colors);
                    target->colors = std::move(colors);
                }
                changed = true;
                continue;
            }
            const auto& old = *bi.at(value.id);
            const bool field_intent = std::any_of(intent.marker_colors.begin(), intent.marker_colors.end(),
                [&](const auto& field) { return field.first == value.id; });
            if (value == old && !field_intent) continue;
            if (target == schemes.end()) { error = "color scheme no longer exists"; return false; }
            auto color_result = target->colors;
            MergeMarkerFields(old.colors, value.colors, target->colors, color_result);
            for (const auto& [scheme_id, marker_id] : intent.marker_colors) {
                if (scheme_id != value.id) continue;
                if (value.colors.contains(marker_id)) color_result[marker_id] = value.colors.at(marker_id);
                else color_result.erase(marker_id);
            }
            target->colors = std::move(color_result);
            if (value.name != old.name) target->name = value.name;
            changed = true;
        }
        if (changed) merged.overlay.color_schemes = std::move(schemes);
    }
    auto& session = merged.session;
    session.active_view_id = intent.view_selection ? local.session.active_view_id : latest.session.active_view_id;
    session.active_color_scheme_id = intent.color_selection ? local.session.active_color_scheme_id : latest.session.active_color_scheme_id;
    if (intent.view_selection && context.local_view_id_remap.contains(session.active_view_id))
        session.active_view_id = context.local_view_id_remap.at(session.active_view_id);
    MergeMarkerFields(base.session.marker_visibility, local.session.marker_visibility, latest.session.marker_visibility, session.marker_visibility);
    MergeExpandedGroups(base.session.expanded_group_ids, local.session.expanded_group_ids, latest.session.expanded_group_ids, context, session.expanded_group_ids);
    MergeMarkerFields(base.session.view_names, local.session.view_names, latest.session.view_names, session.view_names);
    MergeMarkerFields(base.session.group_names, local.session.group_names, latest.session.group_names, session.group_names);
    for (const auto& [old, fresh] : context.local_view_id_remap)
        if (local.session.view_names.contains(old)) session.view_names[fresh] = local.session.view_names.at(old);
    for (const auto& [view, remap] : context.local_group_id_remap) {
        (void)view;
        for (const auto& [old, fresh] : remap)
            if (local.session.group_names.contains(old)) session.group_names[fresh] = local.session.group_names.at(old);
    }
    const auto composed = ComposeBuiltInSpectralLineList(packaged, merged.overlay);
    if (!composed.list) { error = composed.error; return false; }
    NormalizeSpectralLineSession(session, *composed.list);
    result = std::move(merged);
    return true;
}
} // namespace spectiary


