#include "ui/sample_navigation_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"

#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kStateFormatKind = "specforge.sample_navigation_state.cache";
constexpr int kStateSchemaVersion = 1;

}  // namespace

std::filesystem::path DefaultSampleNavigationStateCachePath()
{
    return DefaultLocalUserStatePath("sample-navigation-state.json");
}

SampleNavigationStateCache LoadSampleNavigationStateCache(const std::filesystem::path& path)
{
    SampleNavigationStateCache cache;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(path, kStateFormatKind, {kStateSchemaVersion}, "sample navigation state cache");
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
        const std::optional<std::string> identity = ReadJsonStringMember(source_object, "identity");
        const std::optional<std::size_t> index = ReadJsonSizeMember(source_object, "last_index");
        if (identity && !identity->empty() && index) {
            cache.last_indices_by_source_identity[*identity] = *index;
        }
    }
    return cache;
}

bool SaveSampleNavigationStateCache(
    const std::filesystem::path& path,
    const SampleNavigationStateCache& cache)
{
    if (path.empty()) {
        return false;
    }

    const std::vector<std::string> keys = SortedCacheKeys(cache.last_indices_by_source_identity);

    return WriteVersionedJsonCacheFile(
        path,
        kStateFormatKind,
        kStateSchemaVersion,
        "sample navigation state cache",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            stream << "  \"sources\": [";
            if (!keys.empty()) {
                stream << "\n";
            }
            for (std::size_t index = 0; index < keys.size(); ++index) {
                stream << "    { \"identity\": ";
                WriteJsonString(stream, keys[index]);
                stream << ", \"last_index\": " << cache.last_indices_by_source_identity.at(keys[index]) << " }";
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

