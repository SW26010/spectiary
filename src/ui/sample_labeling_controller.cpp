#include "ui/sample_labeling_controller.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

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

constexpr const char* kStateFormatKind = "specforge.sample_labeling_tasks.cache";
constexpr int kStateSchemaVersion = 1;
constexpr std::uint64_t kStateSaveDebounceFrames = 30;
constexpr std::uint64_t kStateSaveRetryFrames = 120;

bool ShouldRetryOutputSave(const SampleLabelingTask& task)
{
    return task.output_path && !task.pending_sample_indices.empty() &&
           (task.save_state.kind == SampleLabelSaveStateKind::Pending ||
            task.save_state.kind == SampleLabelSaveStateKind::Failed);
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
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

void WriteJsonString(std::ostream& stream, std::string_view value)
{
    stream << '"' << JsonEscape(value) << '"';
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path Utf8ToPath(const std::string& value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
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
    std::int64_t integer_value = 0;
};

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

        const std::uint64_t limit = negative
            ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U
            : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        std::uint64_t magnitude = 0;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
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
};

const JsonValue* ObjectMember(const JsonValue& value, std::string_view key)
{
    if (value.kind != JsonValue::Kind::Object) {
        return nullptr;
    }
    const auto match = value.object.find(std::string(key));
    return match == value.object.end() ? nullptr : &match->second;
}

std::optional<std::string> ReadStringMember(const JsonValue& value, std::string_view key)
{
    const JsonValue* member = ObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::String) {
        return std::nullopt;
    }
    return member->string_value;
}

std::optional<std::size_t> ReadSizeMember(const JsonValue& value, std::string_view key)
{
    const JsonValue* member = ObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::Integer || member->integer_value < 0) {
        return std::nullopt;
    }
    const auto unsigned_value = static_cast<std::uint64_t>(member->integer_value);
    if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(unsigned_value);
}

std::optional<int> ReadIntMember(const JsonValue& value, std::string_view key)
{
    const JsonValue* member = ObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::Integer ||
        member->integer_value < std::numeric_limits<int>::min() ||
        member->integer_value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(member->integer_value);
}

bool ReadBoolMember(const JsonValue& value, std::string_view key, bool fallback)
{
    const JsonValue* member = ObjectMember(value, key);
    if (member == nullptr || member->kind != JsonValue::Kind::Bool) {
        return fallback;
    }
    return member->bool_value;
}

SampleLabelSaveStateKind ParseSaveStateKind(std::string_view text)
{
    if (text == "pending") {
        return SampleLabelSaveStateKind::Pending;
    }
    if (text == "failed") {
        return SampleLabelSaveStateKind::Failed;
    }
    if (text == "autosaved_to_output") {
        return SampleLabelSaveStateKind::AutosavedToOutput;
    }
    return SampleLabelSaveStateKind::InternalDraftOnly;
}

std::string_view SaveStateKindText(SampleLabelSaveStateKind kind)
{
    switch (kind) {
    case SampleLabelSaveStateKind::AutosavedToOutput:
        return "autosaved_to_output";
    case SampleLabelSaveStateKind::Pending:
        return "pending";
    case SampleLabelSaveStateKind::Failed:
        return "failed";
    case SampleLabelSaveStateKind::InternalDraftOnly:
    default:
        return "internal_draft_only";
    }
}

