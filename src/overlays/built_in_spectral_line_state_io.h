#pragma once
#include "overlays/spectral_line_session_state.h"
#include "app/local_user_state_json.h"

namespace spectiary {
struct BuiltInSpectralLineStateLoadResult {
    BuiltInSpectralLineState state;
    bool requires_save = false;
    VersionedJsonCacheLoadIssueKind issue_kind = VersionedJsonCacheLoadIssueKind::None;
    std::string error;
};
[[nodiscard]] BuiltInSpectralLineStateLoadResult LoadBuiltInSpectralLineState(
    const std::filesystem::path& path, const SpectralLineList& base);
[[nodiscard]] bool SaveBuiltInSpectralLineState(const std::filesystem::path& path,
    const SpectralLineList& base, const BuiltInSpectralLineState& state, std::string& error,
    AtomicFileWriteCheckpoint before_replace = {});
} // namespace spectiary
