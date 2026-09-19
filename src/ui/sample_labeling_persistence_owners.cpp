#include "ui/sample_labeling_persistence_owners.h"

#include "app/local_user_state.h"
#include "domain/utf8.h"
#include "domain/uuid_v4.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace specforge {
namespace {
using Json = nlohmann::json;
constexpr char kStateFormat[] = "spectiary.sample_labeling.state";
constexpr char kDraftFormat[] = "spectiary.sample_labeling.drafts";
constexpr int kSchema = 1;

struct InvalidOwner : std::runtime_error { using std::runtime_error::runtime_error; };
void Require(bool condition, const char* message)
{
    if (!condition) throw InvalidOwner(message);
}

const Json& Member(const Json& value, const char* key)
{
    const auto* member = JsonObjectMember(value, key);
    Require(member != nullptr, key);
    return *member;
}

std::string Text(const Json& value, const char* key, bool blank = false)
{
    const auto text = ReadJsonStringMember(value, key);
    Require(text && IsValidUtf8(*text) && (blank || IsValidUtf8WithNonWhitespace(*text)), key);
    return *text;
}

std::size_t Count(const Json& value, const char* key)
{
    const auto count = ReadJsonSizeMember(value, key);
    Require(count.has_value(), key);
    return *count;
}

bool Boolean(const Json& value, const char* key)
{
    const auto& member = Member(value, key);
    Require(member.is_boolean(), key);
    return member.get<bool>();
}

void Keys(const Json& object, std::initializer_list<std::string_view> allowed)
{
    Require(object.is_object(), "expected object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        Require(std::find(allowed.begin(), allowed.end(), it.key()) != allowed.end(),
            "unexpected field in labeling persistence owner");
    }
}

std::string TaskId(const Json& object)
{
    auto id = Text(object, "task_id");
    Require(IsCanonicalUuidV4(id), "invalid task identity");
    return id;
}

Json EncodeMetadata(const SampleLabelingTaskCanonicalMetadata& metadata)
{
    Json result{{"created_at", FormatCanonicalTimestamp(metadata.created_at)},
        {"modified_at", FormatCanonicalTimestamp(metadata.modified_at)},
        {"origin", {{"kind", metadata.origin.kind}}}};
    if (metadata.origin.annotation) {
        const auto& annotation = *metadata.origin.annotation;
        Json value{{"name", annotation.name}, {"format", annotation.format}};
        if (annotation.fingerprint) value["fingerprint"] = *annotation.fingerprint;
        result["origin"]["annotation"] = std::move(value);
    }
    if (metadata.description) result["description"] = *metadata.description;
    if (!metadata.authors.empty()) {
        result["authors"] = Json::array();
        for (const auto& author : metadata.authors) {
            Json value{{"name", author.name}};
            if (author.identifier) value["identifier"] = *author.identifier;
            if (author.email) value["email"] = *author.email;
            result["authors"].push_back(std::move(value));
        }
    }
    return result;
}

