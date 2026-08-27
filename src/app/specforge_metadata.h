#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace specforge {

namespace metadata_contract {

inline constexpr int kSchema5Version = 5;
inline constexpr std::string_view kCanonicalExecutableFileName =
    "SpecForge.exe";
inline constexpr std::string_view kCanonicalMetadataFileName =
    "specforge_metadata.json";

}  // namespace metadata_contract

enum class Distribution {
    Standalone,
    Installer,
    WinGet,
    Portable,
    Scoop,
};

enum class StorageProfile {
    Portable,
    LocalAppData,
};

struct DeploymentMetadata {
    Distribution distribution = Distribution::Standalone;
    StorageProfile storage_profile = StorageProfile::LocalAppData;
};

enum class BuildMetadataStatus {
    Available,
    Unavailable,
    Mismatch,
};

struct BuildIdentity {
    std::string product_name;
    std::string specforge_version;
    std::string configuration;
    std::string target_architecture;
    std::string source_mode;
    std::string source_revision;
};

struct BuildArtifactMetadata {
    std::string file;
    std::string sha256;
};

struct FinalizedArtifactMetadata {
    std::string completed_at_utc;
    BuildArtifactMetadata artifact;
};

struct BuildMetadata {
    std::string compiler_id;
    std::string compiler_version;
    std::string cmake_version;
    std::string generator;
    std::optional<std::string> windows_sdk_version;
    std::string dear_imgui_version;
    std::string implot_version;
    std::string yaml_cpp_version;
    std::string zlib_version;
    std::optional<FinalizedArtifactMetadata> finalized_artifact;
};

struct BuildMetadataReadResult {
    BuildMetadataStatus status = BuildMetadataStatus::Unavailable;
    std::optional<BuildMetadata> metadata;
};

struct SpecForgeMetadataReadResult {
    DeploymentMetadata deployment;
    BuildMetadataReadResult build_metadata;
    std::filesystem::path metadata_path;
    std::optional<std::string> startup_error;
};

[[nodiscard]] const char* DistributionName(Distribution distribution);
[[nodiscard]] const char* StorageProfileName(StorageProfile profile);

[[nodiscard]] SpecForgeMetadataReadResult ReadSpecForgeMetadata(
    const std::filesystem::path& path,
    const BuildIdentity& expected_identity);

[[nodiscard]] SpecForgeMetadataReadResult ReadAdjacentSpecForgeMetadata(
    const std::filesystem::path& package_root,
    const BuildIdentity& expected_identity);

[[nodiscard]] BuildIdentity CompiledBuildIdentity();

}  // namespace specforge
