#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
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

[[nodiscard]] std::optional<JsonValue> ParseJson(std::string_view text, std::string& error);
[[nodiscard]] const JsonValue* JsonObjectMember(const JsonValue& value, std::string_view key);
[[nodiscard]] std::optional<std::string> ReadJsonStringMember(const JsonValue& value, std::string_view key);
[[nodiscard]] std::optional<std::size_t> ReadJsonSizeMember(const JsonValue& value, std::string_view key);
[[nodiscard]] std::optional<int> ReadJsonIntMember(const JsonValue& value, std::string_view key);
[[nodiscard]] bool ReadJsonBoolMember(const JsonValue& value, std::string_view key, bool fallback);

[[nodiscard]] std::string JsonEscape(std::string_view value);
void WriteJsonString(std::ostream& stream, std::string_view value);

struct VersionedJsonCacheDocument {
    JsonValue root;
    int schema_version = 0;
};

struct VersionedJsonCacheLoadResult {
    std::optional<VersionedJsonCacheDocument> document;
    std::string warning;
};

using JsonCacheBodyWriter = std::function<bool(std::ostream& stream, std::string& error)>;

[[nodiscard]] VersionedJsonCacheLoadResult LoadVersionedJsonCacheFile(
    const std::filesystem::path& path,
    std::string_view format_kind,
    std::initializer_list<int> supported_schema_versions,
    std::string_view description);

[[nodiscard]] bool WriteVersionedJsonCacheFile(
    const std::filesystem::path& path,
    std::string_view format_kind,
    int schema_version,
    std::string_view description,
    const JsonCacheBodyWriter& body_writer,
    std::string* error_message = nullptr);

}  // namespace specforge
