#include "app/local_user_state_json.h"

#include "app/local_user_state.h"

#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace spectiary {
namespace {

// Read-only pre-1.0 compatibility for application-owned JSON. Canonical ASDF
// and user-owned annotation metadata never pass through this adapter. Writers
// only emit the current format; legacy schema validation still runs below.
void NormalizeLegacyApplicationState(nlohmann::json& root)
{
    const auto format = ReadJsonStringMember(root, "format_kind");
    if (!format) return;
    constexpr std::pair<std::string_view, std::string_view> formats[] = {
        {"specforge.ui_language.settings", "spectiary.ui_language.settings"},
        {"specforge.appearance.settings", "spectiary.appearance.settings"},
        {"specforge.ui_scale.settings", "spectiary.ui_scale.settings"},
        {"specforge.input.settings", "spectiary.input.settings"},
        {"specforge.external_source.settings", "spectiary.external_source.settings"},
        {"specforge.profile_settings", "spectiary.profile_settings"},
        {"specforge.panel_visibility.cache", "spectiary.panel_visibility.cache"},
        {"specforge.source_collection_session.cache", "spectiary.source_collection_session.cache"},
        {"specforge.sample_navigation_state.cache", "spectiary.sample_navigation_state.cache"},
        {"specforge.sample_workflow_state.cache", "spectiary.sample_workflow_state.cache"},
        {"specforge.catalog_user_state.cache", "spectiary.catalog_user_state.cache"},
        {"specforge.spectrum_plot.preferences", "spectiary.spectrum_plot.preferences"},
        {"specforge.spectrum_viewport.state", "spectiary.spectrum_viewport.state"},
        {"specforge.sample_labeling.state", "spectiary.sample_labeling.state"},
        {"specforge.sample_labeling.drafts", "spectiary.sample_labeling.drafts"},
    };
    for (const auto& [legacy, current] : formats) {
        if (*format != legacy) continue;
        root["format_kind"] = current;
        if (legacy == "specforge.appearance.settings") {
            auto theme = root.find("theme_id");
            if (theme != root.end() && theme->is_string()) {
                if (*theme == "specforge.theme.dark") *theme = "builtin.theme.dark";
                if (*theme == "specforge.theme.light") *theme = "builtin.theme.light";
            }
        }
        if (legacy == "specforge.catalog_user_state.cache") {
            for (const char* key : {"catalogs", "catalog_panel_state"}) {
                auto map = root.find(key);
                if (map == root.end() || !map->is_object()) continue;
                auto old = map->find("specforge.public");
                if (old == map->end()) continue;
                // Current records win in a mixed historical file.
                if (!map->contains("public-spectral-lines.v1")) {
                    (*map)["public-spectral-lines.v1"] = *old;
                }
                map->erase("specforge.public");
            }
            const auto normalize_references = [](auto&& self, nlohmann::json& value) -> void {
                if (value.is_object()) {
                    for (auto& [key, child] : value.items()) {
                        if (key == "catalog_identity" && child == "specforge.public") {
                            child = "public-spectral-lines.v1";
                        } else self(self, child);
                    }
                } else if (value.is_array()) {
                    for (auto& child : value) self(self, child);
                }
            };
            normalize_references(normalize_references, root);
        }
        return;
    }
}

struct JsonLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A byte iterator keeps cancellation responsive even inside one long string,
// number, or whitespace token. JSON grammar remains entirely library-owned.
class CancelableJsonIterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = char;
    using difference_type = std::ptrdiff_t;
    using pointer = const char*;
    using reference = const char&;

    CancelableJsonIterator(std::string_view text, std::size_t position,
                           const JsonCancellationCheckpoint& checkpoint)
        : text_(text), position_(position), checkpoint_(&checkpoint) {}
    reference operator*() const { return text_[position_]; }
    CancelableJsonIterator& operator++()
    {
        ++position_;
        if (position_ % 4096U == 0 && *checkpoint_) {
            (*checkpoint_)();
        }
        return *this;
    }
    CancelableJsonIterator operator++(int)
    {
        auto previous = *this;
        ++*this;
        return previous;
    }
    bool operator==(const CancelableJsonIterator& other) const
    {
        return position_ == other.position_;
    }
private:
    std::string_view text_;
    std::size_t position_;
    const JsonCancellationCheckpoint* checkpoint_;
};

bool SupportsSchema(int version, std::initializer_list<int> supported)
{
    return std::find(supported.begin(), supported.end(), version) != supported.end();
}

