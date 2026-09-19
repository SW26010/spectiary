#include "ui/source_collection_session_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <ostream>
#include <optional>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kSourceSessionStateFormatKind = "spectiary.source_collection_session.cache";
constexpr int kSourceSessionStateSchemaVersion = 2;
constexpr std::size_t kMaxRestoredSources = 32;

}  // namespace

std::filesystem::path DefaultSourceCollectionSessionStateCachePath(const RuntimePaths& runtime_paths)
{
    return runtime_paths.source_session_state_path;
}

SourceCollectionSessionStateCacheLoadResult
LoadSourceCollectionSessionStateCache(const RuntimePaths& runtime_paths, const std::filesystem::path& path)
{
    SourceCollectionSessionStateCacheLoadResult load;
    VersionedJsonCacheLoadResult result = LoadVersionedJsonCacheFile(
        path,
        kSourceSessionStateFormatKind,
        {1, kSourceSessionStateSchemaVersion},
        "source session state cache");
    if (!result.document) {
        load.warning = std::move(result.warning);
        return load;
    }

    load.cache.active_source_index =
        ReadJsonSizeMember(result.document->root, "active_source_index");
    const nlohmann::json* sources = JsonObjectMember(result.document->root, "sources");
    if (sources == nullptr || sources->type() != nlohmann::json::value_t::array) {
        return load;
    }

    for (const nlohmann::json& source_object : (*sources)) {
        if (source_object.type() != nlohmann::json::value_t::object ||
            load.cache.sources.size() >= kMaxRestoredSources) {
            continue;
        }
        const nlohmann::json* path_value = JsonObjectMember(source_object, "path");
        if (path_value == nullptr) {
            continue;
        }

        std::optional<std::filesystem::path> path_reference = ReadPersistedPathReference(*path_value, runtime_paths);
        if (!path_reference || path_reference->empty()) {
            continue;
        }

        SourceCollectionSavedSource source;
        source.path = std::move(*path_reference);
        source.last_spectrum_index = ReadJsonSizeMember(source_object, "last_index").value_or(0);
        const nlohmann::json* annotation_paths = JsonObjectMember(source_object, "annotation_paths");
        if (annotation_paths != nullptr && annotation_paths->type() == nlohmann::json::value_t::array) {
            for (const nlohmann::json& annotation_path_value : (*annotation_paths)) {
                std::optional<std::filesystem::path> annotation_path =
                    ReadPersistedPathReference(annotation_path_value, runtime_paths);
                if (!annotation_path || annotation_path->empty()) {
                    continue;
                }
                source.annotation_paths.push_back(std::move(*annotation_path));
            }
        }
        load.cache.sources.push_back(std::move(source));
    }
    return load;
}

bool SaveSourceCollectionSessionStateCache(
    const RuntimePaths& runtime_paths,
    const std::filesystem::path& path,
    const SourceCollectionSessionStateCache& cache)
{
    if (path.empty()) {
        return true;
    }

    return WriteVersionedJsonCacheFile(
        path,
        kSourceSessionStateFormatKind,
        kSourceSessionStateSchemaVersion,
        "source session state cache",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            stream << "  \"active_source_index\": ";
            if (cache.active_source_index) {
                stream << *cache.active_source_index;
            } else {
                stream << "null";
            }
            stream << ",\n";
            stream << "  \"sources\": [";
            if (!cache.sources.empty()) {
                stream << "\n";
            }
            for (std::size_t index = 0; index < cache.sources.size(); ++index) {
                stream << "    { \"path\": ";
                WritePersistedPathReference(stream, cache.sources[index].path, runtime_paths);
                stream << ", \"last_index\": " << cache.sources[index].last_spectrum_index;
                if (!cache.sources[index].annotation_paths.empty()) {
                    stream << ", \"annotation_paths\": [";
                    for (std::size_t path_index = 0;
                         path_index < cache.sources[index].annotation_paths.size();
                         ++path_index) {
                        if (path_index > 0) {
                            stream << ", ";
                        }
                        WritePersistedPathReference(stream, cache.sources[index].annotation_paths[path_index], runtime_paths);
                    }
                    stream << "]";
                }
                stream << " }";
                stream << (index + 1 == cache.sources.size() ? "\n" : ",\n");
            }
            if (!cache.sources.empty()) {
                stream << "  ";
            }
            stream << "]\n";
            return true;
        });
}

}  // namespace specforge
