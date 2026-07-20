#include "app/local_user_state_json.h"

#include "platform/atomic_file.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

class JsonParser {
public:
    JsonParser(std::string_view text, JsonCancellationCheckpoint cancellation_checkpoint)
        : text_(text),
          cancellation_checkpoint_(std::move(cancellation_checkpoint))
    {
    }

    std::optional<JsonValue> Parse(std::string& error)
    {
        JsonValue value;
        if (!ParseValue(value, error)) {
            return std::nullopt;
        }
        SkipWhitespace();
        if (position_ != text_.size()) {
            error = "unexpected trailing JSON content";
            return std::nullopt;
        }
        return value;
    }

private:
    void Checkpoint()
    {
        if (!cancellation_checkpoint_ || position_ - last_checkpoint_position_ < 4096U) {
            return;
        }
        last_checkpoint_position_ = position_;
        cancellation_checkpoint_();
    }

    void SkipWhitespace()
    {
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
            Checkpoint();
        }
    }

    bool ParseValue(JsonValue& value, std::string& error)
    {
        SkipWhitespace();
        if (position_ >= text_.size()) {
            error = "unexpected end of JSON";
            return false;
        }

        const char character = text_[position_];
        if (character == '{') {
            return ParseObject(value, error);
        }
        if (character == '[') {
            return ParseArray(value, error);
        }
        if (character == '"') {
            value.kind = JsonValue::Kind::String;
            return ParseString(value.string_value, error);
        }
        if (StartsWith("true")) {
            position_ += 4;
            value.kind = JsonValue::Kind::Bool;
            value.bool_value = true;
            return true;
        }
        if (StartsWith("false")) {
            position_ += 5;
            value.kind = JsonValue::Kind::Bool;
            value.bool_value = false;
            return true;
        }
        if (StartsWith("null")) {
            position_ += 4;
            value.kind = JsonValue::Kind::Null;
            return true;
        }
        if (character == '-' || (character >= '0' && character <= '9')) {
            return ParseInteger(value, error);
        }

        error = "unexpected JSON token";
        return false;
    }

    bool ParseObject(JsonValue& value, std::string& error)
    {
        value.kind = JsonValue::Kind::Object;
        ++position_;
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }

        for (;;) {
            Checkpoint();
            std::string key;
            if (!ParseString(key, error)) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                error = "expected ':' after JSON object key";
                return false;
            }

            JsonValue member;
            if (!ParseValue(member, error)) {
                return false;
            }
            value.object.emplace(std::move(key), std::move(member));

            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                error = "expected ',' or '}' in JSON object";
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseArray(JsonValue& value, std::string& error)
    {
        value.kind = JsonValue::Kind::Array;
        ++position_;
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }

        for (;;) {
            Checkpoint();
            JsonValue item;
            if (!ParseValue(item, error)) {
                return false;
            }
            value.array.push_back(std::move(item));

            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                error = "expected ',' or ']' in JSON array";
                return false;
            }
        }
    }

    static int HexDigit(char value)
    {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    static void AppendUtf8(std::string& value, char32_t code_point)
    {
        if (code_point <= 0x7F) {
            value.push_back(static_cast<char>(code_point));
            return;
        }
        if (code_point <= 0x7FF) {
            value.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            value.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            return;
        }
        if (code_point <= 0xFFFF) {
            value.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            value.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            value.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            return;
        }
        value.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        value.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        value.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        value.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }

    bool ParseUnicodeEscape(char32_t& code_point, std::string& error)
    {
        if (position_ + 4 > text_.size()) {
            error = "short JSON unicode escape";
            return false;
        }

        code_point = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = HexDigit(text_[position_++]);
            if (digit < 0) {
                error = "invalid JSON unicode escape";
                return false;
            }
            code_point = (code_point << 4) | static_cast<char32_t>(digit);
        }
        return true;
    }

    bool ParseString(std::string& value, std::string& error)
    {
        SkipWhitespace();
        if (!Consume('"')) {
            error = "expected JSON string";
            return false;
        }

        value.clear();
        while (position_ < text_.size()) {
            Checkpoint();
            const char character = text_[position_++];
            if (character == '"') {
                return true;
            }
            if (static_cast<unsigned char>(character) < 0x20) {
                error = "unescaped control character in JSON string";
                return false;
            }
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            if (position_ >= text_.size()) {
                error = "unterminated JSON string escape";
                return false;
            }
            const char escaped = text_[position_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u': {
                char32_t code_point = 0;
                if (!ParseUnicodeEscape(code_point, error)) {
                    return false;
                }
                if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                    const bool has_low_surrogate =
                        position_ + 6 <= text_.size() && text_[position_] == '\\' &&
                        text_[position_ + 1] == 'u';
                    if (!has_low_surrogate) {
                        error = "expected JSON low surrogate";
                        return false;
                    }
                    position_ += 2;
                    char32_t low_surrogate = 0;
                    if (!ParseUnicodeEscape(low_surrogate, error)) {
                        return false;
                    }
                    if (low_surrogate < 0xDC00 || low_surrogate > 0xDFFF) {
                        error = "invalid JSON low surrogate";
                        return false;
                    }
                    code_point =
                        0x10000 + ((code_point - 0xD800) << 10) + (low_surrogate - 0xDC00);
                } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                    error = "unexpected JSON low surrogate";
                    return false;
                }
                AppendUtf8(value, code_point);
                break;
            }
            default:
                error = "unsupported JSON string escape";
                return false;
            }
        }

        error = "unterminated JSON string";
        return false;
    }

    bool ParseInteger(JsonValue& value, std::string& error)
    {
        bool negative = false;
        if (position_ < text_.size() && text_[position_] == '-') {
            negative = true;
            ++position_;
        }
        if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
            error = "invalid JSON number";
            return false;
        }

        const std::uint64_t limit = negative
            ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U
            : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        std::uint64_t magnitude = 0;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            Checkpoint();
            const auto digit = static_cast<std::uint64_t>(text_[position_] - '0');
            if (magnitude > (limit - digit) / 10U) {
                error = "JSON integer is out of range";
                return false;
            }
            magnitude = magnitude * 10U + digit;
            ++position_;
        }
        value.kind = JsonValue::Kind::Integer;
        if (negative) {
            if (magnitude == limit) {
                value.integer_value = std::numeric_limits<std::int64_t>::min();
            } else {
                value.integer_value = -static_cast<std::int64_t>(magnitude);
            }
        } else {
            value.integer_value = static_cast<std::int64_t>(magnitude);
        }
        return true;
    }

    bool Consume(char expected)
    {
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool StartsWith(std::string_view value) const
    {
        return text_.substr(position_, value.size()) == value;
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::size_t last_checkpoint_position_ = 0;
    JsonCancellationCheckpoint cancellation_checkpoint_;
};

bool SupportsSchema(int schema_version, std::initializer_list<int> supported_schema_versions)
{
    return std::find(supported_schema_versions.begin(), supported_schema_versions.end(), schema_version) !=
           supported_schema_versions.end();
}

char JsonHexNibble(unsigned char value)
{
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + value - 10));
}

}  // namespace

