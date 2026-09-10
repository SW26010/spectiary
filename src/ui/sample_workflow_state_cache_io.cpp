#include "ui/sample_workflow_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"
#include "domain/source_collection_identity_digest.h"

#include <algorithm>
#include <filesystem>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kStateFormatKind = "specforge.sample_workflow_state.cache";
constexpr int kStateSchemaVersion = 2;
constexpr std::string_view kAnnotationSourcePrefix = "annotation:";
constexpr std::string_view kSourceKindAnnotationPath = "annotation_path";

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path PathFromUtf8(std::string_view value)
{
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const char character : value) {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path(utf8);
}

std::string AnnotationSourceIdFromPath(const std::filesystem::path& path)
{
    return path.empty() ? std::string{} : std::string{kAnnotationSourcePrefix} + PathToUtf8(path);
}

std::optional<std::filesystem::path> AnnotationPathFromSourceId(std::string_view source_id)
{
    if (source_id.size() <= kAnnotationSourcePrefix.size() ||
        source_id.substr(0, kAnnotationSourcePrefix.size()) != kAnnotationSourcePrefix) {
        return std::nullopt;
    }

    std::filesystem::path path = PathFromUtf8(source_id.substr(kAnnotationSourcePrefix.size()));
    if (!path.is_absolute()) {
        return std::nullopt;
    }
    return path;
}

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
           !state.annotation_display_names.empty() ||
           (state.selected_sample_sort_source_id && !state.selected_sample_sort_source_id->empty()) ||
           state.selected_sample_sort_direction != SampleNavigationSortDirection::Ascending;
}

void WritePersistedWorkflowSourceId(std::ostream& stream, std::string_view source_id)
{
    if (std::optional<std::filesystem::path> annotation_path = AnnotationPathFromSourceId(source_id)) {
        stream << "{ \"source_kind\": ";
        WriteJsonString(stream, kSourceKindAnnotationPath);
        stream << ", \"path\": ";
        WritePersistedPathReference(stream, *annotation_path);
        stream << " }";
        return;
    }

    WriteJsonString(stream, source_id);
}

std::optional<std::string> ReadPersistedWorkflowSourceId(const nlohmann::json& value)
{
    if (value.type() == nlohmann::json::value_t::string) {
        return value.get_ref<const std::string&>().empty() ? std::nullopt : std::optional<std::string>{value.get_ref<const std::string&>()};
    }
    if (value.type() != nlohmann::json::value_t::object) {
        return std::nullopt;
    }

    const std::optional<std::string> source_kind = ReadJsonStringMember(value, "source_kind");
    const nlohmann::json* path = JsonObjectMember(value, "path");
    if (!source_kind || *source_kind != kSourceKindAnnotationPath || path == nullptr) {
        return std::nullopt;
    }

    std::optional<std::filesystem::path> annotation_path = ReadPersistedPathReference(*path);
    if (!annotation_path || annotation_path->empty()) {
        return std::nullopt;
    }
    return AnnotationSourceIdFromPath(*annotation_path);
}

void WriteSourceIdArrayMember(
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
        WritePersistedWorkflowSourceId(stream, values[index]);
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
        WritePersistedWorkflowSourceId(stream, condition.source_id);
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
        WritePersistedWorkflowSourceId(stream, *state.selected_sample_sort_source_id);
    }
    stream << " }";
    wrote_member = true;
}

void WriteAnnotationDisplayNames(
    std::ostream& stream,
    std::vector<SampleAnnotationDisplayNameOverride> display_names,
    bool& wrote_member)
{
    display_names.erase(
        std::remove_if(
            display_names.begin(),
            display_names.end(),
            [](const SampleAnnotationDisplayNameOverride& display_name) {
                return display_name.source_id.empty() || display_name.display_name.empty();
            }),
        display_names.end());
    if (display_names.empty()) {
        return;
    }
    std::sort(
        display_names.begin(),
        display_names.end(),
        [](const SampleAnnotationDisplayNameOverride& left, const SampleAnnotationDisplayNameOverride& right) {
            return left.source_id < right.source_id;
        });

    if (wrote_member) {
        stream << ",\n";
    }
    stream << "      \"annotation_display_names\": [";
    for (std::size_t index = 0; index < display_names.size(); ++index) {
        const SampleAnnotationDisplayNameOverride& display_name = display_names[index];
        if (index > 0) {
            stream << ",";
        }
        stream << "\n";
        stream << "        { \"source_id\": ";
        WritePersistedWorkflowSourceId(stream, display_name.source_id);
        stream << ", \"display_name\": ";
        WriteJsonString(stream, display_name.display_name);
        stream << " }";
    }
    stream << "\n      ]";
    wrote_member = true;
}

