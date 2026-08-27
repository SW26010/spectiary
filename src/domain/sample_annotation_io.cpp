#include "domain/sample_annotation_io.h"

#include "app/local_user_state_json.h"
#include "domain/npy_array_io.h"
#include "domain/sample_labeling_asdf_codec.h"
#include "domain/source_path_identity.h"
#include "platform/atomic_file.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr const char* kMetadataFormatKind = "specforge.sample_label_result.metadata";
constexpr int kMetadataSchemaVersion = 1;
constexpr std::string_view kInt32DtypeText = "int32";

class NpyAnnotationError : public std::runtime_error {
public:
    explicit NpyAnnotationError(std::string message)
        : std::runtime_error(std::move(message))
    {
    }
};

class AsdfAnnotationError : public std::runtime_error {
public:
    explicit AsdfAnnotationError(std::string message)
        : std::runtime_error(std::move(message))
    {
    }
};

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string FileNameToUtf8(const std::filesystem::path& path)
{
    const std::filesystem::path filename = path.filename();
    return filename.empty() ? PathToUtf8(path) : PathToUtf8(filename);
}

std::string LowerAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool IsAsdfLabelingPath(const std::filesystem::path& path)
{
    return LowerAscii(PathToUtf8(path.extension())) == ".asdf";
}

std::filesystem::path Utf8ToPath(const std::string& value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

template <typename T>
std::string FormatFloatingValue(T value)
{
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value < 0.0 ? "-inf" : "inf";
    }

    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<T>::max_digits10) << value;
    return stream.str();
}

std::string RelativeResultFileReference(const std::filesystem::path& result_path)
{
    const std::filesystem::path filename = result_path.filename();
    return filename.empty() ? PathToUtf8(result_path) : PathToUtf8(filename);
}

bool PathExists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool ValidateNpyLabelOutputPath(
    const std::filesystem::path& result_path,
    std::string* error_message)
{
    if (result_path.empty()) {
        if (error_message != nullptr) {
            *error_message = "label output path is empty";
        }
        return false;
    }
    if (IsAsdfLabelingPath(result_path)) {
        if (error_message != nullptr) {
            *error_message =
                "ASDF label output is unavailable until the ASDF persistence owner is enabled";
        }
        return false;
    }
    return true;
}

bool ValidateLabelMetadataContract(
    const std::filesystem::path& result_path,
    const SampleLabelingTask& task,
    std::string* error_message)
{
    if (!ValidateNpyLabelOutputPath(result_path, error_message)) {
        return false;
    }
    if (task.task_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "sample label result metadata task id is empty";
        }
        return false;
    }
    return true;
}

bool PathsReferToSameFile(const std::filesystem::path& left, const std::filesystem::path& right)
{
    std::error_code equivalent_error;
    if (PathExists(left) && PathExists(right) &&
        std::filesystem::equivalent(left, right, equivalent_error) && !equivalent_error) {
        return true;
    }
    return left.lexically_normal() == right.lexically_normal();
}

bool MetadataReferenceMatchesResult(
    const std::filesystem::path& metadata_path,
    const std::filesystem::path& result_path,
    std::string_view result_file)
{
    if (result_file.empty()) {
        return false;
    }
    const std::filesystem::path reference_path = Utf8ToPath(std::string(result_file));
    if (reference_path.is_absolute()) {
        return false;
    }
    return PathsReferToSameFile(metadata_path.parent_path() / reference_path, result_path);
}

std::optional<int> ReadIntMember(const JsonValue& value, std::string_view key)
{
    return ReadJsonIntMember(value, key);
}

