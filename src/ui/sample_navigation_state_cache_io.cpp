#include "ui/sample_navigation_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"
#include "domain/source_collection_identity_digest.h"

#include <algorithm>
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
    return DefaultLocalUserStatePath(
        local_user_state_paths::kSampleNavigationState);
}

SampleNavigationStateCacheLoadResult
LoadSampleNavigationStateCache(const std::filesystem::path& path)
{
    SampleNavigationStateCacheLoadResult load;
    VersionedJsonCacheLoadResult result =
        LoadVersionedJsonCacheFile(path, kStateFormatKind, {kStateSchemaVersion}, "sample navigation state cache");
    if (!result.document) {
        load.warning = std::move(result.warning);
        return load;
    }

    const JsonValue* sources = JsonObjectMember(result.document->root, "sources");
    if (sources == nullptr || sources->kind != JsonValue::Kind::Array) {
        return load;
    }
    for (const JsonValue& source_object : sources->array) {
        if (source_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        const std::optional<std::string> identity = ReadJsonStringMember(source_object, "identity");
        const std::optional<std::size_t> index = ReadJsonSizeMember(source_object, "last_index");
        if (identity && !identity->empty() && index) {
            load.cache.last_indices_by_source_identity[
                NormalizePersistedSourceCollectionIdentity(*identity)] = *index;
        }
    }
    return load;
}

bool SaveSampleNavigationStateCache(
    const std::filesystem::path& path,
    const SampleNavigationStateCache& cache)
{
    if (path.empty()) {
        return false;
    }

    std::vector<std::string> keys;
    keys.reserve(cache.last_indices_by_source_identity.size());
    for (const auto& [identity, index] : cache.last_indices_by_source_identity) {
        (void)index;
        keys.push_back(identity);
    }
    std::sort(keys.begin(), keys.end());

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
                const std::string& identity = keys[index];
                stream << "    { \"identity\": ";
                WriteJsonString(stream, identity);
                const auto last_index = cache.last_indices_by_source_identity.find(identity);
                stream << ", \"last_index\": "
                       << (last_index == cache.last_indices_by_source_identity.end() ? 0 : last_index->second);
                stream << " }";
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
