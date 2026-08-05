#pragma once

#include "app/specforge_metadata.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace specforge::metadata_validation {

[[nodiscard]] bool IsRequiredMetadataString(std::string_view value);
[[nodiscard]] bool IsDottedNumericVersion(
    std::string_view value,
    std::size_t minimum_separators,
    std::size_t maximum_separators);
[[nodiscard]] bool ContainsControlCharacter(std::string_view value);
[[nodiscard]] bool IsValidUtcTimestamp(std::string_view value);
[[nodiscard]] bool IsValidSha256(std::string_view value);
[[nodiscard]] bool IsValidSidecarSourceTuple(
    std::string_view source_mode,
    const std::optional<std::string>& source_revision);

// Validates every field emitted by the schema 5 build/artifact contract. The
// build identity comparison against the running executable remains the
// parser's responsibility; this function validates the serialized values.
[[nodiscard]] bool ValidateSchema5BuildMetadata(
    const BuildIdentity& identity,
    const BuildMetadata& metadata,
    std::string* error_message = nullptr);

}  // namespace specforge::metadata_validation