SampleLabelSet ParseMetadataLabelSet(const JsonValue& root)
{
    SampleLabelSet label_set;
    const JsonValue* labels = JsonObjectMember(root, "labels");
    if (labels == nullptr || labels->kind != JsonValue::Kind::Array) {
        return label_set;
    }

    for (const JsonValue& label_object : labels->array) {
        if (label_object.kind != JsonValue::Kind::Object) {
            continue;
        }
        const std::optional<int> code = ReadIntMember(label_object, "code");
        const std::optional<std::string> name = ReadJsonStringMember(label_object, "name");
        const std::optional<std::string> shortcut_text =
            ReadJsonStringMember(label_object, "shortcut");
        if (!code || !name) {
            continue;
        }
        const char shortcut = shortcut_text && !shortcut_text->empty() ? (*shortcut_text)[0] : '\0';
        (void)UpsertSampleLabel(label_set, SampleLabelDefinition{*code, *name, shortcut});
    }
    return label_set;
}

std::optional<SampleLabelResultMetadataSource> ParseMetadataSource(const JsonValue& root)
{
    const JsonValue* source = JsonObjectMember(root, "source_collection");
    if (source == nullptr || source->kind != JsonValue::Kind::Object) {
        return std::nullopt;
    }

    SampleLabelResultMetadataSource parsed;
    parsed.source_name = ReadJsonStringMember(*source, "source_name").value_or("");
    parsed.source_fingerprint = ReadJsonStringMember(*source, "source_fingerprint").value_or("");
    parsed.context_fingerprint = ReadJsonStringMember(*source, "context_fingerprint").value_or("");
    parsed.spectrum_count = ReadJsonSizeMember(*source, "spectrum_count").value_or(0);
    if (parsed.source_name.empty() && parsed.source_fingerprint.empty() &&
        parsed.context_fingerprint.empty() && parsed.spectrum_count == 0) {
        return std::nullopt;
    }
    return parsed;
}

SampleLabelResultMetadataLoadResult ReadLabelMetadata(
    const std::filesystem::path& result_path,
    std::size_t expected_count,
    std::string_view expected_dtype,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint)
{
    SampleLabelResultMetadataLoadResult result;
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    const std::filesystem::path metadata_path =
        SampleAnnotationIoAdapter::MetadataPathForResult(result_path);
    std::error_code exists_error;
    if (!std::filesystem::exists(metadata_path, exists_error) || exists_error) {
        return result;
    }

    std::ifstream stream(metadata_path);
    if (!stream.good()) {
        result.warning = "could not read sample label result metadata";
        return result;
    }
    std::string contents;
    if (!ReadTextStreamCancelable(stream, contents, cancellation_checkpoint)) {
        result.warning = "could not read sample label result metadata";
        return result;
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }

    std::string parse_error;
    std::optional<JsonValue> root = ParseJson(contents, parse_error, cancellation_checkpoint);
    if (!root || root->kind != JsonValue::Kind::Object) {
        result.warning = parse_error.empty() ? "invalid sample label result metadata" : parse_error;
        return result;
    }

    const std::optional<std::string> format_kind = ReadJsonStringMember(*root, "format_kind");
    const std::optional<int> schema_version = ReadIntMember(*root, "schema_version");
    if (!format_kind || *format_kind != kMetadataFormatKind ||
        !schema_version || *schema_version != kMetadataSchemaVersion) {
        result.warning = "unsupported sample label result metadata";
        return result;
    }

    SampleLabelResultMetadata metadata;
    metadata.result_file = ReadJsonStringMember(*root, "result_file").value_or("");
    metadata.task_id = ReadJsonStringMember(*root, "task_id").value_or("");
    metadata.value_count = ReadJsonSizeMember(*root, "value_count").value_or(0);
    metadata.expected_dtype = ReadJsonStringMember(*root, "expected_dtype").value_or("");
    metadata.unlabeled_sentinel =
        ReadIntMember(*root, "unlabeled_sentinel").value_or(kUnlabeledSampleLabelCode);
    metadata.task_name = ReadJsonStringMember(*root, "task_name").value_or(metadata.task_id);
    metadata.label_set = ParseMetadataLabelSet(*root);
    metadata.source = ParseMetadataSource(*root);

    if (!MetadataReferenceMatchesResult(metadata_path, result_path, metadata.result_file)) {
        result.warning = "metadata references a different label result";
        return result;
    }
    if (metadata.value_count != expected_count) {
        result.warning = "metadata value count does not match the label result";
        return result;
    }
    if (metadata.expected_dtype != expected_dtype) {
        result.warning = "metadata dtype does not match the label result";
        return result;
    }
    if (metadata.task_id.empty()) {
        result.warning = "metadata is missing a task id";
        return result;
    }

    result.metadata = std::move(metadata);
    return result;
}

