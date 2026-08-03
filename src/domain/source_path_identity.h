#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace specforge {

// Produces a process-local Windows source identity without probing the target.
// It deliberately uses only absolute/lexical normalization: canonical() and
// equivalent() may block on OneDrive, network shares, or unavailable devices.
[[nodiscard]] std::string SourcePathIdentityKey(const std::filesystem::path& path);

// Produces optional physical Windows identities used to enhance writable
// labeling output coordination. Existing targets use their volume/file ID and
// their parent directory volume/file ID plus normalized leaf. Missing targets
// use the latter only. An empty result means physical probing was unavailable;
// callers must retain SourcePathIdentityKey(path) as the required baseline.
[[nodiscard]] std::vector<std::string>
OutputPathIdentityKeys(
    const std::filesystem::path& path);

// Convenience projection for callers that only compare one identity. A
// physical identity is preferred when available; the normalized path is the
// required fallback when probing the target is unavailable.
[[nodiscard]] std::string OutputPathIdentityKey(
    const std::filesystem::path& path);

}  // namespace specforge
