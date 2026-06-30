#include "ui/sample_workflow_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"

#include <algorithm>
#include <ostream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kStateFormatKind = "specforge.sample_workflow_state.cache";
constexpr int kStateSchemaVersion = 1;

const char* SortDirectionName(SampleNavigationSortDirection direction)
{
    switch (direction) {
    case SampleNavigationSortDirection::Ascending:
        return "ascending";
    case SampleNavigationSortDirection::Descending:
        return "descending";
    }
    return "ascending";
}

SampleNavigationSortDirection ParseSortDirection(const std::string& value)
{
    if (value == "descending") {
        return SampleNavigationSortDirection::Descending;
    }
    return SampleNavigationSortDirection::Ascending;
}

std::vector<std::string> SortedAllowedValues(const std::unordered_set<std::string>& values)
{
    std::vector<std::string> sorted_values;
    sorted_values.reserve(values.size());
    for (const std::string& value : values) {
        sorted_values.push_back(value);
    }
    std::sort(sorted_values.begin(), sorted_values.end());
    return sorted_values;
}

std::vector<std::string> UniqueStrings(std::vector<std::string> values)
{
    std::vector<std::string> unique_values;
    unique_values.reserve(values.size());
    std::unordered_set<std::string> seen;
    for (std::string& value : values) {
        if (value.empty() || !seen.emplace(value).second) {
            continue;
        }
        unique_values.push_back(std::move(value));
    }
    return unique_values;
}

bool HasState(const SampleWorkflowSourceState& state)
{
    return !state.filter_conditions.empty() ||
           !state.selected_filter_source_ids.empty() ||
           !state.selected_sample_sort_source_ids.empty() ||
           (state.selected_sample_sort_source_id && !state.selected_sample_sort_source_id->empty()) ||
           state.selected_sample_sort_direction != SampleNavigationSortDirection::Ascending;
}

void WriteStringArrayMember(
    std::ostream& stream,
    const char* name,
    const std::vector<std::string>& values,
    bool& wrote_member)
{
    if (values.empty()) {
        return;
    }
    if (wrote_member) {
        stream << ",\n";
    }
    stream << "      \"" << name << "\": [";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        WriteJsonString(stream, values[index]);
    }
    stream << "]";
    wrote_member = true;
}

void WriteFilterConditions(
    std::ostream& stream,
    const std::vector<SampleFilterCondition>& conditions,
    bool& wrote_member)
{
    if (conditions.empty()) {
        return;
    }
    std::vector<SampleFilterCondition> sorted_conditions = conditions;
    std::sort(
        sorted_conditions.begin(),
        sorted_conditions.end(),
        [](const SampleFilterCondition& left, const SampleFilterCondition& right) {
            return left.source_id < right.source_id;
        });

    if (wrote_member) {
        stream << ",\n";
    }
    stream << "      \"filters\": [";
    for (std::size_t condition_index = 0; condition_index < sorted_conditions.size(); ++condition_index) {
        const SampleFilterCondition& condition = sorted_conditions[condition_index];
        const std::vector<std::string> values = SortedAllowedValues(condition.allowed_value_keys);
        if (condition_index > 0) {
            stream << ",";
        }
        stream << "\n";
        stream << "        { \"source_id\": ";
        WriteJsonString(stream, condition.source_id);
        stream << ", \"allowed_values\": [";
        for (std::size_t value_index = 0; value_index < values.size(); ++value_index) {
            if (value_index > 0) {
                stream << ", ";
            }
            WriteJsonString(stream, values[value_index]);
        }
        stream << "] }";
    }
    stream << "\n      ]";
    wrote_member = true;
}

void WriteSortState(std::ostream& stream, const SampleWorkflowSourceState& state, bool& wrote_member)
{
    if ((!state.selected_sample_sort_source_id || state.selected_sample_sort_source_id->empty()) &&
        state.selected_sample_sort_direction == SampleNavigationSortDirection::Ascending) {
        return;
    }
    if (wrote_member) {
        stream << ",\n";
    }
    stream << "      \"sorting\": { \"direction\": ";
    WriteJsonString(stream, SortDirectionName(state.selected_sample_sort_direction));
    if (state.selected_sample_sort_source_id && !state.selected_sample_sort_source_id->empty()) {
        stream << ", \"source_id\": ";
        WriteJsonString(stream, *state.selected_sample_sort_source_id);
    }
    stream << " }";
    wrote_member = true;
}