std::string DtypeName(const NpyScalarType& scalar_type)
{
    switch (scalar_type.kind) {
    case NpyScalarKind::SignedInteger:
        return "int" + std::to_string(scalar_type.item_size * 8U);
    case NpyScalarKind::UnsignedInteger:
        return "uint" + std::to_string(scalar_type.item_size * 8U);
    case NpyScalarKind::Float:
        return "float" + std::to_string(scalar_type.item_size * 8U);
    case NpyScalarKind::Bytes:
        return "bytes";
    case NpyScalarKind::Unicode:
        return "unicode";
    default:
        return "unknown";
    }
}

struct ValidatedNpyInput {
    std::ifstream stream;
    NpyHeader header;
    NpyScalarType scalar_type;
};

ValidatedNpyInput OpenValidatedNpyInput(
    const std::filesystem::path& path,
    std::size_t expected_count,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint)
{
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw NpyAnnotationError("could not open the NPY file");
    }

    NpyHeader header = ReadNpyHeader(stream);
    if (header.shape.size() != 1 || header.shape[0] != expected_count) {
        throw NpyAnnotationError("NPY array length does not match the source collection");
    }
    const std::optional<NpyScalarType> scalar_type = ParseNpyScalarType(header.descr);
    if (!scalar_type) {
        throw NpyAnnotationError("NPY dtype is not supported for read-only sample annotations");
    }
    ValidateNpyPayloadSize(path, header, expected_count, scalar_type->item_size);
    SeekNpyData(stream, header);
    return ValidatedNpyInput{
        std::move(stream),
        std::move(header),
        *scalar_type};
}

template <typename T>
std::vector<T> ReadNpyTypedValuesCancelable(
    std::ifstream& stream,
    std::size_t value_count,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint)
{
    if (value_count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) / sizeof(T)) {
        throw NpyAnnotationError("NPY array is too large to read");
    }
    std::vector<T> values(value_count);
    constexpr std::size_t kChunkBytes = 1024U * 1024U;
    constexpr std::size_t kChunkValues = std::max<std::size_t>(1, kChunkBytes / sizeof(T));
    for (std::size_t offset = 0; offset < value_count;) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const std::size_t count = std::min(kChunkValues, value_count - offset);
        stream.read(
            reinterpret_cast<char*>(values.data() + offset),
            static_cast<std::streamsize>(count * sizeof(T)));
        if (!stream) {
            throw NpyAnnotationError("NPY array data is truncated");
        }
        offset += count;
    }
    return values;
}

template <typename T>
void AssignIntegralValues(
    SampleAnnotationResult& result,
    std::ifstream& stream,
    std::size_t value_count,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint)
{
    const std::vector<T> typed_values =
        ReadNpyTypedValuesCancelable<T>(stream, value_count, cancellation_checkpoint);
    result.values.reserve(value_count);
    for (std::size_t index = 0; index < typed_values.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        const T value = typed_values[index];
        if constexpr (std::is_signed_v<T>) {
            result.values.push_back(
                SampleAnnotationValue{static_cast<std::int64_t>(value)});
        } else {
            result.values.push_back(
                SampleAnnotationValue{static_cast<std::uint64_t>(value)});
        }
    }
}

template <typename T>
void AssignFloatingValues(
    SampleAnnotationResult& result,
    std::ifstream& stream,
    std::size_t value_count,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint)
{
    const std::vector<T> typed_values =
        ReadNpyTypedValuesCancelable<T>(stream, value_count, cancellation_checkpoint);
    result.values.reserve(value_count);
    for (std::size_t index = 0; index < typed_values.size(); ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        result.values.push_back(
            SampleAnnotationValue{static_cast<double>(typed_values[index])});
    }
}