void ApplyPendingValues(SampleLabelingTask& task, const JsonValue& task_object)
{
    const JsonValue* pending_values = ObjectMember(task_object, "pending_values");
    if (pending_values == nullptr || pending_values->kind != JsonValue::Kind::Array) {
        return;
    }

    for (const JsonValue& pending_object : pending_values->array) {
        if (pending_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        const std::optional<std::size_t> sample_index = ReadSizeMember(pending_object, "index");
        const std::optional<int> value = ReadIntMember(pending_object, "value");
        if (!sample_index || !value || *sample_index >= task.values.size()) {
            continue;
        }
        task.values[*sample_index] = *value;
        task.pending_sample_indices.insert(*sample_index);
    }
}

SampleLabelSet ParseLabelSet(const JsonValue& task_object)
{
    SampleLabelSet label_set;
    const JsonValue* labels = ObjectMember(task_object, "labels");
    if (labels == nullptr || labels->kind != JsonValue::Kind::Array) {
        return label_set;
    }

    for (const JsonValue& label_object : labels->array) {
        if (label_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        const std::optional<int> code = ReadIntMember(label_object, "code");
        const std::optional<std::string> name = ReadStringMember(label_object, "name");
        const std::optional<std::string> shortcut_text = ReadStringMember(label_object, "shortcut");
        if (!code || !name) {
            continue;
        }
        const char shortcut = shortcut_text && !shortcut_text->empty() ? (*shortcut_text)[0] : '\0';
        (void)UpsertSampleLabel(label_set, SampleLabelDefinition{*code, *name, shortcut});
    }
    return label_set;
}

std::optional<SampleLabelingTask> ParseTask(const JsonValue& task_object, std::size_t sample_count)
{
    if (task_object.kind != JsonValue::Kind::Object) {
        return std::nullopt;
    }

    const std::optional<std::string> task_id = ReadStringMember(task_object, "task_id");
    const std::optional<std::string> task_name = ReadStringMember(task_object, "task_name");
    if (!task_id || task_id->empty()) {
        return std::nullopt;
    }

    SampleLabelingTask task = CreateSampleLabelingTask(*task_id, task_name.value_or(*task_id), sample_count);
    task.label_set = ParseLabelSet(task_object);
    task.auto_advance = ReadBoolMember(task_object, "auto_advance", false);
    task.skip_labeled_on_advance = ReadBoolMember(task_object, "skip_labeled_on_advance", false);
    if (const std::optional<std::size_t> remembered = ReadSizeMember(task_object, "remembered_position")) {
        if (*remembered < sample_count) {
            task.remembered_position = *remembered;
        }
    }
    if (const std::optional<std::string> output_path = ReadStringMember(task_object, "output_path")) {
        if (!output_path->empty()) {
            task.output_path = Utf8ToPath(*output_path);
        }
    }
    bool output_load_failed = false;
    std::string output_load_error;
    if (task.output_path) {
        if (std::optional<std::vector<int>> values =
                LoadSampleLabelResultNpy(*task.output_path, sample_count, &output_load_error)) {
            task.values = std::move(*values);
        } else {
            output_load_failed = true;
        }
        ApplyPendingValues(task, task_object);
    } else if (const JsonValue* values = ObjectMember(task_object, "values")) {
        if (values->kind == JsonValue::Kind::Array && values->array.size() == sample_count) {
            std::vector<int> parsed_values;
            parsed_values.reserve(values->array.size());
            bool all_ints = true;
            for (const JsonValue& value : values->array) {
                if (value.kind != JsonValue::Kind::Integer ||
                    value.integer_value < std::numeric_limits<int>::min() ||
                    value.integer_value > std::numeric_limits<int>::max()) {
                    all_ints = false;
                    break;
                }
                parsed_values.push_back(static_cast<int>(value.integer_value));
            }
            if (all_ints) {
                task.values = std::move(parsed_values);
            }
        }
    }
    task.save_state.kind = task.output_path ? SampleLabelSaveStateKind::AutosavedToOutput
                                            : SampleLabelSaveStateKind::InternalDraftOnly;
    if (const std::optional<std::string> save_state = ReadStringMember(task_object, "save_state")) {
        task.save_state.kind = ParseSaveStateKind(*save_state);
    }
    if (const std::optional<std::string> save_message = ReadStringMember(task_object, "save_message")) {
        task.save_state.message = *save_message;
    }
    if (output_load_failed) {
        task.save_state.kind = SampleLabelSaveStateKind::Failed;
        if (task.save_state.message.empty()) {
            task.save_state.message =
                output_load_error.empty() ? "could not read label output" : "could not read label output: " + output_load_error;
        }
    }
    task.save_state.pending_count = task.pending_sample_indices.size();
    if (!task.pending_sample_indices.empty() && task.save_state.kind != SampleLabelSaveStateKind::Failed) {
        task.save_state.kind = SampleLabelSaveStateKind::Pending;
    } else if (!task.output_path && task.save_state.kind == SampleLabelSaveStateKind::Pending) {
        task.save_state.kind = SampleLabelSaveStateKind::InternalDraftOnly;
    }
    return task;
}

struct LoadStateCacheResult {
    std::unordered_map<std::string, SampleLabelingController::SourceState> sources;
    std::string warning;
};

LoadStateCacheResult LoadStateCache(const std::filesystem::path& path)
{
    LoadStateCacheResult result;
    if (path.empty()) {
        return result;
    }

    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        return result;
    }

    std::ifstream stream(path);
    if (!stream.good()) {
        result.warning = "Could not read sample-labeling task record.";
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string contents = buffer.str();

    std::string parse_error;
    JsonParser parser(contents);
    const std::optional<JsonValue> root = parser.Parse(parse_error);
    if (!root || root->kind != JsonValue::Kind::Object) {
        result.warning = "Ignored sample-labeling task record: " + parse_error;
        return result;
    }

    const std::optional<std::string> format_kind = ReadStringMember(*root, "format_kind");
    const std::optional<int> schema_version = ReadIntMember(*root, "schema_version");
    if (!format_kind || *format_kind != kStateFormatKind || !schema_version ||
        *schema_version != kStateSchemaVersion) {
        result.warning = "Ignored unsupported sample-labeling task record.";
        return result;
    }

    const JsonValue* sources = ObjectMember(*root, "sources");
    if (sources == nullptr || sources->kind != JsonValue::Kind::Array) {
        return result;
    }

    for (const JsonValue& source_object : sources->array) {
        if (source_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        const std::optional<std::string> identity = ReadStringMember(source_object, "identity");
        const std::optional<std::size_t> sample_count = ReadSizeMember(source_object, "sample_count");
        if (!identity || identity->empty() || !sample_count || *sample_count == 0) {
            continue;
        }

        SampleLabelingController::SourceState state;
        state.sample_count = *sample_count;
        if (std::optional<std::string> active_task_id = ReadStringMember(source_object, "active_task_id");
            active_task_id && !active_task_id->empty()) {
            state.active_task_id = std::move(*active_task_id);
        }
        if (const JsonValue* tasks = ObjectMember(source_object, "tasks")) {
            if (tasks->kind != JsonValue::Kind::Array) {
                continue;
            }
            for (const JsonValue& task_object : tasks->array) {
                if (std::optional<SampleLabelingTask> task = ParseTask(task_object, state.sample_count)) {
                    state.tasks.push_back(std::move(*task));
                }
            }
        }
        if (state.active_task_id && std::none_of(state.tasks.begin(), state.tasks.end(), [&state](const auto& task) {
                return task.task_id == *state.active_task_id;
            })) {
            state.active_task_id.reset();
        }
        result.sources.emplace(*identity, std::move(state));
    }
    return result;
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

bool ReplaceFileAtomically(const std::filesystem::path& temporary_path, const std::filesystem::path& target_path)
{
#ifdef _WIN32
    return MoveFileExW(
               temporary_path.c_str(),
               target_path.c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary_path, target_path, rename_error);
    return !rename_error;
#endif
}

bool SaveStateCacheFile(
    const std::filesystem::path& path,
    const std::unordered_map<std::string, SampleLabelingController::SourceState>& sources)
{
    if (path.empty()) {
        return false;
    }

    std::error_code filesystem_error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            return false;
        }
    }

    std::vector<std::string> keys;
    keys.reserve(sources.size());
    for (const auto& [key, state] : sources) {
        (void)state;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());

    const std::filesystem::path temporary_path = TemporaryCachePath(path);
    std::ofstream stream(temporary_path, std::ios::trunc);
    if (!stream.good()) {
        return false;
    }

    stream << "{\n";
    stream << "  \"format_kind\": ";
    WriteJsonString(stream, kStateFormatKind);
    stream << ",\n";
    stream << "  \"schema_version\": " << kStateSchemaVersion << ",\n";
    stream << "  \"sources\": [";
    if (!keys.empty()) {
        stream << "\n";
    }

    for (std::size_t source_index = 0; source_index < keys.size(); ++source_index) {
        const std::string& key = keys[source_index];
        const SampleLabelingController::SourceState& state = sources.at(key);
        stream << "    {\n";
        stream << "      \"identity\": ";
        WriteJsonString(stream, key);
        stream << ",\n";
        stream << "      \"sample_count\": " << state.sample_count << ",\n";
        stream << "      \"active_task_id\": ";
        WriteJsonString(stream, state.active_task_id.value_or(""));
        stream << ",\n";
        stream << "      \"tasks\": [";
        if (!state.tasks.empty()) {
            stream << "\n";
        }
        for (std::size_t task_index = 0; task_index < state.tasks.size(); ++task_index) {
            const SampleLabelingTask& task = state.tasks[task_index];
            stream << "        {\n";
            stream << "          \"task_id\": ";
            WriteJsonString(stream, task.task_id);
            stream << ",\n";
            stream << "          \"task_name\": ";
            WriteJsonString(stream, task.task_name);
            stream << ",\n";
            stream << "          \"auto_advance\": " << (task.auto_advance ? "true" : "false") << ",\n";
            stream << "          \"skip_labeled_on_advance\": " << (task.skip_labeled_on_advance ? "true" : "false")
                   << ",\n";
            stream << "          \"remembered_position\": ";
            if (task.remembered_position) {
                stream << *task.remembered_position;
            } else {
                stream << "null";
            }
            stream << ",\n";
            stream << "          \"output_path\": ";
            WriteJsonString(stream, task.output_path ? PathToUtf8(*task.output_path) : std::string_view{});
            stream << ",\n";
            stream << "          \"labels\": [";
            if (!task.label_set.labels.empty()) {
                stream << "\n";
            }
            for (std::size_t label_index = 0; label_index < task.label_set.labels.size(); ++label_index) {
                const SampleLabelDefinition& label = task.label_set.labels[label_index];
                stream << "            { \"code\": " << label.code << ", \"name\": ";
                WriteJsonString(stream, label.name);
                stream << ", \"shortcut\": ";
                const std::string shortcut = label.shortcut == '\0' ? std::string{} : std::string(1, label.shortcut);
                WriteJsonString(stream, shortcut);
                stream << " }" << (label_index + 1 == task.label_set.labels.size() ? "\n" : ",\n");
            }
            if (!task.label_set.labels.empty()) {
                stream << "          ";
            }
            stream << "],\n";
            stream << "          \"save_state\": ";
            WriteJsonString(stream, SaveStateKindText(task.save_state.kind));
            stream << ",\n";
            stream << "          \"save_message\": ";
            WriteJsonString(stream, task.save_state.message);
            if (!task.output_path) {
                stream << ",\n";
                stream << "          \"values\": [";
                for (std::size_t value_index = 0; value_index < task.values.size(); ++value_index) {
                    stream << task.values[value_index];
                    if (value_index + 1 != task.values.size()) {
                        stream << ", ";
                    }
                }
                stream << "]";
            } else if (!task.pending_sample_indices.empty()) {
                std::vector<std::size_t> pending_indices(
                    task.pending_sample_indices.begin(),
                    task.pending_sample_indices.end());
                std::sort(pending_indices.begin(), pending_indices.end());

                stream << ",\n";
                stream << "          \"pending_values\": [";
                bool wrote_pending_value = false;
                for (const std::size_t sample_index : pending_indices) {
                    if (sample_index >= task.values.size()) {
                        continue;
                    }
                    if (wrote_pending_value) {
                        stream << ", ";
                    }
                    stream << "{ \"index\": " << sample_index << ", \"value\": " << task.values[sample_index] << " }";
                    wrote_pending_value = true;
                }
                stream << "]";
            }
            stream << "\n";
            stream << "        }" << (task_index + 1 == state.tasks.size() ? "\n" : ",\n");
        }
        if (!state.tasks.empty()) {
            stream << "      ";
        }
        stream << "]\n";
        stream << "    }" << (source_index + 1 == keys.size() ? "\n" : ",\n");
    }
    if (!keys.empty()) {
        stream << "  ";
    }
    stream << "]\n";
    stream << "}\n";
    if (!stream.good()) {
        stream.close();
        std::filesystem::remove(temporary_path, filesystem_error);
        return false;
    }
    stream.close();
    if (!stream.good()) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return false;
    }
    if (!ReplaceFileAtomically(temporary_path, path)) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return false;
    }
    return true;
}

