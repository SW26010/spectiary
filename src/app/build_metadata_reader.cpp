#include "app/build_metadata_reader.h"

#include "app/local_user_state_json.h"
#include "app/runtime_paths.h"
#include "specforge/specforge_build_identity.h"

#include <cctype>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace specforge {
namespace {

constexpr int kSupportedSchemaVersion = 3;
constexpr std::string_view kMetadataFileName =
    "specforge_build_metadata.json";

bool IsRequiredMetadataString(std::string_view value)
{
    if (value.empty()) {
        return false;
    }
    const auto is_whitespace = [](char character) {
        return std::isspace(
                   static_cast<unsigned char>(character)) != 0;
    };
    return !is_whitespace(value.front()) &&
        !is_whitespace(value.back());
}

bool IsDottedNumericVersion(
    std::string_view value,
    std::size_t minimum_separators,
    std::size_t maximum_separators)
{
    std::size_t separator_count = 0;
    bool segment_has_digit = false;
    for (const char character : value) {
        if (character >= '0' && character <= '9') {
            segment_has_digit = true;
            continue;
        }
        if (character != '.' || !segment_has_digit) {
            return false;
        }
        ++separator_count;
        segment_has_digit = false;
    }
    return segment_has_digit &&
        separator_count >= minimum_separators &&
        separator_count <= maximum_separators;
}

bool ContainsControlCharacter(std::string_view value)
{
    for (const unsigned char character : value) {
        if (character <= 0x1fU) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> ReadRequiredMetadataString(
    const JsonValue& root,
    std::string_view key)
{
    std::optional<std::string> value =
        ReadJsonStringMember(root, key);
    if (!value || !IsRequiredMetadataString(*value)) {
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
    if (!IsRequiredMetadataString(member->string_value)) {
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

}  // namespace

BuildMetadataReadResult ReadBuildMetadata(
    const std::filesystem::path& path,
    const BuildIdentity& expected_identity)
{
    std::ifstream stream(path);
    if (!stream.good()) {
        return {};
    }

    std::string contents;
    if (!ReadTextStreamCancelable(stream, contents)) {
        return {};
    }

    std::string parse_error;
    std::optional<JsonValue> root = ParseJson(contents, parse_error);
    if (!root || root->kind != JsonValue::Kind::Object) {
        return {};
    }

    const std::optional<int> schema_version =
        ReadJsonIntMember(*root, "schema_version");
    const std::optional<std::string> specforge_version =
        ReadRequiredMetadataString(*root, "specforge_version");
    const std::optional<std::string> release_profile =
        ReadRequiredMetadataString(*root, "release_profile");
    const std::optional<std::string> configuration =
        ReadRequiredMetadataString(*root, "configuration");
    const std::optional<std::string> target_architecture =
        ReadRequiredMetadataString(*root, "target_architecture");
    const std::optional<std::string> source_mode =
        ReadRequiredMetadataString(*root, "source_mode");
    const std::optional<std::optional<std::string>> source_revision =
        ReadNullableStringMember(*root, "source_revision");

    BuildMetadata metadata;
    const std::optional<std::string> compiler_id =
        ReadRequiredMetadataString(*root, "compiler_id");
    const std::optional<std::string> compiler_version =
        ReadRequiredMetadataString(*root, "compiler_version");
    const std::optional<std::string> cmake_version =
        ReadRequiredMetadataString(*root, "cmake_version");
    const std::optional<std::string> generator =
        ReadRequiredMetadataString(*root, "generator");
    const std::optional<std::optional<std::string>> windows_sdk_version =
        ReadNullableStringMember(*root, "windows_sdk_version");
    const std::optional<std::string> dear_imgui =
        ReadRequiredMetadataString(*root, "dear_imgui");
    const std::optional<std::string> implot =
        ReadRequiredMetadataString(*root, "implot");
    const std::optional<std::string> zlib =
        ReadRequiredMetadataString(*root, "zlib");

    if (!schema_version || *schema_version != kSupportedSchemaVersion ||
        !specforge_version || !release_profile || !configuration ||
        !target_architecture || !source_mode || !source_revision ||
        !compiler_id || !compiler_version || !cmake_version || !generator ||
        !windows_sdk_version || !dear_imgui || !implot || !zlib) {
        return {};
    }
    if (!IsDottedNumericVersion(*compiler_version, 1U, 3U) ||
        !IsDottedNumericVersion(*cmake_version, 2U, 3U) ||
        ContainsControlCharacter(*generator) ||
        (*windows_sdk_version &&
         !IsDottedNumericVersion(
             **windows_sdk_version,
             2U,
             3U))) {
        return {};
    }

    if (*specforge_version != expected_identity.specforge_version ||
        *release_profile != expected_identity.release_profile ||
        *configuration != expected_identity.configuration ||
        *target_architecture != expected_identity.target_architecture ||
        *source_mode != expected_identity.source_mode ||
        !MatchesSourceRevision(*source_revision, expected_identity)) {
        return {
            .status = BuildMetadataStatus::Mismatch,
            .metadata = std::nullopt,
        };
    }

    metadata.compiler_id = *compiler_id;
    metadata.compiler_version = *compiler_version;
    metadata.cmake_version = *cmake_version;
    metadata.generator = *generator;
    metadata.windows_sdk_version = *windows_sdk_version;
    metadata.dear_imgui_version = *dear_imgui;
    metadata.implot_version = *implot;
    metadata.zlib_version = *zlib;
    return {
        .status = BuildMetadataStatus::Available,
        .metadata = std::move(metadata),
    };
}

const BuildMetadataReadResult& DefaultBuildMetadata()
{
    static const BuildMetadataReadResult result = ReadBuildMetadata(
        DefaultRuntimePaths().package_root / kMetadataFileName,
        {
            .specforge_version = build_info::kSpecForgeVersion,
            .release_profile = build_info::kReleaseProfile,
            .configuration = build_info::kBuildConfiguration,
            .target_architecture = build_info::kTargetArchitecture,
            .source_mode = build_info::kBuildSourceMode,
            .source_revision = build_info::kBuildSourceRevision,
        });
    return result;
}

}  // namespace specforge
