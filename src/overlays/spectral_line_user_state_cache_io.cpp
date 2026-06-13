#include "overlays/spectral_line_user_state.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace specforge {
namespace {

constexpr const char* kCacheFormatKind = "specforge.catalog_user_state.cache";
constexpr int kCacheSchemaVersion = 2;

std::string DisplayNameForCatalogIdentity(std::string_view identity_id)
{
    const CatalogIdentity& public_identity = PublicSpectralLineCatalogIdentity();
    return identity_id == public_identity.id ? public_identity.display_name : std::string(identity_id);
}

MarkerReference MakeReference(const CatalogIdentity& identity, std::string marker_id)
{
    MarkerReference reference;
    reference.catalog_identity = identity;
    reference.marker_id = std::move(marker_id);
    return reference;
}

UserGroup& EnsureUnassignedGroup(GroupingView& view)
{
    for (UserGroup& group : view.groups) {
        if (group.is_unassigned || group.id == UnassignedUserGroupId()) {
            group.id = UnassignedUserGroupId();
            group.name = "Unassigned";
            group.is_unassigned = true;
            return group;
        }
    }

    UserGroup group;
    group.id = UnassignedUserGroupId();
    group.name = "Unassigned";
    group.is_unassigned = true;
    view.groups.push_back(std::move(group));
    return view.groups.back();
}

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
    int int_value = 0;
};

// Internal cache reader for SpecForge's own versioned cache. This is not a
// public import/export parser; replace it with a JSON library before accepting
// arbitrary user-authored documents.
class JsonParser {
public:
    explicit JsonParser(std::string_view text)
        : text_(text)
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
    void SkipWhitespace()
    {
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
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

        int parsed = 0;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            parsed = parsed * 10 + (text_[position_] - '0');
            ++position_;
        }
        value.kind = JsonValue::Kind::Integer;
        value.int_value = negative ? -parsed : parsed;
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
};

const JsonValue* ObjectMember(const JsonValue& value, std::string_view key)
{
    if (value.kind != JsonValue::Kind::Object) {
        return nullptr;
    }
    const auto match = value.object.find(std::string(key));
    return match == value.object.end() ? nullptr : &match->second;
}

std::string ReadStringMember(const JsonValue& value, std::string_view key)
{
    const JsonValue* member = ObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::String) {
        return {};
    }
    return member->string_value;
}

bool ReadBoolMember(const JsonValue& value, std::string_view key, bool fallback)
{
    const JsonValue* member = ObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::Bool) {
        return fallback;
    }
    return member->bool_value;
}

std::filesystem::path TemporaryCachePath(const std::filesystem::path& path)
{
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path temporary = path;
    temporary += ".tmp.";
#ifdef _WIN32
    temporary += std::to_string(GetCurrentProcessId());
#else
    temporary += "pid";
#endif
    temporary += ".";
    temporary += std::to_string(timestamp);
    return temporary;
}

bool ReplaceFileAtomically(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path,
    std::string& error)
{
#ifdef _WIN32
    if (!MoveFileExW(
            temporary_path.c_str(),
            target_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "could not replace cache file: " + std::system_category().message(GetLastError());
        return false;
    }
    return true;
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary_path, target_path, rename_error);
    if (rename_error) {
        error = "could not replace cache file: " + rename_error.message();
        return false;
    }
    return true;
#endif
}

char JsonHexNibble(unsigned char value)
{
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + value - 10));
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

std::optional<MarkerReference> ReadMarkerReference(
    const JsonValue& value,
    const CatalogIdentity& fallback_identity)
{
    if (value.kind == JsonValue::Kind::String) {
        return MakeReference(fallback_identity, value.string_value);
    }
    if (value.kind != JsonValue::Kind::Object) {
        return std::nullopt;
    }

    const std::string marker_id = ReadStringMember(value, "marker_id");
    if (marker_id.empty()) {
        return std::nullopt;
    }
    CatalogIdentity identity = fallback_identity;
    const std::string identity_id = ReadStringMember(value, "catalog_identity");
    if (!identity_id.empty()) {
        identity.id = identity_id;
        identity.display_name = DisplayNameForCatalogIdentity(identity_id);
    }
    return MakeReference(identity, marker_id);
}

std::vector<MarkerReference> ReadMarkerReferences(
    const JsonValue& value,
    const CatalogIdentity& fallback_identity)
{
    std::vector<MarkerReference> references;
    if (value.kind != JsonValue::Kind::Array) {
        return references;
    }
    for (const JsonValue& item : value.array) {
        if (std::optional<MarkerReference> reference = ReadMarkerReference(item, fallback_identity)) {
            references.push_back(std::move(*reference));
        }
    }
    return references;
}