std::filesystem::path DefaultSampleLabelingStatePath()
{
#ifdef _WIN32
    DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 0) {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
        if (written > 0 && written < required) {
            value.resize(written);
            return std::filesystem::path(value) / L"SpecForge" / L"sample-labeling-tasks.json";
        }
    }
#endif
    return std::filesystem::temp_directory_path() / "SpecForge" / "sample-labeling-tasks.json";
}

}  // namespace

SampleLabelingController::SampleLabelingController()
    : SampleLabelingController(DefaultSampleLabelingStatePath())
{
}

SampleLabelingController::SampleLabelingController(std::filesystem::path state_cache_path)
    : state_cache_path_(std::move(state_cache_path))
{
}

void SampleLabelingController::ActivateSource(std::string source_identity, std::size_t sample_count)
{
    EnsureStateCacheLoaded();
    if (source_identity.empty() || sample_count == 0) {
        ClearActiveSource();
        return;
    }

    SourceState& state = sources_[source_identity];
    if (state.sample_count != 0 && state.sample_count != sample_count) {
        state.tasks.clear();
        state.active_task_id.reset();
    }
    state.sample_count = sample_count;
    active_source_identity_ = std::move(source_identity);
    if (std::any_of(state.tasks.begin(), state.tasks.end(), ShouldRetryOutputSave)) {
        QueueOutputRetry();
    }
}

