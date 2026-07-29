#pragma once

#include "overlays/spectral_line_user_state.h"

#include <filesystem>
#include <string>

namespace specforge {

enum class CatalogUserStateCacheLoadIssueKind {
    None,
    ReadFailed,
    InvalidDocument,
    UnsupportedFormatOrSchema,
};

struct CatalogUserStateCacheLoadResult {
    CatalogUserStateCache cache;
    std::string warning;
    bool requires_save = false;
    CatalogUserStateCacheLoadIssueKind issue_kind =
        CatalogUserStateCacheLoadIssueKind::None;
    std::string diagnostic_detail;
};

[[nodiscard]] std::filesystem::path DefaultCatalogUserStateCachePath();
[[nodiscard]] CatalogUserStateCacheLoadResult LoadCatalogUserStateCache(const std::filesystem::path& path);
bool SaveCatalogUserStateCache(
    const std::filesystem::path& path,
    const CatalogUserStateCache& cache,
    std::string& error);

}  // namespace specforge