SampleLabelingTaskCanonicalMetadata DecodeMetadata(const Json& value)
{
    Keys(value, {"created_at", "modified_at", "origin", "description", "authors"});
    auto created = ParseCanonicalTimestamp(Text(value, "created_at"));
    auto modified = ParseCanonicalTimestamp(Text(value, "modified_at"));
    Require(created && modified && *modified >= *created, "invalid draft timestamps");
    SampleLabelingTaskCanonicalMetadata result;
    result.created_at = *created;
    result.modified_at = *modified;
    const auto& origin = Member(value, "origin");
    Keys(origin, {"kind", "annotation"});
    result.origin.kind = Text(origin, "kind");
    Require(result.origin.kind.front() >= 'a' && result.origin.kind.front() <= 'z' &&
        std::all_of(result.origin.kind.begin(), result.origin.kind.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        }), "invalid draft origin");
    if (const auto* annotation = JsonObjectMember(origin, "annotation")) {
        Keys(*annotation, {"name", "format", "fingerprint"});
        SampleLabelingAnnotationOrigin parsed{Text(*annotation, "name"), Text(*annotation, "format")};
        Require(IsValidSampleLabelingAnnotationOriginName(parsed.name) &&
            (parsed.format == "csv" || parsed.format == "npy"), "invalid annotation origin");
        if (annotation->contains("fingerprint")) {
            parsed.fingerprint = Text(*annotation, "fingerprint");
            const auto& fingerprint = *parsed.fingerprint;
            Require(fingerprint.size() == 71 && fingerprint.starts_with("sha256:") &&
                std::all_of(fingerprint.begin() + 7, fingerprint.end(), [](char c) {
                    return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
                }), "invalid annotation fingerprint");
        }
        result.origin.annotation = std::move(parsed);
    }
    Require(result.origin.kind != "manual" || !result.origin.annotation, "manual annotation origin");
    Require(result.origin.kind != "annotation_promotion" || result.origin.annotation.has_value(),
        "missing annotation origin");
    if (value.contains("description")) result.description = Text(value, "description", true);
    if (const auto* authors = JsonObjectMember(value, "authors")) {
        Require(authors->is_array(), "invalid authors");
        for (const auto& author : *authors) {
            Keys(author, {"name", "identifier", "email"});
            SampleLabelingAuthor parsed{.name = Text(author, "name")};
            if (author.contains("identifier")) parsed.identifier = Text(author, "identifier");
            if (author.contains("email")) parsed.email = Text(author, "email");
            result.authors.push_back(std::move(parsed));
        }
    }
    return result;
}

template <typename Source>
Json SourceHeader(const std::string& identity, const Source& source)
{
    return {{"identity", identity}, {"sample_count", source.sample_count},
        {"source_name", source.source_name}, {"source_fingerprint", source.source_fingerprint},
        {"context_fingerprint", source.context_fingerprint}};
}

template <typename Source>
void DecodeSourceHeader(const Json& value, Source& source)
{
    source.sample_count = Count(value, "sample_count");
    Require(source.sample_count > 0, "empty sample roster");
    source.source_name = Text(value, "source_name", true);
    source.source_fingerprint = Text(value, "source_fingerprint", true);
    source.context_fingerprint = Text(value, "context_fingerprint", true);
}

SampleLabelingOrdinaryState DecodeState(const RuntimePaths& paths, const Json& root,
    const JsonCancellationCheckpoint& cancellation)
{
    SampleLabelingOrdinaryState result;
    const auto& sources = Member(root, "sources");
    Require(sources.is_array(), "invalid sources");
    for (const auto& value : sources) {
        if (cancellation) cancellation();
        Keys(value, {"identity", "sample_count", "source_name", "source_fingerprint",
            "context_fingerprint", "active_task_id", "tasks"});
        SampleLabelingSourceRegistration source;
        DecodeSourceHeader(value, source);
        const auto identity = Text(value, "identity");
        const auto& tasks = Member(value, "tasks");
        Require(tasks.is_array(), "invalid task registrations");
        std::unordered_set<std::string> ids;
        for (const auto& task : tasks) {
            Keys(task, {"task_id", "output", "auto_advance", "skip_labeled_on_advance", "remembered_position", "display_name_hint"});
            SampleLabelingTaskRegistration registration;
            registration.task_id = TaskId(task);
            if (task.contains("display_name_hint")) registration.display_name_hint = Text(task, "display_name_hint");
            Require(ids.insert(registration.task_id).second, "duplicate task registration");
            registration.session.auto_advance = Boolean(task, "auto_advance");
            registration.session.skip_labeled_on_advance = Boolean(task, "skip_labeled_on_advance");
            if (!Member(task, "remembered_position").is_null()) {
                registration.session.remembered_position = Count(task, "remembered_position");
                Require(*registration.session.remembered_position < source.sample_count, "invalid position");
            }
            const auto& output = Member(task, "output");
            Keys(output, {"path", "format"});
            const auto format = Text(output, "format");
            if (format == "none") {
                Require(Member(output, "path").is_null(), "draft registration has output");
            } else {
                Require(format == "canonical_asdf" || format == "legacy_npy_with_sidecar", "invalid output format");
                registration.output_format = format == "canonical_asdf"
                    ? SampleLabelingOutputArtifactFormat::CanonicalAsdf
                    : SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
                registration.output_path = ReadPersistedPathReference(Member(output, "path"), paths);
                Require(registration.output_path && !registration.output_path->empty(), "invalid output locator");
            }
            source.tasks.push_back(std::move(registration));
        }
        if (!Member(value, "active_task_id").is_null()) {
            source.active_task_id = Text(value, "active_task_id");
            Require(ids.contains(*source.active_task_id), "unknown active task");
        }
        Require(result.sources.emplace(identity, std::move(source)).second, "duplicate source registration");
    }
    return result;
}

