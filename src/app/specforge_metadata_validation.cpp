#include "app/specforge_metadata_validation.h"

#include <cctype>
#include <string>
#include <utility>

namespace specforge::metadata_validation {
namespace {

void SetError(std::string* error_message, std::string message)
{
    if (error_message != nullptr) {
        *error_message = std::move(message);
    }
}

int ParseFixedDecimal(
    std::string_view value,
    std::size_t offset,
    std::size_t length)
{
    int result = 0;
    for (std::size_t index = 0; index < length; ++index) {
        result = result * 10 +
            (value[offset + index] - '0');
    }
    return result;
}

bool IsLeapYear(int year)
{
    return year % 4 == 0 &&
        (year % 100 != 0 || year % 400 == 0);
}

}  // namespace

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

bool IsValidUtcTimestamp(std::string_view value)
{
    if (value.size() != 20U ||
        value[4] != '-' ||
        value[7] != '-' ||
        value[10] != 'T' ||
        value[13] != ':' ||
        value[16] != ':' ||
        value[19] != 'Z') {
        return false;
    }

    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4U || index == 7U || index == 10U ||
            index == 13U || index == 16U || index == 19U) {
            continue;
        }
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
    }

    const int year = ParseFixedDecimal(value, 0U, 4U);
    const int month = ParseFixedDecimal(value, 5U, 2U);
    const int day = ParseFixedDecimal(value, 8U, 2U);
    const int hour = ParseFixedDecimal(value, 11U, 2U);
    const int minute = ParseFixedDecimal(value, 14U, 2U);
    const int second = ParseFixedDecimal(value, 17U, 2U);
    if (month < 1 || month > 12 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    constexpr int kDaysInMonth[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };
    const int days_in_month =
        kDaysInMonth[month - 1] +
        (month == 2 && IsLeapYear(year) ? 1 : 0);
    return day >= 1 && day <= days_in_month;
}

bool IsValidSha256(std::string_view value)
{
    if (value.size() != 64U) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool IsValidSidecarSourceTuple(
    std::string_view source_mode,
    const std::optional<std::string>& source_revision)
{
    if (source_mode == "working_tree") {
        return !source_revision;
    }
    if (source_mode != "head" ||
        !source_revision ||
        source_revision->size() != 40U) {
        return false;
    }
    for (const char character : *source_revision) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool ValidateSchema6BuildMetadata(
    const BuildIdentity& identity,
    const BuildMetadata& metadata,
    std::string* error_message)
{
    if (error_message != nullptr) {
        error_message->clear();
    }

    const auto fail = [&](std::string message) {
        SetError(error_message, std::move(message));
        return false;
    };

    if (identity.application_id != project_identity::kApplicationId) {
        return fail("application_id must equal the canonical founding identity");
    }
    if (!IsRequiredMetadataString(identity.product_name) ||
        !IsRequiredMetadataString(identity.specforge_version) ||
        !IsRequiredMetadataString(identity.configuration) ||
        !IsRequiredMetadataString(identity.target_architecture) ||
        !IsRequiredMetadataString(identity.source_mode)) {
        return fail("build identity contains an empty or padded required string");
    }

    std::optional<std::string> source_revision;
    if (identity.source_mode == "working_tree" &&
        identity.source_revision.empty()) {
        source_revision = std::nullopt;
    } else {
        source_revision = identity.source_revision;
    }
    if (!IsValidSidecarSourceTuple(
            identity.source_mode,
            source_revision)) {
        return fail("build identity has an invalid source mode/revision tuple");
    }

    if (!IsRequiredMetadataString(metadata.compiler_id) ||
        !IsRequiredMetadataString(metadata.compiler_version) ||
        !IsRequiredMetadataString(metadata.cmake_version) ||
        !IsRequiredMetadataString(metadata.generator) ||
        !IsRequiredMetadataString(metadata.dear_imgui_version) ||
        !IsRequiredMetadataString(metadata.implot_version) ||
        !IsRequiredMetadataString(metadata.cfitsio_version) ||
        !IsRequiredMetadataString(metadata.yaml_cpp_version) ||
        !IsRequiredMetadataString(metadata.zlib_version)) {
        return fail("configured build metadata contains an empty or padded required string");
    }
    if (!IsDottedNumericVersion(metadata.compiler_version, 1U, 3U)) {
        return fail("compiler_version must be a dotted numeric version");
    }
    if (!IsDottedNumericVersion(metadata.cmake_version, 2U, 3U)) {
        return fail("cmake_version must be a dotted numeric version");
    }
    if (ContainsControlCharacter(metadata.generator)) {
        return fail("generator must not contain control characters");
    }
    if (metadata.windows_sdk_version &&
        !IsDottedNumericVersion(*metadata.windows_sdk_version, 2U, 3U)) {
        return fail("windows_sdk_version must be a dotted numeric version");
    }
    if (!IsDottedNumericVersion(metadata.cfitsio_version, 1U, 3U)) {
        return fail("cfitsio must be a dotted numeric version");
    }

    if (!metadata.finalized_artifact ||
        !IsValidUtcTimestamp(
            metadata.finalized_artifact->completed_at_utc)) {
        return fail("completed_at_utc must be a valid UTC ISO 8601 timestamp");
    }
    if (metadata.finalized_artifact->artifact.file !=
            metadata_contract::kCanonicalExecutableFileName ||
        !IsValidSha256(metadata.finalized_artifact->artifact.sha256)) {
        return fail("artifact must contain Spectiary.exe and a lowercase SHA-256 digest");
    }
    return true;
}

}  // namespace specforge::metadata_validation
