#pragma once

#include "overlays/spectral_line_user_state.h"
#include "platform/atomic_file.h"

#include <filesystem>
#include <functional>
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
    int schema_version = 0;
    CatalogUserStateCacheLoadIssueKind issue_kind =
        CatalogUserStateCacheLoadIssueKind::None;
    std::string diagnostic_detail;
};

[[nodiscard]] std::filesystem::path DefaultCatalogUserStateCachePath();
// The lease is held only while a task reloads, reconciles, validates, and
// atomically replaces this cache.  It is separate from the cache file so the
// normal atomic writer remains usable by migration and unit-test callers.
[[nodiscard]] std::filesystem::path CatalogUserStateCacheCommitLeasePath(
    const std::filesystem::path& path);
[[nodiscard]] CatalogUserStateCacheLoadResult LoadCatalogUserStateCache(const std::filesystem::path& path);
bool SaveCatalogUserStateCache(
    const std::filesystem::path& path,
    const CatalogUserStateCache& cache,
    std::string& error);

// Narrow process-local seam used by the interruption-recovery regression. It
// is empty for normal application code and is never needed for production
// persistence behavior.
using CatalogUserStateCacheBeforeReplaceHook = AtomicFileWriteCheckpoint;
void SetCatalogUserStateCacheBeforeReplaceHookForTests(
    CatalogUserStateCacheBeforeReplaceHook hook);

}  // namespace specforge