void SampleLabelingController::ClearActiveSource()
{
    active_source_identity_.reset();
}

void SampleLabelingController::RemoveSource(std::string_view source_identity)
{
    if (active_source_identity_ && *active_source_identity_ == source_identity) {
        active_source_identity_.reset();
    }
}

bool SampleLabelingController::has_active_source() const
{
    return ActiveSource() != nullptr;
}

SampleLabelingTask* SampleLabelingController::active_task()
{
    SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id) {
        return nullptr;
    }

    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [state](const auto& task) {
        return task.task_id == *state->active_task_id;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

const SampleLabelingTask* SampleLabelingController::active_task() const
{
    const SourceState* state = ActiveSource();
    if (state == nullptr || !state->active_task_id) {
        return nullptr;
    }

    const auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [state](const auto& task) {
        return task.task_id == *state->active_task_id;
    });
    return match == state->tasks.end() ? nullptr : &*match;
}

SampleLabelingTask* SampleLabelingController::CreateTask(std::string task_id, std::string task_name)
{
    SourceState* state = ActiveSource();
    if (state == nullptr || task_id.empty()) {
        return nullptr;
    }

    auto match = std::find_if(state->tasks.begin(), state->tasks.end(), [&task_id](const auto& task) {
        return task.task_id == task_id;
    });
    if (match == state->tasks.end()) {
        state->tasks.push_back(CreateSampleLabelingTask(std::move(task_id), std::move(task_name), state->sample_count));
        match = state->tasks.end() - 1;
    }
    state->active_task_id = match->task_id;
    QueueStateSave();
    return active_task();
}

