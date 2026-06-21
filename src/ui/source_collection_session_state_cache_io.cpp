#include "ui/source_collection_session_state_cache_io.h"

#include "app/local_user_state.h"
#include "app/local_user_state_json.h"

#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace specforge {
namespace {

constexpr const char* kSourceSessionStateFormatKind = "specforge.source_collection_session.cache";
constexpr int kSourceSessionStateSchemaVersion = 1;
constexpr std::size_t kMaxRestoredSources = 32;

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

}  // namespace

std::filesystem::path DefaultSourceCollectionSessionStateCachePath()
{
    return DefaultLocalUserStatePath("source-session.json");
}

SourceCollectionSessionStateCache LoadSourceCollectionSessionStateCache(const std::filesystem::path& path)
{
    SourceCollectionSessionStateCache state;
    VersionedJsonCacheLoadResult result = LoadVersionedJsonCacheFile(
        path,
        kSourceSessionStateFormatKind,
        {kSourceSessionStateSchemaVersion},
        "source session state cache");
    if (!result.document) {
        return state;
    }

    state.active_source_index = ReadJsonSizeMember(result.document->root, "active_source_index");
    const JsonValue* sources = JsonObjectMember(result.document->root, "sources");
    if (sources == nullptr || sources->kind != JsonValue::Kind::Array) {
        return state;
    }

    for (const JsonValue& source_object : sources->array) {
        if (source_object.kind != JsonValue::Kind::Object || state.sources.size() >= kMaxRestoredSources) {
            continue;
        }
        const std::optional<std::string> path_text = ReadJsonStringMember(source_object, "path");
        if (!path_text || path_text->empty()) {
            continue;
        }

        SourceCollectionSavedSource source;
        source.path = PathFromUtf8(*path_text);
        source.last_spectrum_index = ReadJsonSizeMember(source_object, "last_index").value_or(0);
        state.sources.push_back(std::move(source));
    }
    return state;
}

bool SaveSourceCollectionSessionStateCache(
    const std::filesystem::path& path,
    const std::vector<SourceCollectionSavedSource>& sources,
    std::optional<std::size_t> active_source_index)
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
            if (active_source_index) {
                stream << *active_source_index;
            } else {
                stream << "null";
            }
            stream << ",\n";
            stream << "  \"sources\": [";
            if (!sources.empty()) {
                stream << "\n";
            }
            for (std::size_t index = 0; index < sources.size(); ++index) {
                stream << "    { \"path\": ";
                WriteJsonString(stream, PathToUtf8(sources[index].path));
                stream << ", \"last_index\": " << sources[index].last_spectrum_index << " }";
                stream << (index + 1 == sources.size() ? "\n" : ",\n");
            }
            if (!sources.empty()) {
                stream << "  ";
            }
            stream << "]\n";
            return true;
        });
}

}  // namespace specforge
