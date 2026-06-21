#pragma once

#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace specforge {

struct SourceCollectionSessionStateCache {
    std::vector<SourceCollectionSavedSource> sources;
    std::optional<std::size_t> active_source_index;
};

[[nodiscard]] std::filesystem::path DefaultSourceCollectionSessionStateCachePath();

[[nodiscard]] SourceCollectionSessionStateCache LoadSourceCollectionSessionStateCache(
    const std::filesystem::path& path);

[[nodiscard]] bool SaveSourceCollectionSessionStateCache(
    const std::filesystem::path& path,
    const std::vector<SourceCollectionSavedSource>& sources,
    std::optional<std::size_t> active_source_index);

}  // namespace specforge
