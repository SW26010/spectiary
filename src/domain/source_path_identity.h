#pragma once

#include <filesystem>
#include <string>

namespace specforge {

// Produces a process-local Windows source identity without probing the target.
// It deliberately uses only absolute/lexical normalization: canonical() and
// equivalent() may block on OneDrive, network shares, or unavailable devices.
[[nodiscard]] std::string SourcePathIdentityKey(const std::filesystem::path& path);

}  // namespace specforge