std::vector<SampleFilterCondition> ParseFilterConditions(const nlohmann::json& source_object)
{
    std::vector<SampleFilterCondition> conditions;
    const nlohmann::json* filters = JsonObjectMember(source_object, "filters");
    if (filters == nullptr || filters->type() != nlohmann::json::value_t::array) {
        return conditions;
    }

    for (const nlohmann::json& condition_object : (*filters)) {
        if (condition_object.type() != nlohmann::json::value_t::object) {
            continue;
        }
        const nlohmann::json* source_id_value = JsonObjectMember(condition_object, "source_id");
        std::optional<std::string> source_id =
            source_id_value == nullptr ? std::nullopt : ReadPersistedWorkflowSourceId(*source_id_value);
        if (!source_id || source_id->empty()) {
            continue;
        }
        const nlohmann::json* values = JsonObjectMember(condition_object, "allowed_values");
        if (values == nullptr || values->type() != nlohmann::json::value_t::array) {
            continue;
        }

        std::unordered_set<std::string> allowed_values;
        for (const nlohmann::json& value : (*values)) {
            if (value.type() == nlohmann::json::value_t::string && !value.get_ref<const std::string&>().empty()) {
                allowed_values.insert(value.get_ref<const std::string&>());
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

std::vector<std::string> ParseSourceIdArrayMember(const nlohmann::json& source_object, const char* name)
{
    std::vector<std::string> values;
    const nlohmann::json* array = JsonObjectMember(source_object, name);
    if (array == nullptr || array->type() != nlohmann::json::value_t::array) {
        return values;
    }
    for (const nlohmann::json& value : (*array)) {
        if (std::optional<std::string> source_id = ReadPersistedWorkflowSourceId(value)) {
            values.push_back(std::move(*source_id));
        }
    }
    return UniqueStrings(std::move(values));
}

std::vector<SampleAnnotationDisplayNameOverride> ParseAnnotationDisplayNames(const nlohmann::json& source_object)
{
    std::vector<SampleAnnotationDisplayNameOverride> display_names;
    const nlohmann::json* array = JsonObjectMember(source_object, "annotation_display_names");
    if (array == nullptr || array->type() != nlohmann::json::value_t::array) {
        return display_names;
    }

    std::unordered_set<std::string> seen;
    for (const nlohmann::json& value : (*array)) {
        if (value.type() != nlohmann::json::value_t::object) {
            continue;
        }
        const nlohmann::json* source_id_value = JsonObjectMember(value, "source_id");
        std::optional<std::string> source_id =
            source_id_value == nullptr ? std::nullopt : ReadPersistedWorkflowSourceId(*source_id_value);
        std::optional<std::string> display_name = ReadJsonStringMember(value, "display_name");
        if (!source_id || source_id->empty() || !display_name || display_name->empty() ||
            !seen.emplace(*source_id).second) {
            continue;
        }
        display_names.push_back(
            SampleAnnotationDisplayNameOverride{std::move(*source_id), std::move(*display_name)});
    }
    return display_names;
}

void ParseSortState(const nlohmann::json& source_object, SampleWorkflowSourceState& state)
{
    const nlohmann::json* sorting = JsonObjectMember(source_object, "sorting");
    if (sorting == nullptr || sorting->type() != nlohmann::json::value_t::object) {
        return;
    }
    const nlohmann::json* source_id_value = JsonObjectMember(*sorting, "source_id");
    if (std::optional<std::string> source_id =
            source_id_value == nullptr ? std::nullopt : ReadPersistedWorkflowSourceId(*source_id_value);
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
    return DefaultLocalUserStatePath(
        local_user_state_paths::kSampleWorkflowState);
}

SampleWorkflowStateCacheLoadResult LoadSampleWorkflowStateCache(
    const std::filesystem::path& path,
    const std::function<void()>& cancellation_checkpoint)
{
    SampleWorkflowStateCacheLoadResult load;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(
            path,
            kStateFormatKind,
            {1, kStateSchemaVersion},
            "sample workflow state cache",
            cancellation_checkpoint);
    if (!result.document) {
        load.warning = std::move(result.warning);
        return load;
    }

    const nlohmann::json* sources = JsonObjectMember(result.document->root, "sources");
    if (sources == nullptr || sources->type() != nlohmann::json::value_t::array) {
        return load;
    }
    for (const nlohmann::json& source_object : (*sources)) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        if (source_object.type() != nlohmann::json::value_t::object) {
            continue;
        }
        std::optional<std::string> identity = ReadJsonStringMember(source_object, "identity");
        if (!identity || identity->empty()) {
            continue;
        }

        SampleWorkflowSourceState state;
        state.filter_conditions = ParseFilterConditions(source_object);
        state.selected_filter_source_ids =
            ParseSourceIdArrayMember(source_object, "selected_filter_source_ids");
        state.selected_sample_sort_source_ids =
            ParseSourceIdArrayMember(source_object, "selected_sample_sort_source_ids");
        state.annotation_display_names = ParseAnnotationDisplayNames(source_object);
        ParseSortState(source_object, state);
        if (HasState(state)) {
            load.cache.sources_by_identity.emplace(
                NormalizePersistedSourceCollectionIdentity(std::move(*identity)),
                std::move(state));
        }
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    return load;
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
                WriteSourceIdArrayMember(
                    stream,
                    "selected_filter_source_ids",
                    state.selected_filter_source_ids,
                    wrote_member);
                WriteSourceIdArrayMember(
                    stream,
                    "selected_sample_sort_source_ids",
                    state.selected_sample_sort_source_ids,
                    wrote_member);
                WriteSortState(stream, state, wrote_member);
                WriteAnnotationDisplayNames(stream, state.annotation_display_names, wrote_member);
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
