#pragma once

#include "platform/atomic_file.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spectiary {

inline constexpr std::size_t kMaxJsonInputBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaxJsonNestingDepth = 64U;
inline constexpr std::size_t kMaxJsonNodes = 2U * 1024U * 1024U;

[[nodiscard]] bool JsonIsInt64(const nlohmann::json& value);

using JsonCancellationCheckpoint = std::function<void()>;

[[nodiscard]] bool ReadTextStreamCancelable(
    std::istream& stream,
    std::string& contents,
    const JsonCancellationCheckpoint& cancellation_checkpoint = {});
[[nodiscard]] std::optional<nlohmann::json> ParseJson(
    std::string_view text,
    std::string& error,
    const JsonCancellationCheckpoint& cancellation_checkpoint = {});
[[nodiscard]] const nlohmann::json* JsonObjectMember(const nlohmann::json& value, std::string_view key);
[[nodiscard]] std::optional<std::string> ReadJsonStringMember(const nlohmann::json& value, std::string_view key);
[[nodiscard]] std::optional<std::size_t> ReadJsonSizeMember(const nlohmann::json& value, std::string_view key);
[[nodiscard]] std::optional<int> ReadJsonIntMember(const nlohmann::json& value, std::string_view key);
[[nodiscard]] bool ReadJsonBoolMember(const nlohmann::json& value, std::string_view key, bool fallback);

[[nodiscard]] std::string JsonEscape(std::string_view value);
void WriteJsonString(std::ostream& stream, std::string_view value);

template <typename AssociativeContainer>
[[nodiscard]] std::vector<std::string> SortedCacheKeys(const AssociativeContainer& values)
{
    std::vector<std::string> keys;
    keys.reserve(values.size());
    for (const auto& [key, value] : values) {
        (void)value;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

struct VersionedJsonCacheDocument {
    nlohmann::json root;
    int schema_version = 0;
};

enum class VersionedJsonCacheLoadIssueKind {
    None,
    ReadFailed,
    InvalidDocument,
    UnsupportedFormatOrSchema,
};

struct VersionedJsonCacheLoadResult {
    std::optional<VersionedJsonCacheDocument> document;
    std::string warning;
    VersionedJsonCacheLoadIssueKind issue_kind =
        VersionedJsonCacheLoadIssueKind::None;
    std::string diagnostic_detail;
};

using JsonCacheBodyWriter = std::function<bool(std::ostream& stream, std::string& error)>;

[[nodiscard]] VersionedJsonCacheLoadResult LoadVersionedJsonCacheFile(
    const std::filesystem::path& path,
    std::string_view format_kind,
    std::initializer_list<int> supported_schema_versions,
    std::string_view description,
    const JsonCancellationCheckpoint& cancellation_checkpoint = {});

[[nodiscard]] bool WriteVersionedJsonCacheFile(
    const std::filesystem::path& path,
    std::string_view format_kind,
    int schema_version,
    std::string_view description,
    const JsonCacheBodyWriter& body_writer,
    std::string* error_message = nullptr,
    AtomicFileWriteCheckpoint before_replace = {});

// Structured write seam for migrated cache/settings adapters. Object members
// are serialized in stable key order; the shell retains ownership of atomic
// replacement, format_kind, and schema_version.
[[nodiscard]] bool WriteVersionedJsonCacheDocument(
    const std::filesystem::path& path,
    std::string_view format_kind,
    int schema_version,
    std::string_view description,
    const nlohmann::json& body,
    std::string* error_message = nullptr,
    AtomicFileReplaceRetryPolicy replace_retry_policy = {});

}  // namespace spectiary