void AssignStringValues(
    SampleAnnotationResult& result,
    std::ifstream& stream,
    const NpyScalarType& scalar_type,
    std::size_t value_count,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint)
{
    result.values.reserve(value_count);
    std::string bytes(scalar_type.item_size, '\0');
    for (std::size_t index = 0; index < value_count; ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw NpyAnnotationError("NPY string data is truncated");
        }
        result.values.push_back(
            SampleAnnotationValue{DecodeNpyString(bytes, scalar_type)});
    }
}

void ApplyLabelResultMetadata(
    SampleAnnotationResult& result,
    const std::filesystem::path& path,
    std::size_t expected_count,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint)
{
    if (result.kind != SampleAnnotationKind::CategoricalInteger) {
        return;
    }

    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    SampleLabelResultMetadataLoadResult metadata_result =
        ReadLabelMetadata(path, expected_count, result.dtype_name, cancellation_checkpoint);
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    if (!metadata_result.warning.empty()) {
        result.metadata_warning =
            "Ignored " +
            FileNameToUtf8(SampleAnnotationIoAdapter::MetadataPathForResult(path).filename()) +
            ": " + metadata_result.warning + ".";
        return;
    }
    if (!metadata_result.metadata) {
        return;
    }

    result.label_metadata = std::move(metadata_result.metadata);
    result.relationship = SampleAnnotationWorkflowRelationship::ExternalLabelResult;
    if (!result.label_metadata->task_name.empty()) {
        result.name = result.label_metadata->task_name;
    }
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
}

SampleAnnotationResult ReadAnnotationNpyValues(
    const std::filesystem::path& path,
    std::size_t expected_count,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint)
{
    ValidatedNpyInput input =
        OpenValidatedNpyInput(path, expected_count, cancellation_checkpoint);

    SampleAnnotationResult result;
    result.name = FileNameToUtf8(path.filename());
    result.path = path;
    result.dtype = input.header.descr;
    result.dtype_name = DtypeName(input.scalar_type);

    switch (input.scalar_type.kind) {
    case NpyScalarKind::SignedInteger:
        result.kind = SampleAnnotationKind::CategoricalInteger;
        switch (input.scalar_type.item_size) {
        case 1:
            AssignIntegralValues<std::int8_t>(result, input.stream, expected_count, cancellation_checkpoint);
            break;
        case 2:
            AssignIntegralValues<std::int16_t>(result, input.stream, expected_count, cancellation_checkpoint);
            break;
        case 4:
            AssignIntegralValues<std::int32_t>(result, input.stream, expected_count, cancellation_checkpoint);
            break;
        case 8:
            AssignIntegralValues<std::int64_t>(result, input.stream, expected_count, cancellation_checkpoint);
            break;
        default:
            throw NpyAnnotationError("signed integer NPY dtype is not supported");
        }
        break;
    case NpyScalarKind::UnsignedInteger:
        result.kind = SampleAnnotationKind::CategoricalInteger;
        switch (input.scalar_type.item_size) {
        case 1:
            AssignIntegralValues<std::uint8_t>(result, input.stream, expected_count, cancellation_checkpoint);
            break;
        case 2:
            AssignIntegralValues<std::uint16_t>(result, input.stream, expected_count, cancellation_checkpoint);
            break;
        case 4:
            AssignIntegralValues<std::uint32_t>(result, input.stream, expected_count, cancellation_checkpoint);
            break;
        case 8:
            AssignIntegralValues<std::uint64_t>(result, input.stream, expected_count, cancellation_checkpoint);
            break;
        default:
            throw NpyAnnotationError("unsigned integer NPY dtype is not supported");
        }
        break;
    case NpyScalarKind::Float:
        result.kind = SampleAnnotationKind::ContinuousFloat;
        if (input.scalar_type.item_size == 4) {
            AssignFloatingValues<float>(result, input.stream, expected_count, cancellation_checkpoint);
        } else if (input.scalar_type.item_size == 8) {
            AssignFloatingValues<double>(result, input.stream, expected_count, cancellation_checkpoint);
        } else {
            throw NpyAnnotationError("floating-point NPY dtype is not supported");
        }
        break;
    case NpyScalarKind::Bytes:
    case NpyScalarKind::Unicode:
        result.kind = SampleAnnotationKind::Text;
        AssignStringValues(
            result,
            input.stream,
            input.scalar_type,
            expected_count,
            cancellation_checkpoint);
        break;
    default:
        throw NpyAnnotationError("NPY dtype is not supported for read-only sample annotations");
    }

    ApplyLabelResultMetadata(result, path, expected_count, cancellation_checkpoint);
    return result;
}