bool SampleLabelingController::UpsertActiveLabel(SampleLabelDefinition label)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr || !UpsertSampleLabel(task->label_set, std::move(label))) {
        return false;
    }
    QueueStateSave();
    return true;
}

bool SampleLabelingController::SetActiveTaskOutputPath(std::filesystem::path output_path)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr || output_path.empty()) {
        return false;
    }
    SelectSampleLabelTaskOutputPath(*task, std::move(output_path));
    if (ShouldRetryOutputSave(*task)) {
        QueueOutputRetry();
    }
    QueueStateSave();
    return true;
}

bool SampleLabelingController::RememberActivePosition(std::size_t sample_index)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr || sample_index >= task->values.size()) {
        return false;
    }
    task->remembered_position = sample_index;
    QueueStateSave();
    return true;
}

bool SampleLabelingController::PersistActiveTask()
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return false;
    }

    if (!task->output_path) {
        return PersistActiveTaskRecord();
    }

    const SampleLabelTaskPersistResult result = PersistSampleLabelingTaskResult(*task);
    if (!result.output_saved && ShouldRetryOutputSave(*task)) {
        QueueOutputRetry();
    }
    QueueStateSave();
    const bool record_saved = FlushStateCache();
    return result.output_saved && record_saved;
}

bool SampleLabelingController::PersistActiveTaskRecord()
{
    QueueStateSave();
    return FlushStateCache();
}