std::vector<SampleFilterCondition> ParseFilterConditions(const JsonValue& source_object)
{
    std::vector<SampleFilterCondition> conditions;
    const JsonValue* filters = JsonObjectMember(source_object, "filters");
    if (filters == nullptr || filters->kind != JsonValue::Kind::Array) {
        return conditions;
    }

    for (const JsonValue& condition_object : filters->array) {
        if (condition_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        std::optional<std::string> source_id = ReadJsonStringMember(condition_object, "source_id");
        if (!source_id || source_id->empty()) {
            continue;
        }
        const JsonValue* values = JsonObjectMember(condition_object, "allowed_values");
        if (values == nullptr || values->kind != JsonValue::Kind::Array) {
            continue;
        }

        std::unordered_set<std::string> allowed_values;
        for (const JsonValue& value : values->array) {
            if (value.kind == JsonValue::Kind::String && !value.string_value.empty()) {
                allowed_values.insert(value.string_value);
            }
        }
        if (!allowed_values.empty()) {
            SampleFilterCondition condition;
            condition.source_id = std::move(*source_id);
            condition.allowed_value_keys = std::move(allowed_values);
            conditions.push_back(std::move(condition));
        }
    }
    return conditions;
}

std::vector<std::string> ParseStringArrayMember(const JsonValue& source_object, const char* name)
{
    std::vector<std::string> values;
    const JsonValue* array = JsonObjectMember(source_object, name);
    if (array == nullptr || array->kind != JsonValue::Kind::Array) {
        return values;
    }
    for (const JsonValue& value : array->array) {
        if (value.kind == JsonValue::Kind::String && !value.string_value.empty()) {
            values.push_back(value.string_value);
        }
    }
    return UniqueStrings(std::move(values));
}

void ParseSortState(const JsonValue& source_object, SampleWorkflowSourceState& state)
{
    const JsonValue* sorting = JsonObjectMember(source_object, "sorting");
    if (sorting == nullptr || sorting->kind != JsonValue::Kind::Object) {
        return;
    }
    if (std::optional<std::string> source_id = ReadJsonStringMember(*sorting, "source_id");
        source_id && !source_id->empty()) {
        state.selected_sample_sort_source_id = std::move(*source_id);
    }
    if (std::optional<std::string> direction = ReadJsonStringMember(*sorting, "direction")) {
        state.selected_sample_sort_direction = ParseSortDirection(*direction);
    }
}

}  // namespace

std::filesystem::path DefaultSampleWorkflowStateCachePath()
{
    return DefaultLocalUserStatePath("sample-workflow-state.json");
}

SampleWorkflowStateCache LoadSampleWorkflowStateCache(const std::filesystem::path& path)
{
    SampleWorkflowStateCache cache;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(path, kStateFormatKind, {kStateSchemaVersion}, "sample workflow state cache");
    if (!result.document) {
        return cache;
    }

    const JsonValue* sources = JsonObjectMember(result.document->root, "sources");
    if (sources == nullptr || sources->kind != JsonValue::Kind::Array) {
        return cache;
    }
    for (const JsonValue& source_object : sources->array) {
        if (source_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        std::optional<std::string> identity = ReadJsonStringMember(source_object, "identity");
        if (!identity || identity->empty()) {
            continue;
        }

        SampleWorkflowSourceState state;
        state.filter_conditions = ParseFilterConditions(source_object);
        state.selected_filter_source_ids =
            ParseStringArrayMember(source_object, "selected_filter_source_ids");
        state.selected_sample_sort_source_ids =
            ParseStringArrayMember(source_object, "selected_sample_sort_source_ids");
        ParseSortState(source_object, state);
        if (HasState(state)) {
            cache.sources_by_identity.emplace(std::move(*identity), std::move(state));
        }
    }
    return cache;
}

bool SaveSampleWorkflowStateCache(
    const std::filesystem::path& path,
    const SampleWorkflowStateCache& cache)
{
    if (path.empty()) {
        return true;
    }

    std::vector<std::string> keys;
    keys.reserve(cache.sources_by_identity.size());
    for (const auto& [identity, state] : cache.sources_by_identity) {
        if (HasState(state)) {
            keys.push_back(identity);
        }
    }
    std::sort(keys.begin(), keys.end());

    return WriteVersionedJsonCacheFile(
        path,
        kStateFormatKind,
        kStateSchemaVersion,
        "sample workflow state cache",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            stream << "  \"sources\": [";
            if (!keys.empty()) {
                stream << "\n";
            }
            for (std::size_t index = 0; index < keys.size(); ++index) {
                const std::string& identity = keys[index];
                const SampleWorkflowSourceState& state = cache.sources_by_identity.at(identity);
                stream << "    {\n";
                stream << "      \"identity\": ";
                WriteJsonString(stream, identity);
                bool wrote_member = true;
                WriteFilterConditions(stream, state.filter_conditions, wrote_member);
                WriteStringArrayMember(
                    stream,
                    "selected_filter_source_ids",
                    state.selected_filter_source_ids,
                    wrote_member);
                WriteStringArrayMember(
                    stream,
                    "selected_sample_sort_source_ids",
                    state.selected_sample_sort_source_ids,
                    wrote_member);
                WriteSortState(stream, state, wrote_member);
                stream << "\n";
                stream << "    }";
                stream << (index + 1 == keys.size() ? "\n" : ",\n");
            }
            if (!keys.empty()) {
                stream << "  ";
            }
            stream << "]\n";
            return true;
        });
}

}  // namespace specforge