bool ReadTextStreamCancelable(
    std::istream& stream,
    std::string& contents,
    const JsonCancellationCheckpoint& cancellation_checkpoint)
{
    contents.clear();
    constexpr std::size_t kReadChunkBytes = 1024U * 1024U;
    std::vector<char> read_buffer(kReadChunkBytes);
    while (stream.good()) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        stream.read(read_buffer.data(), static_cast<std::streamsize>(read_buffer.size()));
        const std::streamsize read_count = stream.gcount();
        if (read_count > 0) {
            contents.append(read_buffer.data(), static_cast<std::size_t>(read_count));
        }
    }
    return stream.eof();
}

std::optional<JsonValue> ParseJson(
    std::string_view text,
    std::string& error,
    const JsonCancellationCheckpoint& cancellation_checkpoint)
{
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    JsonParser parser(text, cancellation_checkpoint);
    return parser.Parse(error);
}

const JsonValue* JsonObjectMember(const JsonValue& value, std::string_view key)
{
    if (value.kind != JsonValue::Kind::Object) {
        return nullptr;
    }
    const auto match = value.object.find(std::string(key));
    return match == value.object.end() ? nullptr : &match->second;
}

std::optional<std::string> ReadJsonStringMember(const JsonValue& value, std::string_view key)
{
    const JsonValue* member = JsonObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::String) {
        return std::nullopt;
    }
    return member->string_value;
}