SampleLabelingDraftCheckpoints DecodeDrafts(const Json& root,
    const JsonCancellationCheckpoint& cancellation)
{
    SampleLabelingDraftCheckpoints result;
    const auto& sources = Member(root, "sources");
    Require(sources.is_array(), "invalid draft sources");
    for (const auto& value : sources) {
        if (cancellation) cancellation();
        Keys(value, {"identity", "sample_count", "source_name", "source_fingerprint", "context_fingerprint", "draft"});
        SampleLabelingSourceDraftCheckpoint source;
        DecodeSourceHeader(value, source);
        const auto identity = Text(value, "identity");
        const auto& draft = Member(value, "draft");
        Keys(draft, {"task_id", "task_name", "canonical_metadata", "labels", "values"});
        source.draft.task_id = TaskId(draft);
        source.draft.task_name = Text(draft, "task_name");
        source.draft.canonical_metadata = DecodeMetadata(Member(draft, "canonical_metadata"));
        const auto& labels = Member(draft, "labels");
        Require(labels.is_array(), "invalid draft labels");
        std::unordered_set<int> codes;
        std::unordered_set<char> shortcuts;
        for (const auto& label : labels) {
            Keys(label, {"code", "name", "shortcut"});
            const auto code = ReadJsonIntMember(label, "code");
            const auto shortcut = Text(label, "shortcut", true);
            Require(code && *code != kUnlabeledSampleLabelCode && codes.insert(*code).second && shortcut.size() <= 1,
                "invalid draft label code or shortcut");
            const char key = shortcut.empty() ? '\0' : shortcut.front();
            Require(key == '\0' || (IsValidSampleLabelShortcut(key) &&
                shortcuts.insert(NormalizeSampleLabelShortcut(key)).second), "invalid or duplicate draft shortcut");
            source.draft.label_set.labels.push_back({*code, Text(label, "name"), key});
        }
        const auto& values = Member(draft, "values");
        Require(values.is_array() && values.size() == source.sample_count, "draft roster/value mismatch");
        source.draft.values.reserve(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            if ((i & 0xfffU) == 0 && cancellation) cancellation();
            const auto& item = values[i];
            Require(JsonIsInt64(item) && item.get<std::int64_t>() >= std::numeric_limits<int>::min() &&
                item.get<std::int64_t>() <= std::numeric_limits<int>::max(), "invalid draft value");
            const auto code = static_cast<int>(item.get<std::int64_t>());
            Require(code == kUnlabeledSampleLabelCode || codes.contains(code), "undefined draft label");
            source.draft.values.push_back(code);
        }
        Require(result.sources.emplace(identity, std::move(source)).second, "conflicting draft slots");
    }
    return result;
}
} // namespace

SampleLabelingOwnerLoadResult<SampleLabelingOrdinaryState>
LoadSampleLabelingOrdinaryState(const RuntimePaths& paths, const std::filesystem::path& path,
    const JsonCancellationCheckpoint& cancellation)
{
    auto loaded = LoadVersionedJsonCacheFile(path, kStateFormat, {kSchema}, "labeling state", cancellation);
    SampleLabelingOwnerLoadResult<SampleLabelingOrdinaryState> result;
    result.issue_kind = loaded.issue_kind;
    result.diagnostic = loaded.diagnostic_detail;
    if (loaded.document) {
        try { result.owner = DecodeState(paths, loaded.document->root, cancellation); }
        catch (const InvalidOwner& error) {
            result.issue_kind = VersionedJsonCacheLoadIssueKind::InvalidDocument;
            result.diagnostic = error.what();
        }
    }
    return result;
}

