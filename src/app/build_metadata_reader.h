#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace specforge {

enum class BuildMetadataStatus {
    Available,
    Unavailable,
    Mismatch,
};

struct BuildIdentity {
    std::string specforge_version;
    std::string release_profile;
    std::string configuration;
    std::string target_architecture;
    std::string source_mode;
    std::string source_revision;
};

struct BuildMetadata {
    std::string compiler_id;
    std::string compiler_version;
    std::string cmake_version;
    std::string generator;
    std::optional<std::string> windows_sdk_version;
    std::string dear_imgui_version;
    std::string implot_version;
    std::string zlib_version;
};

struct BuildMetadataReadResult {
    BuildMetadataStatus status = BuildMetadataStatus::Unavailable;
    std::optional<BuildMetadata> metadata;
};

[[nodiscard]] BuildMetadataReadResult ReadBuildMetadata(
    const std::filesystem::path& path,
    const BuildIdentity& expected_identity);

[[nodiscard]] const BuildMetadataReadResult& DefaultBuildMetadata();

}  // namespace specforge