SampleAnnotationResult ReadAnnotationAsdfValues(
    const std::filesystem::path& path,
    const SampleAnnotationSourceCompatibility& source,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint)
{
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    std::exception_ptr cancellation;
    const SampleLabelingAsdfReadCheckpoint codec_checkpoint =
        cancellation_checkpoint
            ? SampleLabelingAsdfReadCheckpoint{
                  [&cancellation_checkpoint, &cancellation]() {
                      try {
                          cancellation_checkpoint();
                      } catch (...) {
                          cancellation = std::current_exception();
                          throw;
                      }
                  }}
            : SampleLabelingAsdfReadCheckpoint{};
    SampleLabelingAsdfReadResult read =
        ReadSampleLabelingAsdfDocument(path, codec_checkpoint);
    if (cancellation) {
        std::rethrow_exception(cancellation);
    }
    if (!read.succeeded()) {
        throw AsdfAnnotationError(
            read.error.message.empty()
                ? "could not read the ASDF labeling document"
                : read.error.message);
    }

    const SampleLabelingDocument& document = *read.document;
    if (const std::optional<SampleLabelingSourceCompatibilityError> mismatch =
            CheckSampleLabelingSourceCompatibility(
                document,
                source,
                cancellation_checkpoint)) {
        throw AsdfAnnotationError(mismatch->message);
    }
    // Annotation attachment does not persist through the codec's rewrite
    // seam. Drop its potentially large encoded roster snapshot before
    // projecting a second values representation.
    read.durable_base.reset();
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }

    SampleAnnotationResult result;
    result.name = document.labeling.name;
    result.path = path;
    result.kind = SampleAnnotationKind::CategoricalInteger;
    result.dtype = "<i4";
    result.dtype_name = std::string{kInt32DtypeText};
    result.relationship =
        SampleAnnotationWorkflowRelationship::ExternalLabelResult;
    result.values.reserve(document.annotation.values.size());
    for (std::size_t index = 0;
         index < document.annotation.values.size();
         ++index) {
        if ((index & 0xfffU) == 0U && cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        result.values.push_back(SampleAnnotationValue{
            static_cast<std::int64_t>(document.annotation.values[index])});
    }
    result.labeling_document =
        std::make_shared<SampleLabelingDocument>(std::move(*read.document));
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
    return result;
}

}  // namespace

SampleAnnotationArtifactIdentitySet
SampleAnnotationArtifactIdentities(
    const std::filesystem::path& result_path,
    SampleLabelingOutputArtifactFormat format,
    bool resolve_physical_paths)
{
    SampleAnnotationArtifactIdentitySet identities;
    if (result_path.empty()) {
        return identities;
    }

    std::vector<std::filesystem::path> paths = {result_path};
    if (format ==
        SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar) {
        paths.push_back(
            SampleAnnotationIoAdapter::MetadataPathForResult(
                result_path));
    }
    identities.all_paths_physically_resolved =
        resolve_physical_paths;
    for (const std::filesystem::path& path : paths) {
        const std::string stable_key =
            SourcePathIdentityKey(path);
        if (!stable_key.empty()) {
            identities.stable_path_keys.push_back(
                stable_key);
        }
        if (resolve_physical_paths) {
            std::vector<std::string> physical_keys =
                OutputPathIdentityKeys(path);
            if (physical_keys.empty()) {
                identities.all_paths_physically_resolved =
                    false;
            } else {
                identities.physical_path_keys.insert(
                    identities.physical_path_keys.end(),
                    std::make_move_iterator(
                        physical_keys.begin()),
                    std::make_move_iterator(
                        physical_keys.end()));
            }
        }
    }
    const auto sort_and_deduplicate = [](
                                          std::vector<std::string>& keys) {
        std::sort(keys.begin(), keys.end());
        keys.erase(
            std::unique(keys.begin(), keys.end()),
            keys.end());
    };
    sort_and_deduplicate(identities.stable_path_keys);
    sort_and_deduplicate(identities.physical_path_keys);
    return identities;
}

