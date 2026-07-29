#pragma once

#include "platform/atomic_file.h"

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

namespace specforge {

struct JsonValue {
    enum class Kind {
        Null,
        Object,
        Array,
        String,
        Bool,
        Integer,
    };

    Kind kind = Kind::Null;
    std::unordered_map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
    std::string string_value;
    bool bool_value = false;
    std::int64_t integer_value = 0;
};

using JsonCancellationCheckpoint = std::function<void()>;

[[nodiscard]] bool ReadTextStreamCancelable(
    std::istream& stream,
    std::string& contents,
    const JsonCancellationCheckpoint& cancellation_checkpoint = {});
[[nodiscard]] std::optional<JsonValue> ParseJson(
    std::string_view text,
    std::string& error,
    const JsonCancellationCheckpoint& cancellation_checkpoint = {});
[[nodiscard]] const JsonValue* JsonObjectMember(const JsonValue& value, std::string_view key);
[[nodiscard]] std::optional<std::string> ReadJsonStringMember(const JsonValue& value, std::string_view key);
[[nodiscard]] std::optional<std::size_t> ReadJsonSizeMember(const JsonValue& value, std::string_view key);
[[nodiscard]] std::optional<int> ReadJsonIntMember(const JsonValue& value, std::string_view key);
[[nodiscard]] bool ReadJsonBoolMember(const JsonValue& value, std::string_view key, bool fallback);

[[nodiscard]] std::string JsonEscape(std::string_view value);
void WriteJsonString(std::ostream& stream, std::string_view value);

[[nodiscard]] JsonValue JsonNullValue();
[[nodiscard]] JsonValue JsonObjectValue(
    std::initializer_list<std::pair<std::string, JsonValue>>
        members = {});
[[nodiscard]] JsonValue JsonArrayValue(
    std::initializer_list<JsonValue> values = {});
[[nodiscard]] JsonValue JsonStringValue(
    std::string_view value);
[[nodiscard]] JsonValue JsonBoolValue(bool value);
[[nodiscard]] JsonValue JsonIntegerValue(
    std::int64_t value);

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
    JsonValue root;
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
    std::string* error_message = nullptr);

// Structured write seam for migrated cache/settings adapters. Object members
// are serialized in stable key order; the shell retains ownership of atomic
// replacement, format_kind, and schema_version.
[[nodiscard]] bool WriteVersionedJsonCacheDocument(
    const std::filesystem::path& path,
    std::string_view format_kind,
    int schema_version,
    std::string_view description,
    const JsonValue& body,
    std::string* error_message = nullptr,
    AtomicFileReplaceRetryPolicy replace_retry_policy = {});

}  // namespace specforge