bool SampleLabelingController::MarkActiveOutputPersisted()
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return false;
    }
    MarkSampleLabelTaskPersisted(*task, SampleLabelSaveStateKind::AutosavedToOutput);
    QueueStateSave();
    return true;
}

bool SampleLabelingController::MarkActiveOutputSaveFailed(std::string message)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return false;
    }
    MarkSampleLabelTaskSaveFailed(*task, std::move(message));
    if (ShouldRetryOutputSave(*task)) {
        QueueOutputRetry();
    }
    QueueStateSave();
    return true;
}

SampleLabelWriteResult SampleLabelingController::AssignLabel(std::size_t sample_index, int code)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return {};
    }
    SampleLabelWriteResult result = AssignSampleLabel(*task, sample_index, code);
    if (result.changed) {
        if (ShouldRetryOutputSave(*task)) {
            QueueOutputRetry();
        }
        QueueStateSave();
    }
    return result;
}

SampleLabelWriteResult SampleLabelingController::ClearLabel(std::size_t sample_index)
{
    SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return {};
    }
    SampleLabelWriteResult result = ClearSampleLabel(*task, sample_index);
    if (result.changed) {
        if (ShouldRetryOutputSave(*task)) {
            QueueOutputRetry();
        }
        QueueStateSave();
    }
    return result;
}

std::optional<int> SampleLabelingController::LabelCodeForShortcut(char shortcut) const
{
    const SampleLabelingTask* task = active_task();
    if (task == nullptr) {
        return std::nullopt;
    }
    return SampleLabelCodeForShortcut(task->label_set, shortcut);
}

SampleLabelingController::SourceState* SampleLabelingController::ActiveSource()
{
    if (!active_source_identity_) {
        return nullptr;
    }
    const auto match = sources_.find(*active_source_identity_);
    return match == sources_.end() ? nullptr : &match->second;
}

const SampleLabelingController::SourceState* SampleLabelingController::ActiveSource() const
{
    if (!active_source_identity_) {
        return nullptr;
    }
    const auto match = sources_.find(*active_source_identity_);
    return match == sources_.end() ? nullptr : &match->second;
}

void SampleLabelingController::EnsureStateCacheLoaded()
{
    if (state_cache_loaded_) {
        return;
    }
    state_cache_loaded_ = true;
    LoadStateCacheResult result = LoadStateCache(state_cache_path_);
    sources_ = std::move(result.sources);
    state_cache_load_warning_ = std::move(result.warning);
}

void SampleLabelingController::QueueStateSave()
{
    state_cache_dirty_ = true;
    state_cache_save_failed_ = false;
    state_cache_error_.clear();
}

void SampleLabelingController::QueueOutputRetry()
{
    output_retry_pending_ = true;
}