std::optional<std::size_t> ReadJsonSizeMember(const JsonValue& value, std::string_view key)
{
    const JsonValue* member = JsonObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::Integer || member->integer_value < 0) {
        return std::nullopt;
    }
    const auto unsigned_value = static_cast<std::uint64_t>(member->integer_value);
    if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(unsigned_value);
}

std::optional<int> ReadJsonIntMember(const JsonValue& value, std::string_view key)
{
    const JsonValue* member = JsonObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::Integer ||
        member->integer_value < std::numeric_limits<int>::min() ||
        member->integer_value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(member->integer_value);
}

bool ReadJsonBoolMember(const JsonValue& value, std::string_view key, bool fallback)
{
    const JsonValue* member = JsonObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::Bool) {
        return fallback;
    }
    return member->bool_value;
}

std::string JsonEscape(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char character : value) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                const auto byte = static_cast<unsigned char>(character);
                escaped += "\\u00";
                escaped.push_back(JsonHexNibble((byte >> 4) & 0x0F));
                escaped.push_back(JsonHexNibble(byte & 0x0F));
            } else {
                escaped.push_back(character);
            }
            break;
        }
    }
    return escaped;
}

void WriteJsonString(std::ostream& stream, std::string_view value)
{
    stream << '"' << JsonEscape(value) << '"';
}

VersionedJsonCacheLoadResult LoadVersionedJsonCacheFile(
    const std::filesystem::path& path,
    std::string_view format_kind,
    std::initializer_list<int> supported_schema_versions,
    std::string_view description,
    const JsonCancellationCheckpoint& cancellation_checkpoint)
{
    VersionedJsonCacheLoadResult result;
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    if (path.empty()) {
        return result;
    }

    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        return result;
    }

    std::ifstream stream(path);
    if (!stream.good()) {
        result.warning = "Could not read " + std::string(description) + ".";
        return result;
    }
    std::string contents;
    if (!ReadTextStreamCancelable(stream, contents, cancellation_checkpoint)) {
        result.warning = "Could not read " + std::string(description) + ".";
        return result;
    }

    std::string parse_error;
    std::optional<JsonValue> root = ParseJson(contents, parse_error, cancellation_checkpoint);
    if (!root || root->kind != JsonValue::Kind::Object) {
        result.warning = "Ignored " + std::string(description) + ": " + parse_error;
        return result;
    }

    const std::optional<std::string> parsed_format_kind = ReadJsonStringMember(*root, "format_kind");
    const std::optional<int> schema_version = ReadJsonIntMember(*root, "schema_version");
    if (!parsed_format_kind || *parsed_format_kind != format_kind || !schema_version ||
        !SupportsSchema(*schema_version, supported_schema_versions)) {
        result.warning = "Ignored unsupported " + std::string(description) + ".";
        return result;
    }

    VersionedJsonCacheDocument document;
    document.root = std::move(*root);
    document.schema_version = *schema_version;
    result.document = std::move(document);
    return result;
}

bool WriteVersionedJsonCacheFile(
    const std::filesystem::path& path,
    std::string_view format_kind,
    int schema_version,
    std::string_view description,
    const JsonCacheBodyWriter& body_writer,
    std::string* error_message)
{
    AtomicFileWriteOptions options;
    options.target_description = description;
    return WriteFileAtomically(path, options, [&](std::ostream& stream, std::string& error) {
        stream << "{\n";
        stream << "  \"format_kind\": ";
        WriteJsonString(stream, format_kind);
        stream << ",\n";
        stream << "  \"schema_version\": " << schema_version;
        if (!body_writer(stream, error)) {
            return false;
        }
        stream << "\n";
        stream << "}\n";
        return true;
    }, error_message);
}

}  // namespace specforge