std::optional<SampleAnnotationResult> SampleAnnotationIoAdapter::Load(
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* error_message) const
{
    return LoadCancelable(path, expected_count, {}, error_message);
}

std::optional<SampleAnnotationResult> SampleAnnotationIoAdapter::LoadCancelable(
    const std::filesystem::path& path,
    std::size_t expected_count,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint,
    std::string* error_message) const
{
    try {
        if (IsAsdfLabelingPath(path)) {
            throw AsdfAnnotationError(
                "ASDF labeling documents require source collection identity and roster validation");
        }
        return ReadAnnotationNpyValues(path, expected_count, cancellation_checkpoint);
    } catch (const std::exception& error) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return std::nullopt;
    }
}

std::optional<SampleAnnotationResult> SampleAnnotationIoAdapter::LoadForSource(
    const std::filesystem::path& path,
    const SampleAnnotationSourceCompatibility& source,
    std::string* error_message) const
{
    return LoadForSourceCancelable(path, source, {}, error_message);
}

std::optional<SampleAnnotationResult>
SampleAnnotationIoAdapter::LoadForSourceCancelable(
    const std::filesystem::path& path,
    const SampleAnnotationSourceCompatibility& source,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint,
    std::string* error_message) const
{
    try {
        if (IsAsdfLabelingPath(path)) {
            return ReadAnnotationAsdfValues(
                path,
                source,
                cancellation_checkpoint);
        }
        return ReadAnnotationNpyValues(
            path,
            source.sample_count,
            cancellation_checkpoint);
    } catch (const std::exception& error) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return std::nullopt;
    }
}

std::optional<LoadedSampleLabelResult> SampleAnnotationIoAdapter::LoadLabelResult(
    const std::filesystem::path& path,
    std::size_t expected_count,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint,
    std::string* error_message) const
{
    try {
        ValidatedNpyInput input =
            OpenValidatedNpyInput(path, expected_count, cancellation_checkpoint);
        if (input.scalar_type.kind != NpyScalarKind::SignedInteger ||
            input.scalar_type.item_size != sizeof(std::int32_t)) {
            throw NpyAnnotationError("label output is not int32");
        }
        static_assert(
            sizeof(int) == sizeof(std::int32_t) &&
                std::numeric_limits<int>::is_signed &&
                std::numeric_limits<int>::digits == 31,
            "sample labeling requires a signed 32-bit int");

        LoadedSampleLabelResult loaded;
        loaded.values = ReadNpyTypedValuesCancelable<int>(
            input.stream,
            expected_count,
            cancellation_checkpoint);
        loaded.metadata_sidecar_exists = PathExists(MetadataPathForResult(path));
        SampleLabelResultMetadataLoadResult metadata_result =
            ReadLabelMetadata(
                path,
                expected_count,
                kInt32DtypeText,
                cancellation_checkpoint);
        if (!metadata_result.warning.empty()) {
            loaded.metadata_warning =
                "Ignored " +
                FileNameToUtf8(MetadataPathForResult(path).filename()) +
                ": " + metadata_result.warning + ".";
        } else {
            loaded.metadata = std::move(metadata_result.metadata);
        }
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        return loaded;
    } catch (const std::exception& error) {
        if (cancellation_checkpoint) {
            cancellation_checkpoint();
        }
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return std::nullopt;
    }
}