SampleLabelingOwnerLoadResult<SampleLabelingDraftCheckpoints>
LoadSampleLabelingDraftCheckpoints(const std::filesystem::path& path,
    const JsonCancellationCheckpoint& cancellation)
{
    auto loaded = LoadVersionedJsonCacheFile(path, kDraftFormat, {kSchema}, "unsaved labeling checkpoint", cancellation);
    SampleLabelingOwnerLoadResult<SampleLabelingDraftCheckpoints> result;
    result.issue_kind = loaded.issue_kind;
    result.diagnostic = loaded.diagnostic_detail;
    if (loaded.document) {
        try { result.owner = DecodeDrafts(loaded.document->root, cancellation); }
        catch (const InvalidOwner& error) {
            result.issue_kind = VersionedJsonCacheLoadIssueKind::InvalidDocument;
            result.diagnostic = error.what();
        }
    }
    return result;
}

bool SaveSampleLabelingOrdinaryState(const RuntimePaths& paths, const std::filesystem::path& path,
    const SampleLabelingOrdinaryState& owner, std::string* error)
{
    Json body{{"sources", Json::array()}};
    for (const auto& identity : SortedCacheKeys(owner.sources)) {
        const auto& source = owner.sources.at(identity);
        auto value = SourceHeader(identity, source);
        value["active_task_id"] = source.active_task_id ? Json(*source.active_task_id) : Json(nullptr);
        value["tasks"] = Json::array();
        for (const auto& task : source.tasks) {
            Json output{{"path", nullptr}, {"format", "none"}};
            if (task.output_path) {
                std::ostringstream reference;
                WritePersistedPathReference(reference, *task.output_path, paths);
                output["path"] = Json::parse(reference.str());
            }
            switch (task.output_format) {
            case SampleLabelingOutputArtifactFormat::CanonicalAsdf: output["format"] = "canonical_asdf"; break;
            case SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar: output["format"] = "legacy_npy_with_sidecar"; break;
            default: break;
            }
            value["tasks"].push_back({{"task_id", task.task_id}, {"output", std::move(output)},
                {"auto_advance", task.session.auto_advance}, {"skip_labeled_on_advance", task.session.skip_labeled_on_advance},
                {"remembered_position", task.session.remembered_position ? Json(*task.session.remembered_position) : Json(nullptr)}});
            if (task.output_path && task.display_name_hint)
                value["tasks"].back()["display_name_hint"] = *task.display_name_hint;
        }
        body["sources"].push_back(std::move(value));
    }
    try { static_cast<void>(DecodeState(paths, body, {})); }
    catch (const InvalidOwner& failure) { if (error) *error = failure.what(); return false; }
    return WriteVersionedJsonCacheDocument(path, kStateFormat, kSchema, "labeling state", body, error);
}

bool SaveSampleLabelingDraftCheckpoints(const std::filesystem::path& path,
    const SampleLabelingDraftCheckpoints& owner, std::string* error)
{
    Json body{{"sources", Json::array()}};
    for (const auto& identity : SortedCacheKeys(owner.sources)) {
        const auto& source = owner.sources.at(identity);
        auto value = SourceHeader(identity, source);
        const auto& draft = source.draft;
        Json labels = Json::array();
        for (const auto& label : draft.label_set.labels) {
            labels.push_back({{"code", label.code}, {"name", label.name},
                {"shortcut", label.shortcut == '\0' ? std::string{} : std::string(1, label.shortcut)}});
        }
        value["draft"] = {{"task_id", draft.task_id}, {"task_name", draft.task_name},
            {"canonical_metadata", EncodeMetadata(draft.canonical_metadata)},
            {"labels", std::move(labels)}, {"values", draft.values}};
        body["sources"].push_back(std::move(value));
    }
    try { static_cast<void>(DecodeDrafts(body, {})); }
    catch (const InvalidOwner& failure) { if (error) *error = failure.what(); return false; }
    const bool saved = WriteVersionedJsonCacheDocument(path, kDraftFormat, kSchema, "unsaved labeling checkpoint", body, error);
    if (saved) HideUnsavedCheckpointDirectory(path.parent_path());
    return saved;
}
} // namespace specforge