bool SampleLabelingController::TryRetryOutputSaves()
{
    EnsureStateCacheLoaded();

    bool attempted = false;
    bool all_succeeded = true;
    for (auto& [identity, state] : sources_) {
        (void)identity;
        for (SampleLabelingTask& task : state.tasks) {
            if (!ShouldRetryOutputSave(task)) {
                continue;
            }
            attempted = true;
            const SampleLabelTaskPersistResult result = PersistSampleLabelingTaskResult(task);
            all_succeeded = all_succeeded && result.output_saved;
        }
    }

    if (!attempted) {
        output_retry_pending_ = false;
        next_output_retry_frame_ = 0;
        return true;
    }

    QueueStateSave();
    return all_succeeded;
}

bool SampleLabelingController::MaybeRetryOutputSaves(std::uint64_t frame_index)
{
    if (!output_retry_pending_) {
        return false;
    }
    if (next_output_retry_frame_ == 0) {
        next_output_retry_frame_ = frame_index + kStateSaveRetryFrames;
        return false;
    }
    if (frame_index < next_output_retry_frame_) {
        return false;
    }

    const bool all_succeeded = TryRetryOutputSaves();
    if (all_succeeded) {
        output_retry_pending_ = false;
        next_output_retry_frame_ = 0;
    } else {
        output_retry_pending_ = true;
        next_output_retry_frame_ = frame_index + kStateSaveRetryFrames;
    }
    return true;
}

bool SampleLabelingController::TrySaveStateCache()
{
    EnsureStateCacheLoaded();
    const auto normalize_saved_states = [](std::unordered_map<std::string, SourceState>& sources) {
        for (auto& [identity, state] : sources) {
            (void)identity;
            for (SampleLabelingTask& task : state.tasks) {
                if (!task.output_path) {
                    MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::InternalDraftOnly);
                    continue;
                }
                task.save_state.pending_count = task.pending_sample_indices.size();
                if (task.pending_sample_indices.empty() &&
                    task.save_state.kind != SampleLabelSaveStateKind::Failed) {
                    MarkSampleLabelTaskPersisted(task, SampleLabelSaveStateKind::AutosavedToOutput);
                } else if (!task.pending_sample_indices.empty() &&
                           task.save_state.kind != SampleLabelSaveStateKind::Failed) {
                    task.save_state.kind = SampleLabelSaveStateKind::Pending;
                    task.save_state.message.clear();
                }
            }
        }
    };

    std::unordered_map<std::string, SourceState> sources_to_write = sources_;
    normalize_saved_states(sources_to_write);
    const bool saved = SaveStateCacheFile(state_cache_path_, sources_to_write);
    if (saved) {
        normalize_saved_states(sources_);
        state_cache_dirty_ = false;
        state_cache_save_failed_ = false;
        next_state_save_frame_ = 0;
        state_cache_error_.clear();
    } else {
        state_cache_dirty_ = true;
        state_cache_save_failed_ = true;
        state_cache_error_ = "could not write local sample-labeling task record";
    }
    return saved;
}

void SampleLabelingController::MaybeSaveStateCache(std::uint64_t frame_index)
{
    const bool output_retry_attempted = MaybeRetryOutputSaves(frame_index);
    if (output_retry_attempted && state_cache_dirty_) {
        if (!TrySaveStateCache()) {
            next_state_save_frame_ = frame_index + kStateSaveRetryFrames;
        }
        return;
    }

    if (!state_cache_dirty_) {
        return;
    }
    if (next_state_save_frame_ == 0) {
        next_state_save_frame_ = frame_index + kStateSaveDebounceFrames;
        return;
    }
    if (frame_index < next_state_save_frame_) {
        return;
    }

    if (!TrySaveStateCache()) {
        next_state_save_frame_ = frame_index + kStateSaveRetryFrames;
    }
}

bool SampleLabelingController::FlushStateCache()
{
    if (!state_cache_dirty_) {
        return true;
    }
    return TrySaveStateCache();
}

bool SampleLabelingController::state_save_pending() const
{
    return state_cache_dirty_;
}

bool SampleLabelingController::state_save_failed() const
{
    return state_cache_save_failed_;
}

std::string_view SampleLabelingController::state_save_error() const
{
    return state_cache_error_;
}

std::string_view SampleLabelingController::state_load_warning() const
{
    return state_cache_load_warning_;
}

}  // namespace specforge
