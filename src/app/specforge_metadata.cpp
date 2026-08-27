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

constexpr int kLegacySchemaVersion = 3;
constexpr int kSchema4Version = 4;
constexpr std::string_view kLegacyMetadataFileName =
    "specforge_build_metadata.json";

enum class MetadataSchemaRequirement {
    Any,
    CurrentFilename,
    LegacyFilename,
};

std::optional<std::string> ReadRequiredMetadataString(
    const JsonValue& root,
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
    const JsonValue& root,
    std::string_view key)
{
    const JsonValue* member = JsonObjectMember(root, key);
    if (member == nullptr) {
        return std::nullopt;
    }
    if (member->kind == JsonValue::Kind::Null) {
        return std::optional<std::string>{};
    }
    if (member->kind != JsonValue::Kind::String) {
        return std::nullopt;
    }
    if (!metadata_validation::IsRequiredMetadataString(
            member->string_value)) {
        return std::nullopt;
    }
    return member->string_value;
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

bool MatchesTargetArchitecture(
    std::string_view actual_architecture,
    int schema_version,
    std::string_view expected_architecture)
{
    if (actual_architecture == expected_architecture) {
        return true;
    }
    return schema_version == kLegacySchemaVersion &&
        ((actual_architecture == "x64" &&
          expected_architecture == "amd64") ||
         (actual_architecture == "amd64" &&
          expected_architecture == "x64"));
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
    const JsonValue& root,
    int schema_version,
    const BuildIdentity& expected_identity)
{
    const JsonValue* product = &root;
    const JsonValue* build = &root;
    std::optional<std::string> product_name = std::string("SpecForge");
    if (schema_version >= kSchema4Version) {
        product = JsonObjectMember(root, "product");
        build = JsonObjectMember(root, "build");
        if (product == nullptr || product->kind != JsonValue::Kind::Object ||
            build == nullptr || build->kind != JsonValue::Kind::Object) {
            return {};
        }
        product_name = ReadRequiredMetadataString(*product, "name");
    }

    const std::optional<std::string> specforge_version =
        ReadRequiredMetadataString(
            *product,
            schema_version >= kSchema4Version
                ? "version"
                : "specforge_version");
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
    // yaml_cpp was added after schema 5 had already shipped.  A missing member
    // therefore denotes the legacy schema 5 shape and is completed from the
    // executable's generated build identity.  If the member is present it must
    // still satisfy the ordinary required-string contract.
    const JsonValue* yaml_cpp_member = JsonObjectMember(*build, "yaml_cpp");
    const std::optional<std::string> yaml_cpp =
        schema_version == metadata_contract::kSchema5Version &&
            yaml_cpp_member != nullptr
        ? ReadRequiredMetadataString(*build, "yaml_cpp")
        : std::optional<std::string>{build_info::kBuildYamlCppVersion};
    const std::optional<std::string> zlib =
        ReadRequiredMetadataString(*build, "zlib");

    std::optional<FinalizedArtifactMetadata> finalized_artifact;
    if (schema_version == metadata_contract::kSchema5Version) {
        const std::optional<std::string> completed_at_utc =
            ReadRequiredMetadataString(*build, "completed_at_utc");
        const JsonValue* artifact_value =
            JsonObjectMember(root, "artifact");
        const std::optional<std::string> artifact_file =
            artifact_value != nullptr &&
                artifact_value->kind == JsonValue::Kind::Object
            ? ReadRequiredMetadataString(*artifact_value, "file")
            : std::nullopt;
        const std::optional<std::string> artifact_sha256 =
            artifact_value != nullptr &&
                artifact_value->kind == JsonValue::Kind::Object
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

    if (!product_name || !specforge_version || !configuration ||
        !target_architecture || !source_mode || !source_revision ||
        !compiler_id || !compiler_version || !cmake_version || !generator ||
        !windows_sdk_version || !dear_imgui || !implot || !yaml_cpp || !zlib) {
        return {};
    }
    metadata.compiler_id = *compiler_id;
    metadata.compiler_version = *compiler_version;
    metadata.cmake_version = *cmake_version;
    metadata.generator = *generator;
    metadata.windows_sdk_version = *windows_sdk_version;
    metadata.dear_imgui_version = *dear_imgui;
    metadata.implot_version = *implot;
    metadata.yaml_cpp_version = *yaml_cpp;
    metadata.zlib_version = *zlib;
    metadata.finalized_artifact = std::move(finalized_artifact);

    if (schema_version == metadata_contract::kSchema5Version) {
        const BuildIdentity actual_identity = {
            .product_name = *product_name,
            .specforge_version = *specforge_version,
            .configuration = *configuration,
            .target_architecture = *target_architecture,
            .source_mode = *source_mode,
            .source_revision = *source_revision
                ? **source_revision
                : "",
        };
        if (!metadata_validation::ValidateSchema5BuildMetadata(
                actual_identity,
                metadata)) {
            return {};
        }
    } else if (!metadata_validation::IsDottedNumericVersion(
                   *compiler_version,
                   1U,
                   3U) ||
               !metadata_validation::IsDottedNumericVersion(
                   *cmake_version,
                   2U,
                   3U) ||
               metadata_validation::ContainsControlCharacter(*generator) ||
               (*windows_sdk_version &&
                !metadata_validation::IsDottedNumericVersion(
                    **windows_sdk_version,
                    2U,
                    3U)) ||
               !metadata_validation::IsValidSidecarSourceTuple(
                   *source_mode,
                   *source_revision)) {
        return {};
    }

    if (*product_name != expected_identity.product_name ||
        *specforge_version != expected_identity.specforge_version ||
        *configuration != expected_identity.configuration ||
        !MatchesTargetArchitecture(
            *target_architecture,
            schema_version,
            expected_identity.target_architecture) ||
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
    const JsonValue& root,
    int schema_version,
    DeploymentMetadata& deployment)
{
    if (schema_version == kLegacySchemaVersion) {
        const std::optional<std::string> release_profile =
            ReadRequiredMetadataString(root, "release_profile");
        if (!release_profile) {
            return "schema 3 requires a non-empty string release_profile";
        }
        if (*release_profile == "Portable") {
            deployment.storage_profile = StorageProfile::Portable;
            return std::nullopt;
        }
        if (*release_profile == "Installed") {
            deployment.storage_profile = StorageProfile::LocalAppData;
            return std::nullopt;
        }
        return "schema 3 release_profile must be exactly Portable or Installed";
    }

    const JsonValue* deployment_value =
        JsonObjectMember(root, "deployment");
    if (deployment_value == nullptr) {
        return std::nullopt;
    }
    if (deployment_value->kind != JsonValue::Kind::Object) {
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

bool IsSchemaAllowedForRequirement(
    int schema_version,
    MetadataSchemaRequirement requirement)
{
    switch (requirement) {
    case MetadataSchemaRequirement::Any:
        return schema_version == kLegacySchemaVersion ||
            schema_version == kSchema4Version ||
            schema_version == metadata_contract::kSchema5Version;
    case MetadataSchemaRequirement::CurrentFilename:
        return schema_version == kSchema4Version ||
            schema_version == metadata_contract::kSchema5Version;
    case MetadataSchemaRequirement::LegacyFilename:
        return schema_version == kLegacySchemaVersion;
    }
    return false;
}

std::string SchemaRequirementDescription(
    MetadataSchemaRequirement requirement)
{
    switch (requirement) {
    case MetadataSchemaRequirement::Any:
        return "schema 3, 4, or 5";
    case MetadataSchemaRequirement::CurrentFilename:
        return "schema 4 or 5";
    case MetadataSchemaRequirement::LegacyFilename:
        return "schema 3";
    }
    return "a supported schema";
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
    const BuildIdentity& expected_identity,
    MetadataSchemaRequirement schema_requirement)
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
    std::optional<JsonValue> root = ParseJson(contents, parse_error);
    if (!root || root->kind != JsonValue::Kind::Object) {
        result.startup_error = MetadataError(
            path,
            parse_error.empty()
                ? "the document root must be an object"
                : "invalid JSON: " + parse_error);
        return result;
    }

    const std::optional<int> schema_version =
        ReadJsonIntMember(*root, "schema_version");
    if (!schema_version ||
        !IsSchemaAllowedForRequirement(
            *schema_version,
            MetadataSchemaRequirement::Any)) {
        result.startup_error = MetadataError(
            path,
            "schema_version must be exactly 3, 4, or 5");
        return result;
    }
    if (!IsSchemaAllowedForRequirement(
            *schema_version,
            schema_requirement)) {
        result.startup_error = MetadataError(
            path,
            "this metadata filename requires " +
                SchemaRequirementDescription(schema_requirement));
        return result;
    }

    if (const std::optional<std::string> deployment_error =
            ReadDeployment(*root, *schema_version, result.deployment)) {
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
        expected_identity,
        MetadataSchemaRequirement::Any);
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
            expected_identity,
            MetadataSchemaRequirement::CurrentFilename);
    }
    if (exists_error) {
        SpecForgeMetadataReadResult result;
        result.metadata_path = current_path;
        result.startup_error = std::move(exists_error);
        return result;
    }

    const std::filesystem::path legacy_path =
        package_root / kLegacyMetadataFileName;
    if (PathExists(legacy_path, exists_error)) {
        return ReadSpecForgeMetadataForSchema(
            legacy_path,
            expected_identity,
            MetadataSchemaRequirement::LegacyFilename);
    }
    if (exists_error) {
        SpecForgeMetadataReadResult result;
        result.metadata_path = legacy_path;
        result.startup_error = std::move(exists_error);
        return result;
    }
    return MissingMetadataResult();
}

BuildIdentity CompiledBuildIdentity()
{
    return {
        .product_name = "SpecForge",
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