bool SampleAnnotationIoAdapter::SaveLabelArray(
    const std::filesystem::path& path,
    const SampleLabelingTask& task,
    std::string* error_message) const
{
    if (!ValidateNpyLabelOutputPath(path, error_message)) {
        return false;
    }
    AtomicFileWriteOptions options;
    options.open_mode = std::ios::binary | std::ios::trunc;
    options.target_description = "sample label result";
    return WriteFileAtomically(
        path,
        options,
        [&task](std::ostream& stream, std::string& error) {
            try {
                WriteNpyInt32Vector(stream, task.values);
            } catch (const NpyArrayError& write_error) {
                error = write_error.what();
                return false;
            }
            return true;
        },
        error_message);
}

bool SampleAnnotationIoAdapter::SaveLabelMetadata(
    const std::filesystem::path& result_path,
    const SampleLabelingTask& task,
    const SampleLabelResultMetadataSource* source,
    std::string* error_message) const
{
    if (!ValidateLabelMetadataContract(result_path, task, error_message)) {
        return false;
    }

    const std::filesystem::path metadata_path = MetadataPathForResult(result_path);
    const std::string result_file = RelativeResultFileReference(result_path);
    return WriteVersionedJsonCacheFile(
        metadata_path,
        kMetadataFormatKind,
        kMetadataSchemaVersion,
        "sample label result metadata",
        [&](std::ostream& stream, std::string&) {
            stream << ",\n";
            stream << "  \"result_file\": ";
            WriteJsonString(stream, result_file);
            stream << ",\n";
            stream << "  \"task_id\": ";
            WriteJsonString(stream, task.task_id);
            stream << ",\n";
            stream << "  \"value_count\": " << task.values.size() << ",\n";
            stream << "  \"expected_dtype\": ";
            WriteJsonString(stream, kInt32DtypeText);
            stream << ",\n";
            stream << "  \"unlabeled_sentinel\": " << kUnlabeledSampleLabelCode << ",\n";
            stream << "  \"task_name\": ";
            WriteJsonString(stream, task.task_name);
            stream << ",\n";
            stream << "  \"labels\": [";
            if (!task.label_set.labels.empty()) {
                stream << "\n";
            }
            for (std::size_t label_index = 0; label_index < task.label_set.labels.size(); ++label_index) {
                const SampleLabelDefinition& label = task.label_set.labels[label_index];
                stream << "    { \"code\": " << label.code << ", \"name\": ";
                WriteJsonString(stream, label.name);
                stream << ", \"shortcut\": ";
                const std::string shortcut =
                    label.shortcut == '\0' ? std::string{} : std::string(1, label.shortcut);
                WriteJsonString(stream, shortcut);
                stream << " }" << (label_index + 1 == task.label_set.labels.size() ? "\n" : ",\n");
            }
            if (!task.label_set.labels.empty()) {
                stream << "  ";
            }
            stream << "]";
            if (source != nullptr) {
                stream << ",\n";
                stream << "  \"source_collection\": {\n";
                stream << "    \"source_name\": ";
                WriteJsonString(stream, source->source_name);
                stream << ",\n";
                stream << "    \"source_fingerprint\": ";
                WriteJsonString(stream, source->source_fingerprint);
                stream << ",\n";
                stream << "    \"context_fingerprint\": ";
                WriteJsonString(stream, source->context_fingerprint);
                stream << ",\n";
                stream << "    \"spectrum_count\": " << source->spectrum_count << "\n";
                stream << "  }";
            }
            stream << "\n";
            return true;
        },
        error_message);
}

SampleLabelResultWriteOutcome SampleAnnotationIoAdapter::SaveLabelResult(
    const std::filesystem::path& path,
    const SampleLabelingTask& task,
    const SampleLabelResultMetadataSource* source) const
{
    SampleLabelResultWriteOutcome outcome;
    if (!ValidateLabelMetadataContract(path, task, &outcome.message)) {
        return outcome;
    }
    if (!SaveLabelArray(path, task, &outcome.message)) {
        return outcome;
    }
    outcome.array_saved = true;

    if (!SaveLabelMetadata(path, task, source, &outcome.message)) {
        return outcome;
    }
    outcome.metadata_saved = true;
    return outcome;
}