void ValidateWrittenCache(const std::filesystem::path& temporary_path,
                          const std::filesystem::path&)
{
    // Never report a durable save that this same build cannot read back under
    // its resource limits. The atomic-file owner retains the old generation
    // and removes the temporary file if this checkpoint throws.
    std::ifstream stream(temporary_path, std::ios::binary);
    std::string contents;
    std::string error;
    if (!ReadTextStreamCancelable(stream, contents)) {
        throw JsonLimitError("written JSON cache exceeds the input limit or could not be read");
    }
    if (!ParseJson(contents, error)) {
        throw JsonLimitError("written JSON cache is not readable: " + error);
    }
}

} // namespace

bool ReadTextStreamCancelable(
    std::istream& stream, std::string& contents,
    const JsonCancellationCheckpoint& cancellation_checkpoint)
{
    contents.clear();
    std::vector<char> buffer(64U * 1024U);
    while (stream.good()) {
        if (cancellation_checkpoint) cancellation_checkpoint();
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (count > kMaxJsonInputBytes - contents.size()) {
            contents.clear();
            return false;
        }
        contents.append(buffer.data(), count);
    }
    return stream.eof() && !stream.bad();
}

std::optional<nlohmann::json> ParseJson(
    std::string_view text, std::string& error,
    const JsonCancellationCheckpoint& cancellation_checkpoint)
{
    error.clear();
    if (cancellation_checkpoint) cancellation_checkpoint();
    if (text.size() > kMaxJsonInputBytes) {
        error = "JSON input exceeds the supported byte limit";
        return std::nullopt;
    }
    // Ordered sets bound duplicate detection even for adversarial key hashes.
    std::vector<std::set<std::string>> object_keys;
    std::size_t nodes = 0;
    auto callback = [&](int depth, nlohmann::json::parse_event_t event,
                        nlohmann::json& parsed) {
        using Event = nlohmann::json::parse_event_t;
        if (event == Event::object_start || event == Event::array_start || event == Event::value) {
            if (++nodes > kMaxJsonNodes) throw JsonLimitError("JSON node count exceeds the supported limit");
        }
        if (event == Event::object_start || event == Event::array_start) {
            if (depth >= static_cast<int>(kMaxJsonNestingDepth))
                throw JsonLimitError("JSON nesting depth exceeds the supported limit");
        }
        if (event == Event::object_start) object_keys.emplace_back();
        if (event == Event::object_end) object_keys.pop_back();
        if (event == Event::key &&
            !object_keys.back().insert(parsed.get_ref<const std::string&>()).second) {
            throw JsonLimitError("duplicate JSON object member");
        }
        return true;
    };
    try {
        auto value = nlohmann::json::parse(
            CancelableJsonIterator(text, 0, cancellation_checkpoint),
            CancelableJsonIterator(text, text.size(), cancellation_checkpoint), callback);
        if (cancellation_checkpoint) cancellation_checkpoint();
        return value;
    } catch (const JsonLimitError& exception) {
        error = exception.what();
    } catch (const nlohmann::json::exception& exception) {
        error = exception.what();
    }
    // Cancellation exceptions deliberately propagate to the caller.
    return std::nullopt;
}

const nlohmann::json* JsonObjectMember(const nlohmann::json& value, std::string_view key)
{
    if (!value.is_object()) return nullptr;
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &*found;
}

