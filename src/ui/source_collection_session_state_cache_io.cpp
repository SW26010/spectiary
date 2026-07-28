#include "ui/source_collection_session_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"

#include <ostream>
#include <optional>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kSourceSessionStateFormatKind = "specforge.source_collection_session.cache";
constexpr int kSourceSessionStateSchemaVersion = 2;
constexpr std::size_t kMaxRestoredSources = 32;

}  // namespace

std::filesystem::path DefaultSourceCollectionSessionStateCachePath()
{
    return DefaultLocalUserStatePath(
        local_user_state_paths::kSourceSessionState);
}

SourceCollectionSessionStateCacheLoadResult
LoadSourceCollectionSessionStateCache(const std::filesystem::path& path)
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
    const JsonValue* sources = JsonObjectMember(result.document->root, "sources");
    if (sources == nullptr || sources->kind != JsonValue::Kind::Array) {
        return load;
    }

    for (const JsonValue& source_object : sources->array) {
        if (source_object.kind != JsonValue::Kind::Object ||
            load.cache.sources.size() >= kMaxRestoredSources) {
            continue;
        }
        const JsonValue* path_value = JsonObjectMember(source_object, "path");
        if (path_value == nullptr) {
            continue;
        }

        std::optional<std::filesystem::path> path_reference = ReadPersistedPathReference(*path_value);
        if (!path_reference || path_reference->empty()) {
            continue;
        }

        SourceCollectionSavedSource source;
        source.path = std::move(*path_reference);
        source.last_spectrum_index = ReadJsonSizeMember(source_object, "last_index").value_or(0);
        const JsonValue* annotation_paths = JsonObjectMember(source_object, "annotation_paths");
        if (annotation_paths != nullptr && annotation_paths->kind == JsonValue::Kind::Array) {
            for (const JsonValue& annotation_path_value : annotation_paths->array) {
                std::optional<std::filesystem::path> annotation_path =
                    ReadPersistedPathReference(annotation_path_value);
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
                WritePersistedPathReference(stream, cache.sources[index].path);
                stream << ", \"last_index\": " << cache.sources[index].last_spectrum_index;
                if (!cache.sources[index].annotation_paths.empty()) {
                    stream << ", \"annotation_paths\": [";
                    for (std::size_t path_index = 0;
                         path_index < cache.sources[index].annotation_paths.size();
                         ++path_index) {
                        if (path_index > 0) {
                            stream << ", ";
                        }
                        WritePersistedPathReference(stream, cache.sources[index].annotation_paths[path_index]);
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