std::vector<UserGroup> ReadUserGroups(const JsonValue& value, const CatalogIdentity& identity)
{
    std::vector<UserGroup> groups;
    if (value.kind != JsonValue::Kind::Array) {
        return groups;
    }
    for (const JsonValue& item : value.array) {
        if (item.kind != JsonValue::Kind::Object) {
            continue;
        }
        UserGroup group;
        group.id = ReadStringMember(item, "id");
        group.name = ReadStringMember(item, "name");
        group.is_unassigned = ReadBoolMember(item, "is_unassigned", false) || group.id == UnassignedUserGroupId();
        if (group.is_unassigned) {
            group.id = UnassignedUserGroupId();
            group.name = "Unassigned";
        }
        if (group.id.empty() || group.name.empty()) {
            continue;
        }
        if (const JsonValue* references = ObjectMember(item, "marker_references")) {
            group.marker_references = ReadMarkerReferences(*references, identity);
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<GroupingView> ReadGroupingViews(const JsonValue& value, const CatalogIdentity& identity)
{
    std::vector<GroupingView> views;
    if (value.kind != JsonValue::Kind::Array) {
        return views;
    }
    for (const JsonValue& item : value.array) {
        if (item.kind != JsonValue::Kind::Object) {
            continue;
        }
        GroupingView view;
        view.id = ReadStringMember(item, "id");
        view.name = ReadStringMember(item, "name");
        view.read_only = ReadBoolMember(item, "read_only", false);
        if (view.id.empty() || view.name.empty() || view.read_only) {
            continue;
        }
        if (const JsonValue* groups = ObjectMember(item, "groups")) {
            view.groups = ReadUserGroups(*groups, identity);
        }
        EnsureUnassignedGroup(view);
        views.push_back(std::move(view));
    }
    return views;
}

std::unordered_set<std::string> ReadExpandedGroupIds(const JsonValue& value)
{
    std::unordered_set<std::string> expanded;
    if (value.kind != JsonValue::Kind::Array) {
        return expanded;
    }
    for (const JsonValue& item : value.array) {
        if (item.kind == JsonValue::Kind::String && !item.string_value.empty()) {
            expanded.insert(item.string_value);
        }
    }
    return expanded;
}

std::unordered_map<std::string, bool> ReadMarkerVisibility(const JsonValue& value)
{
    std::unordered_map<std::string, bool> visibility;
    if (value.kind != JsonValue::Kind::Object) {
        return visibility;
    }
    for (const auto& [marker_id, item] : value.object) {
        if (!marker_id.empty() && item.kind == JsonValue::Kind::Bool) {
            visibility.emplace(marker_id, item.bool_value);
        }
    }
    return visibility;
}

void WriteMarkerReferences(std::ostream& stream, const std::vector<MarkerReference>& references, int indent)
{
    const std::string base(static_cast<std::size_t>(indent), ' ');
    const std::string item_indent(static_cast<std::size_t>(indent + 2), ' ');
    stream << "[";
    if (!references.empty()) {
        stream << "\n";
    }
    for (std::size_t index = 0; index < references.size(); ++index) {
        const MarkerReference& reference = references[index];
        stream << item_indent << "{ \"catalog_identity\": ";
        WriteJsonString(stream, reference.catalog_identity.id);
        stream << ", \"marker_id\": ";
        WriteJsonString(stream, reference.marker_id);
        stream << " }";
        stream << (index + 1 == references.size() ? "\n" : ",\n");
    }
    if (!references.empty()) {
        stream << base;
    }
    stream << "]";
}

void WriteUserGroups(std::ostream& stream, const std::vector<UserGroup>& groups, int indent)
{
    const std::string base(static_cast<std::size_t>(indent), ' ');
    const std::string item_indent(static_cast<std::size_t>(indent + 2), ' ');
    stream << "[";
    if (!groups.empty()) {
        stream << "\n";
    }
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const UserGroup& group = groups[index];
        stream << item_indent << "{\n";
        stream << item_indent << "  \"id\": ";
        WriteJsonString(stream, group.id);
        stream << ",\n";
        stream << item_indent << "  \"name\": ";
        WriteJsonString(stream, group.name);
        stream << ",\n";
        stream << item_indent << "  \"is_unassigned\": " << (group.is_unassigned ? "true" : "false") << ",\n";
        stream << item_indent << "  \"marker_references\": ";
        WriteMarkerReferences(stream, group.marker_references, indent + 2);
        stream << "\n" << item_indent << "}";
        stream << (index + 1 == groups.size() ? "\n" : ",\n");
    }
    if (!groups.empty()) {
        stream << base;
    }
    stream << "]";
}

void WriteGroupingViews(std::ostream& stream, const std::vector<GroupingView>& views, int indent)
{
    const std::string base(static_cast<std::size_t>(indent), ' ');
    const std::string item_indent(static_cast<std::size_t>(indent + 2), ' ');
    std::vector<const GroupingView*> writable_views;
    for (const GroupingView& view : views) {
        if (!view.read_only) {
            writable_views.push_back(&view);
        }
    }

    stream << "[";
    if (!writable_views.empty()) {
        stream << "\n";
    }
    for (std::size_t index = 0; index < writable_views.size(); ++index) {
        const GroupingView& view = *writable_views[index];
        stream << item_indent << "{\n";
        stream << item_indent << "  \"id\": ";
        WriteJsonString(stream, view.id);
        stream << ",\n";
        stream << item_indent << "  \"name\": ";
        WriteJsonString(stream, view.name);
        stream << ",\n";
        stream << item_indent << "  \"groups\": ";
        WriteUserGroups(stream, view.groups, indent + 2);
        stream << "\n" << item_indent << "}";
        stream << (index + 1 == writable_views.size() ? "\n" : ",\n");
    }
    if (!writable_views.empty()) {
        stream << base;
    }
    stream << "]";
}

std::vector<std::string> SortedMapKeys(const std::unordered_map<std::string, bool>& values)
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

std::vector<std::string> SortedSetValues(const std::unordered_set<std::string>& values)
{
    std::vector<std::string> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

std::vector<std::string> SortedCatalogKeys(const CatalogUserStateCache& cache)
{
    std::vector<std::string> keys;
    keys.reserve(cache.catalogs.size());
    for (const auto& [key, state] : cache.catalogs) {
        (void)state;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<std::string> SortedCatalogPanelKeys(const CatalogUserStateCache& cache)
{
    std::vector<std::string> keys;
    keys.reserve(cache.catalog_panel_state.size());
    for (const auto& [key, state] : cache.catalog_panel_state) {
        (void)state;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

}  // namespace

std::filesystem::path DefaultCatalogUserStateCachePath()
{
#ifdef _WIN32
    DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 0) {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
        if (written > 0 && written < required) {
            value.resize(written);
            return std::filesystem::path(value) / L"SpecForge" / L"spectral-line-grouping-views.json";
        }
    }
#endif
    return std::filesystem::temp_directory_path() / "SpecForge" / "spectral-line-grouping-views.json";
}

CatalogUserStateCacheLoadResult LoadCatalogUserStateCache(const std::filesystem::path& path)
{
    CatalogUserStateCacheLoadResult result;
    if (path.empty()) {
        return result;
    }

    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        return result;
    }

    std::ifstream stream(path);
    if (!stream.good()) {
        result.warning = "Could not read spectral-line grouping cache: " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string contents = buffer.str();

    std::string parse_error;
    JsonParser parser(contents);
    const std::optional<JsonValue> root = parser.Parse(parse_error);
    if (!root || root->kind != JsonValue::Kind::Object) {
        result.warning = "Ignored spectral-line grouping cache: " + parse_error;
        return result;
    }

    const std::string format_kind = ReadStringMember(*root, "format_kind");
    const JsonValue* schema_version = ObjectMember(*root, "schema_version");
    const bool supported_schema =
        schema_version != nullptr && schema_version->kind == JsonValue::Kind::Integer &&
        (schema_version->int_value == 1 || schema_version->int_value == kCacheSchemaVersion);
    if (format_kind != kCacheFormatKind || !supported_schema) {
        result.warning = "Ignored unsupported spectral-line grouping cache.";
        return result;
    }

    const JsonValue* catalogs = ObjectMember(*root, "catalogs");
    if (catalogs == nullptr || catalogs->kind != JsonValue::Kind::Object) {
        return result;
    }

    for (const auto& [identity_id, catalog_value] : catalogs->object) {
        if (identity_id.empty() || catalog_value.kind != JsonValue::Kind::Object) {
            continue;
        }

        CatalogIdentity identity;
        identity.id = identity_id;
        identity.display_name = DisplayNameForCatalogIdentity(identity_id);
        CatalogUserState state = MakeCatalogUserState(identity);
        const std::string active_view_id = ReadStringMember(catalog_value, "active_view_id");
        if (!active_view_id.empty()) {
            state.active_view_id = active_view_id;
        }
        if (const JsonValue* marker_visibility = ObjectMember(catalog_value, "marker_visibility")) {
            state.marker_visibility = ReadMarkerVisibility(*marker_visibility);
        }
        if (const JsonValue* expanded = ObjectMember(catalog_value, "expanded_group_ids")) {
            result.cache.catalog_panel_state[identity_id].expanded_group_ids = ReadExpandedGroupIds(*expanded);
        }
        if (const JsonValue* grouping_views = ObjectMember(catalog_value, "grouping_views")) {
            state.grouping_views = ReadGroupingViews(*grouping_views, identity);
        }
        result.cache.catalogs.emplace(identity_id, std::move(state));
    }
    if (const JsonValue* panel_state = ObjectMember(*root, "catalog_panel_state")) {
        if (panel_state->kind == JsonValue::Kind::Object) {
            for (const auto& [identity_id, panel_value] : panel_state->object) {
                if (identity_id.empty() || panel_value.kind != JsonValue::Kind::Object) {
                    continue;
                }
                if (const JsonValue* expanded = ObjectMember(panel_value, "expanded_group_ids")) {
                    result.cache.catalog_panel_state[identity_id].expanded_group_ids = ReadExpandedGroupIds(*expanded);
                }
            }
        }
    }
    return result;
}

bool SaveCatalogUserStateCache(
    const std::filesystem::path& path,
    const CatalogUserStateCache& cache,
    std::string& error)
{
    error.clear();
    if (path.empty()) {
        error = "spectral-line grouping cache path is empty";
        return false;
    }

    std::error_code filesystem_error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "could not create cache directory: " + filesystem_error.message();
            return false;
        }
    }

    const std::filesystem::path temporary_path = TemporaryCachePath(path);
    std::ofstream stream(temporary_path, std::ios::trunc);
    if (!stream.good()) {
        error = "could not open temporary cache for writing: " + temporary_path.string();
        return false;
    }

    stream << "{\n";
    stream << "  \"format_kind\": ";
    WriteJsonString(stream, kCacheFormatKind);
    stream << ",\n";
    stream << "  \"schema_version\": " << kCacheSchemaVersion << ",\n";
    stream << "  \"catalogs\": {\n";

    const std::vector<std::string> catalog_keys = SortedCatalogKeys(cache);
    for (std::size_t catalog_index = 0; catalog_index < catalog_keys.size(); ++catalog_index) {
        const std::string& catalog_id = catalog_keys[catalog_index];
        const CatalogUserState& state = cache.catalogs.at(catalog_id);

        stream << "    ";
        WriteJsonString(stream, catalog_id);
        stream << ": {\n";
        stream << "      \"active_view_id\": ";
        WriteJsonString(stream, state.active_view_id);
        stream << ",\n";

        stream << "      \"marker_visibility\": {";
        const std::vector<std::string> marker_ids = SortedMapKeys(state.marker_visibility);
        if (!marker_ids.empty()) {
            stream << "\n";
        }
        for (std::size_t index = 0; index < marker_ids.size(); ++index) {
            const std::string& marker_id = marker_ids[index];
            stream << "        ";
            WriteJsonString(stream, marker_id);
            stream << ": " << (state.marker_visibility.at(marker_id) ? "true" : "false");
            stream << (index + 1 == marker_ids.size() ? "\n" : ",\n");
        }
        if (!marker_ids.empty()) {
            stream << "      ";
        }
        stream << "},\n";

        stream << "      \"grouping_views\": ";
        WriteGroupingViews(stream, state.grouping_views, 6);
        stream << "\n    }";
        stream << (catalog_index + 1 == catalog_keys.size() ? "\n" : ",\n");
    }

    stream << "  },\n";
    stream << "  \"catalog_panel_state\": {\n";

    const std::vector<std::string> panel_keys = SortedCatalogPanelKeys(cache);
    for (std::size_t catalog_index = 0; catalog_index < panel_keys.size(); ++catalog_index) {
        const std::string& catalog_id = panel_keys[catalog_index];
        const CatalogPanelState& state = cache.catalog_panel_state.at(catalog_id);
        stream << "    ";
        WriteJsonString(stream, catalog_id);
        stream << ": {\n";
        stream << "      \"expanded_group_ids\": [";
        const std::vector<std::string> expanded = SortedSetValues(state.expanded_group_ids);
        if (!expanded.empty()) {
            stream << "\n";
        }
        for (std::size_t index = 0; index < expanded.size(); ++index) {
            stream << "        ";
            WriteJsonString(stream, expanded[index]);
            stream << (index + 1 == expanded.size() ? "\n" : ",\n");
        }
        if (!expanded.empty()) {
            stream << "      ";
        }
        stream << "]\n";
        stream << "    }";
        stream << (catalog_index + 1 == panel_keys.size() ? "\n" : ",\n");
    }

    stream << "  }\n";
    stream << "}\n";
    if (!stream.good()) {
        error = "could not write temporary spectral-line grouping cache: " + temporary_path.string();
        stream.close();
        std::error_code remove_error;
        std::filesystem::remove(temporary_path, remove_error);
        return false;
    }
    stream.close();
    if (!stream.good()) {
        error = "could not close temporary spectral-line grouping cache: " + temporary_path.string();
        std::error_code remove_error;
        std::filesystem::remove(temporary_path, remove_error);
        return false;
    }

    if (!ReplaceFileAtomically(temporary_path, path, error)) {
        std::error_code remove_error;
        std::filesystem::remove(temporary_path, remove_error);
        return false;
    }
    return true;
}


}  // namespace specforge
