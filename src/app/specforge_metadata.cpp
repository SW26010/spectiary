#include "app/specforge_metadata.h"

#include "app/local_user_state_json.h"
#include "app/specforge_metadata_validation.h"
#include "specforge/specforge_build_identity.h"

#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

std::optional<std::string> ReadRequiredMetadataString(
    const nlohmann::json& root,
    std::string_view key)
{
    std::optional<std::string> value =
        ReadJsonStringMember(root, key);
    if (!value ||
        !metadata_validation::IsRequiredMetadataString(*value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::optional<std::string>> ReadNullableStringMember(
    const nlohmann::json& root,
    std::string_view key)
{
    const nlohmann::json* member = JsonObjectMember(root, key);
    if (member == nullptr) {
        return std::nullopt;
    }
    if (member->type() == nlohmann::json::value_t::null) {
        return std::optional<std::string>{};
    }
    if (member->type() != nlohmann::json::value_t::string) {
        return std::nullopt;
    }
    if (!metadata_validation::IsRequiredMetadataString(
            member->get_ref<const std::string&>())) {
        return std::nullopt;
    }
    return member->get_ref<const std::string&>();
}

bool MatchesSourceRevision(
    const std::optional<std::string>& actual_revision,
    const BuildIdentity& expected_identity)
{
    if (expected_identity.source_mode == "working_tree") {
        return expected_identity.source_revision.empty() &&
            !actual_revision;
    }
    return actual_revision &&
        *actual_revision == expected_identity.source_revision;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string MetadataError(
    const std::filesystem::path& path,
    std::string_view detail)
{
    return "Invalid SpecForge metadata '" + PathToUtf8(path) +
        "': " + std::string(detail);
}

BuildMetadataReadResult ReadBuildMetadata(
    const nlohmann::json& root,
    int schema_version,
    const BuildIdentity& expected_identity)
{
    const nlohmann::json* product = JsonObjectMember(root, "product");
    const nlohmann::json* build = JsonObjectMember(root, "build");
    if (product == nullptr || !product->is_object() ||
        build == nullptr || !build->is_object()) {
        return {};
    }
    const auto application_id = ReadRequiredMetadataString(root, "application_id");
    const auto product_name = ReadRequiredMetadataString(*product, "name");
    const auto specforge_version = ReadRequiredMetadataString(*product, "version");
    const std::optional<std::string> configuration =
        ReadRequiredMetadataString(*build, "configuration");
    const std::optional<std::string> target_architecture =
        ReadRequiredMetadataString(*build, "target_architecture");
    const std::optional<std::string> source_mode =
        ReadRequiredMetadataString(*build, "source_mode");
    const std::optional<std::optional<std::string>> source_revision =
        ReadNullableStringMember(*build, "source_revision");

    BuildMetadata metadata;
    const std::optional<std::string> compiler_id =
        ReadRequiredMetadataString(*build, "compiler_id");
    const std::optional<std::string> compiler_version =
        ReadRequiredMetadataString(*build, "compiler_version");
    const std::optional<std::string> cmake_version =
        ReadRequiredMetadataString(*build, "cmake_version");
    const std::optional<std::string> generator =
        ReadRequiredMetadataString(*build, "generator");
    const std::optional<std::optional<std::string>> windows_sdk_version =
        ReadNullableStringMember(*build, "windows_sdk_version");
    const std::optional<std::string> dear_imgui =
        ReadRequiredMetadataString(*build, "dear_imgui");
    const std::optional<std::string> implot =
        ReadRequiredMetadataString(*build, "implot");
    const auto cfitsio = ReadRequiredMetadataString(*build, "cfitsio");
    const auto yaml_cpp = ReadRequiredMetadataString(*build, "yaml_cpp");
    const std::optional<std::string> zlib =
        ReadRequiredMetadataString(*build, "zlib");

    std::optional<FinalizedArtifactMetadata> finalized_artifact;
    if (schema_version == metadata_contract::kSchema6Version) {
        const std::optional<std::string> completed_at_utc =
            ReadRequiredMetadataString(*build, "completed_at_utc");
        const nlohmann::json* artifact_value =
            JsonObjectMember(root, "artifact");
        const std::optional<std::string> artifact_file =
            artifact_value != nullptr &&
                artifact_value->type() == nlohmann::json::value_t::object
            ? ReadRequiredMetadataString(*artifact_value, "file")
            : std::nullopt;
        const std::optional<std::string> artifact_sha256 =
            artifact_value != nullptr &&
                artifact_value->type() == nlohmann::json::value_t::object
            ? ReadRequiredMetadataString(*artifact_value, "sha256")
            : std::nullopt;
        if (completed_at_utc && artifact_file && artifact_sha256) {
            finalized_artifact = FinalizedArtifactMetadata{
                .completed_at_utc = *completed_at_utc,
                .artifact = BuildArtifactMetadata{
                    .file = *artifact_file,
                    .sha256 = *artifact_sha256,
                },
            };
        }
    }

    if (!application_id || !product_name || !specforge_version || !configuration ||
        !target_architecture || !source_mode || !source_revision ||
        !compiler_id || !compiler_version || !cmake_version || !generator ||
        !windows_sdk_version || !dear_imgui || !implot || !cfitsio ||
        !yaml_cpp || !zlib) {
        return {};
    }
    metadata.compiler_id = *compiler_id;
    metadata.compiler_version = *compiler_version;
    metadata.cmake_version = *cmake_version;
    metadata.generator = *generator;
    metadata.windows_sdk_version = *windows_sdk_version;
    metadata.dear_imgui_version = *dear_imgui;
    metadata.implot_version = *implot;
    metadata.cfitsio_version = *cfitsio;
    metadata.yaml_cpp_version = *yaml_cpp;
    metadata.zlib_version = *zlib;
    metadata.finalized_artifact = std::move(finalized_artifact);

    if (schema_version == metadata_contract::kSchema6Version) {
        const BuildIdentity actual_identity = {
            .application_id = *application_id,
            .product_name = *product_name,
            .specforge_version = *specforge_version,
            .configuration = *configuration,
            .target_architecture = *target_architecture,
            .source_mode = *source_mode,
            .source_revision = *source_revision
                ? **source_revision
                : "",
        };
        if (!metadata_validation::ValidateSchema6BuildMetadata(
                actual_identity,
                metadata)) {
            return {};
        }
    }

    if (*application_id != expected_identity.application_id ||
        *specforge_version != expected_identity.specforge_version ||
        *configuration != expected_identity.configuration ||
        *target_architecture != expected_identity.target_architecture ||
        *source_mode != expected_identity.source_mode ||
        !MatchesSourceRevision(*source_revision, expected_identity)) {
        return {
            .status = BuildMetadataStatus::Mismatch,
            .metadata = std::nullopt,
        };
    }

    return {
        .status = BuildMetadataStatus::Available,
        .metadata = std::move(metadata),
    };
}

std::optional<std::string> ReadDeployment(
    const nlohmann::json& root,
    DeploymentMetadata& deployment)
{
    const nlohmann::json* deployment_value =
        JsonObjectMember(root, "deployment");
    if (deployment_value == nullptr) {
        return std::nullopt;
    }
    if (deployment_value->type() != nlohmann::json::value_t::object) {
        return "deployment must be an object";
    }

    const std::optional<std::string> distribution =
        ReadRequiredMetadataString(*deployment_value, "distribution");
    const std::optional<std::string> storage_profile =
        ReadRequiredMetadataString(*deployment_value, "storage_profile");
    if (!distribution) {
        return "deployment.distribution must be a non-empty string";
    }
    if (!storage_profile) {
        return "deployment.storage_profile must be a non-empty string";
    }

    if (*distribution == "installer") {
        deployment.distribution = Distribution::Installer;
    } else if (*distribution == "winget") {
        deployment.distribution = Distribution::WinGet;
    } else if (*distribution == "portable") {
        deployment.distribution = Distribution::Portable;
    } else if (*distribution == "scoop") {
        deployment.distribution = Distribution::Scoop;
    } else {
        return "deployment.distribution must be exactly installer, winget, portable, or scoop";
    }

    if (*storage_profile == "portable") {
        deployment.storage_profile = StorageProfile::Portable;
    } else if (*storage_profile == "local_app_data") {
        deployment.storage_profile = StorageProfile::LocalAppData;
    } else {
        return "deployment.storage_profile must be exactly portable or local_app_data";
    }
    return std::nullopt;
}

SpecForgeMetadataReadResult MissingMetadataResult()
{
    return {};
}

bool PathExists(
    const std::filesystem::path& path,
    std::optional<std::string>& error)
{
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        error = MetadataError(
            path,
            "could not determine whether the file exists: " +
                exists_error.message());
        return false;
    }
    return exists;
}

}  // namespace

const char* DistributionName(Distribution distribution)
{
    switch (distribution) {
    case Distribution::Standalone:
        return "Standalone";
    case Distribution::Installer:
        return "Installer";
    case Distribution::WinGet:
        return "WinGet";
    case Distribution::Portable:
        return "Portable";
    case Distribution::Scoop:
        return "Scoop";
    }
    return "Standalone";
}

const char* StorageProfileName(StorageProfile profile)
{
    switch (profile) {
    case StorageProfile::Portable:
        return "portable";
    case StorageProfile::LocalAppData:
        return "local_app_data";
    }
    return "local_app_data";
}

namespace {

SpecForgeMetadataReadResult ReadSpecForgeMetadataForSchema(
    const std::filesystem::path& path,
    const BuildIdentity& expected_identity)
{
    SpecForgeMetadataReadResult result;
    result.metadata_path = path;

    std::optional<std::string> exists_error;
    if (!PathExists(path, exists_error)) {
        result.startup_error = std::move(exists_error);
        return result;
    }

    std::ifstream stream(path);
    if (!stream.good()) {
        result.startup_error =
            MetadataError(path, "the file could not be opened");
        return result;
    }

    std::string contents;
    if (!ReadTextStreamCancelable(stream, contents)) {
        result.startup_error =
            MetadataError(path, "the file could not be read completely");
        return result;
    }

    std::string parse_error;
    std::optional<nlohmann::json> root = ParseJson(contents, parse_error);
    if (!root || root->type() != nlohmann::json::value_t::object) {
        result.startup_error = MetadataError(
            path,
            parse_error.empty()
                ? "the document root must be an object"
                : "invalid JSON: " + parse_error);
        return result;
    }

    const std::optional<int> schema_version =
        ReadJsonIntMember(*root, "schema_version");
    if (!schema_version || *schema_version != metadata_contract::kSchema6Version) {
        result.startup_error = MetadataError(path, "schema_version must be exactly 6");
        return result;
    }

    if (const std::optional<std::string> deployment_error =
            ReadDeployment(*root, result.deployment)) {
        result.startup_error = MetadataError(path, *deployment_error);
    }
    result.build_metadata =
        ReadBuildMetadata(*root, *schema_version, expected_identity);
    return result;
}

}  // namespace

SpecForgeMetadataReadResult ReadSpecForgeMetadata(
    const std::filesystem::path& path,
    const BuildIdentity& expected_identity)
{
    return ReadSpecForgeMetadataForSchema(
        path,
        expected_identity);
}

SpecForgeMetadataReadResult ReadAdjacentSpecForgeMetadata(
    const std::filesystem::path& package_root,
    const BuildIdentity& expected_identity)
{
    const std::filesystem::path current_path =
        package_root /
        std::filesystem::path(std::string(
            metadata_contract::kCanonicalMetadataFileName));
    std::optional<std::string> exists_error;
    if (PathExists(current_path, exists_error)) {
        return ReadSpecForgeMetadataForSchema(
            current_path,
            expected_identity);
    }
    if (exists_error) {
        SpecForgeMetadataReadResult result;
        result.metadata_path = current_path;
        result.startup_error = std::move(exists_error);
        return result;
    }

    return MissingMetadataResult();
}

BuildIdentity CompiledBuildIdentity()
{
    return {
        .product_name = project_identity::kProductDisplayName,
        .specforge_version = build_info::kSpecForgeVersion,
        .configuration = build_info::kBuildConfiguration,
        .target_architecture =
            build_info::kTargetArchitecture,
        .source_mode = build_info::kBuildSourceMode,
        .source_revision =
            build_info::kBuildSourceRevision,
    };
}

}  // namespace specforge