bool JsonIsInt64(const nlohmann::json& value)
{
    return value.is_number_integer() &&
        (!value.is_number_unsigned() ||
         value.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
}

std::optional<std::string> ReadJsonStringMember(const nlohmann::json& value, std::string_view key)
{
    const auto* member = JsonObjectMember(value, key);
    if (!member || !member->is_string()) return std::nullopt;
    return member->get<std::string>();
}

std::optional<std::size_t> ReadJsonSizeMember(const nlohmann::json& value, std::string_view key)
{
    const auto* member = JsonObjectMember(value, key);
    if (!member || !member->is_number_integer()) return std::nullopt;
    if (!member->is_number_unsigned() && member->get<std::int64_t>() < 0) return std::nullopt;
    const auto number = member->get<std::uint64_t>();
    if (number > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    return static_cast<std::size_t>(number);
}

std::optional<int> ReadJsonIntMember(const nlohmann::json& value, std::string_view key)
{
    const auto* member = JsonObjectMember(value, key);
    if (!member || !JsonIsInt64(*member)) return std::nullopt;
    const auto number = member->get<std::int64_t>();
    if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) return std::nullopt;
    return static_cast<int>(number);
}

bool ReadJsonBoolMember(const nlohmann::json& value, std::string_view key, bool fallback)
{
    const auto* member = JsonObjectMember(value, key);
    return member && member->is_boolean() ? member->get<bool>() : fallback;
}

std::string JsonEscape(std::string_view value)
{
    const std::string quoted = nlohmann::json(value).dump();
    return quoted.substr(1, quoted.size() - 2);
}

void WriteJsonString(std::ostream& stream, std::string_view value)
{
    stream << nlohmann::json(value).dump();
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
    const bool exists =
        std::filesystem::exists(path, exists_error);
    if (exists_error) {
        result.warning =
            "Could not read " +
            std::string(description) + ".";
        result.issue_kind =
            VersionedJsonCacheLoadIssueKind::ReadFailed;
        result.diagnostic_detail =
            LocalUserStatePathToUtf8(path) + ": " +
            exists_error.message();
        return result;
    }
    if (!exists) {
        return result;
    }

    std::ifstream stream(path);
    if (!stream.good()) {
        result.warning = "Could not read " + std::string(description) + ".";
        result.issue_kind =
            VersionedJsonCacheLoadIssueKind::ReadFailed;
        result.diagnostic_detail =
            LocalUserStatePathToUtf8(path);
        return result;
    }
    std::string contents;
    if (!ReadTextStreamCancelable(stream, contents, cancellation_checkpoint)) {
        result.warning = "Could not read " + std::string(description) + ".";
        result.issue_kind =
            VersionedJsonCacheLoadIssueKind::ReadFailed;
        result.diagnostic_detail =
            LocalUserStatePathToUtf8(path);
        return result;
    }

    std::string parse_error;
    std::optional<nlohmann::json> root = ParseJson(contents, parse_error, cancellation_checkpoint);
    if (!root || !root->is_object()) {
        result.warning = "Ignored " + std::string(description) + ": " + parse_error;
        result.issue_kind =
            VersionedJsonCacheLoadIssueKind::InvalidDocument;
        result.diagnostic_detail =
            parse_error.empty()
                ? "expected a JSON object root"
                : std::move(parse_error);
        return result;
    }

    NormalizeLegacyApplicationState(*root);
    const std::optional<std::string> parsed_format_kind = ReadJsonStringMember(*root, "format_kind");
    const std::optional<int> schema_version = ReadJsonIntMember(*root, "schema_version");
    if (!parsed_format_kind || *parsed_format_kind != format_kind || !schema_version ||
        !SupportsSchema(*schema_version, supported_schema_versions)) {
        result.warning = "Ignored unsupported " + std::string(description) + ".";
        result.issue_kind =
            VersionedJsonCacheLoadIssueKind::
                UnsupportedFormatOrSchema;
        std::ostringstream detail;
        detail << "format_kind=";
        if (parsed_format_kind) {
            detail << *parsed_format_kind;
        } else {
            detail << "<missing>";
        }
        detail << ", schema_version=";
        if (schema_version) {
            detail << *schema_version;
        } else {
            detail << "<missing>";
        }
        result.diagnostic_detail =
            std::move(detail).str();
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
    std::string* error_message,
    AtomicFileWriteCheckpoint before_replace)
{
    AtomicFileWriteOptions options;
    options.target_description = description;
    options.before_replace = [checkpoint = std::move(before_replace)](
        const std::filesystem::path& temporary, const std::filesystem::path& target) {
        ValidateWrittenCache(temporary, target);
        if (checkpoint) checkpoint(temporary, target);
    };
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

bool WriteVersionedJsonCacheDocument(
    const std::filesystem::path& path,
    std::string_view format_kind,
    int schema_version,
    std::string_view description,
    const nlohmann::json& body,
    std::string* error_message,
    AtomicFileReplaceRetryPolicy replace_retry_policy)
{
    if (!body.is_object()) {
        if (error_message != nullptr) {
            *error_message =
                "The structured JSON cache body must be an object.";
        }
        return false;
    }
    if (body.contains("format_kind") ||
        body.contains("schema_version")) {
        if (error_message != nullptr) {
            *error_message =
                "The structured JSON cache body contains a reserved member.";
        }
        return false;
    }

    AtomicFileWriteOptions options;
    options.target_description = description;
    options.replace_retry_policy = replace_retry_policy;
    options.before_replace = ValidateWrittenCache;
    return WriteFileAtomically(
        path,
        options,
        [&](std::ostream& stream, std::string& error) {
            try {
                nlohmann::json document = body;
                document["format_kind"] = format_kind;
                document["schema_version"] = schema_version;
                stream << document.dump(2) << '\n';
                return true;
            } catch (const nlohmann::json::exception& exception) {
                error = exception.what();
                return false;
            }
        },
        error_message);
}

} // namespace spectiary