SampleLabelResultMetadataLoadResult SampleAnnotationIoAdapter::LoadLabelMetadata(
    const std::filesystem::path& result_path,
    std::size_t expected_count,
    std::string_view expected_dtype,
    const SampleAnnotationCancellationCheckpoint& cancellation_checkpoint) const
{
    return ReadLabelMetadata(
        result_path,
        expected_count,
        expected_dtype,
        cancellation_checkpoint);
}

std::filesystem::path SampleAnnotationIoAdapter::MetadataPathForResult(
    const std::filesystem::path& result_path)
{
    std::filesystem::path filename = result_path.stem();
    filename += ".sf-labels.json";
    return result_path.parent_path() / filename;
}

std::optional<int> SampleAnnotationValueAsInt(const SampleAnnotationValue& value)
{
    if (const std::int64_t* signed_value = std::get_if<std::int64_t>(&value.semantic);
        signed_value != nullptr &&
        *signed_value >= std::numeric_limits<int>::min() &&
        *signed_value <= std::numeric_limits<int>::max()) {
        return static_cast<int>(*signed_value);
    }
    if (const std::uint64_t* unsigned_value = std::get_if<std::uint64_t>(&value.semantic);
        unsigned_value != nullptr &&
        *unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return static_cast<int>(*unsigned_value);
    }
    return std::nullopt;
}

std::string SampleAnnotationValueKey(const SampleAnnotationValue& value)
{
    if (const std::int64_t* signed_value = std::get_if<std::int64_t>(&value.semantic)) {
        return std::to_string(*signed_value);
    }
    if (const std::uint64_t* unsigned_value = std::get_if<std::uint64_t>(&value.semantic)) {
        return std::to_string(*unsigned_value);
    }
    if (const double* floating_value = std::get_if<double>(&value.semantic)) {
        return FormatFloatingValue(*floating_value);
    }
    return std::get<std::string>(value.semantic);
}

std::string FormatSampleAnnotationValue(
    const SampleAnnotationResult& annotation,
    const SampleAnnotationValue& value)
{
    if (annotation.labeling_document) {
        if (const std::optional<int> code =
                SampleAnnotationValueAsInt(value)) {
            const SampleLabelingDocument& document =
                *annotation.labeling_document;
            if (*code == document.annotation.missing.value) {
                return "Unlabeled (" + std::to_string(*code) + ")";
            }
            const auto label = std::find_if(
                document.labeling.labels.begin(),
                document.labeling.labels.end(),
                [code](const SampleLabelingDocumentLabel& candidate) {
                    return candidate.code == *code;
                });
            if (label != document.labeling.labels.end()) {
                return label->name + " (" + std::to_string(*code) + ")";
            }
        }
    }
    if (annotation.label_metadata) {
        if (const std::optional<int> code = SampleAnnotationValueAsInt(value)) {
            return FormatSampleLabelValue(
                annotation.label_metadata->label_set,
                *code,
                annotation.label_metadata->unlabeled_sentinel);
        }
    }
    if (const double* floating_value = std::get_if<double>(&value.semantic)) {
        return annotation.dtype_name == "float32"
            ? FormatFloatingValue(static_cast<float>(*floating_value))
            : FormatFloatingValue(*floating_value);
    }
    return SampleAnnotationValueKey(value);
}

std::string_view SampleAnnotationKindLabel(SampleAnnotationKind kind)
{
    switch (kind) {
    case SampleAnnotationKind::CategoricalInteger:
        return "categorical";
    case SampleAnnotationKind::Text:
        return "text";
    case SampleAnnotationKind::ContinuousFloat:
        return "continuous";
    default:
        return "unknown";
    }
}

}  // namespace specforge
