#include "domain/sample_labeling_document_diagnostics.h"
#include "domain/canonical_timestamp.h"
#include "domain/sample_labeling_asdf_codec.h"
#include "domain/sample_labeling_asdf_store.h"
#include "domain/sample_labeling_document.h"
#include "sample_labeling_asdf_mutation_support.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace mutation = spectiary::asdf_mutation_test;

inline constexpr std::uint64_t kDefaultSeed = 0x78c0de2026ULL;
inline constexpr std::string_view kArtifactDirectoryEnvironment =
    "SPECTIARY_ASDF_MUTATION_ARTIFACT_DIRECTORY";

[[nodiscard]] std::filesystem::path DefaultArtifactDirectory()
{
    char* configured_value = nullptr;
    std::size_t configured_size = 0;
    const int environment_error = _dupenv_s(
        &configured_value,
        &configured_size,
        kArtifactDirectoryEnvironment.data());
    const std::unique_ptr<char, decltype(&std::free)> configured(
        configured_value, &std::free);
    if (environment_error == 0 && configured_size > 1U &&
        configured != nullptr) {
        return std::filesystem::path(configured.get());
    }
    return std::filesystem::current_path() /
        "asdf-labeling-mutation-reproducer";
}

struct Options {
    std::uint64_t seed = kDefaultSeed;
    bool seed_was_provided = false;
    std::size_t case_count = mutation::kDefaultMutationCaseCount;
    std::optional<std::size_t> case_index;
    std::filesystem::path base_fixture = "profile_zlib.asdf";
    std::optional<std::filesystem::path> replay_file;
    std::size_t maximum_input_bytes =
        mutation::kDefaultMaximumInputBytes;
    std::filesystem::path artifact_directory = DefaultArtifactDirectory();
    std::optional<std::size_t> inject_failure_case;
    std::optional<std::size_t> inject_abrupt_exit_case;
    bool show_help = false;
};

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path() /
            ("spectiary-asdf-mutation-" + std::to_string(nonce));
        std::error_code error;
        if (!std::filesystem::create_directories(path_, error) || error) {
            throw std::runtime_error(
                "could not create bounded mutation temporary directory");
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string PathText(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

[[nodiscard]] std::filesystem::path Utf8Path(std::string_view value)
{
    const std::u8string utf8(
        reinterpret_cast<const char8_t*>(value.data()), value.size());
    return std::filesystem::path(utf8);
}

[[nodiscard]] std::size_t ParseSize(
    std::string_view value,
    std::string_view option)
{
    if (value.empty() || value.front() == '-') {
        throw std::runtime_error(
            std::string(option) + " requires an unsigned integer");
    }
    std::size_t consumed = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(std::string(value), &consumed, 0);
    } catch (const std::exception&) {
        throw std::runtime_error(
            std::string(option) + " requires an unsigned integer");
    }
    if (consumed != value.size() ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(
            std::string(option) + " is outside the supported range");
    }
    return static_cast<std::size_t>(parsed);
}

[[nodiscard]] std::uint64_t ParseSeed(std::string_view value)
{
    if (value.empty() || value.front() == '-') {
        throw std::runtime_error("--seed requires an unsigned integer");
    }
    std::size_t consumed = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(std::string(value), &consumed, 0);
    } catch (const std::exception&) {
        throw std::runtime_error("--seed requires an unsigned integer");
    }
    if (consumed != value.size()) {
        throw std::runtime_error("--seed requires an unsigned integer");
    }
    return static_cast<std::uint64_t>(parsed);
}

[[nodiscard]] Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::runtime_error(
                "missing value after " + std::string(argument));
        }
        const std::string_view value = argv[++index];
        if (argument == "--seed") {
            options.seed = ParseSeed(value);
            options.seed_was_provided = true;
        } else if (argument == "--case-index") {
            options.case_index = ParseSize(value, argument);
        } else if (argument == "--base-fixture") {
            options.base_fixture = Utf8Path(value);
        } else if (argument == "--replay-file") {
            options.replay_file = Utf8Path(value);
        } else if (argument == "--case-count") {
            options.case_count = ParseSize(value, argument);
        } else if (argument == "--max-input-bytes") {
            options.maximum_input_bytes = ParseSize(value, argument);
        } else if (argument == "--artifact-directory") {
            options.artifact_directory = Utf8Path(value);
        } else if (argument == "--inject-failure-case") {
            options.inject_failure_case = ParseSize(value, argument);
        } else if (argument == "--inject-abrupt-exit-case") {
            options.inject_abrupt_exit_case = ParseSize(value, argument);
        } else {
            throw std::runtime_error(
                "unknown option: " + std::string(argument));
        }
    }
    if (options.case_count == 0U ||
        options.case_count > mutation::kMaximumMutationCaseCount) {
        throw std::runtime_error(
            "--case-count must be between 1 and " +
            std::to_string(mutation::kMaximumMutationCaseCount));
    }
    if (options.maximum_input_bytes == 0U ||
        options.maximum_input_bytes > mutation::kMaximumInputBytes) {
        throw std::runtime_error(
            "--max-input-bytes must be between 1 and " +
            std::to_string(mutation::kMaximumInputBytes));
    }
    if (options.replay_file &&
        (!options.seed_was_provided || !options.case_index)) {
        throw std::runtime_error(
            "--replay-file requires explicit --seed and --case-index from "
            "reproducer.json");
    }
    return options;
}

void PrintHelp()
{
    std::cout
        << "Bounded deterministic production ASDF mutation properties\n\n"
        << "  --seed UINT64\n"
        << "  --case-index UINT\n"
        << "  --base-fixture PATH-OR-FIXTURE-NAME\n"
        << "  --replay-file PATH  (requires explicit --seed and --case-index)\n"
        << "  --case-count UINT\n"
        << "  --max-input-bytes UINT\n"
        << "  --artifact-directory PATH\n"
        << "  --inject-failure-case UINT  (harness self-check only)\n"
        << "  --inject-abrupt-exit-case UINT  (harness self-check only)\n\n"
        << kArtifactDirectoryEnvironment
        << " sets the default failure artifact directory.\n";
}

[[nodiscard]] std::filesystem::path ResolveFixture(
    const std::filesystem::path& requested)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(requested, error) && !error) {
        return std::filesystem::absolute(requested);
    }
    const std::filesystem::path fixture =
        std::filesystem::path(SPECTIARY_ASDF_LABELING_FIXTURE_DIR) /
        requested;
    error.clear();
    if (!std::filesystem::is_regular_file(fixture, error) || error) {
        throw std::runtime_error(
            "base fixture is not a regular file: " +
            PathText(requested));
    }
    return fixture;
}

[[nodiscard]] std::vector<unsigned char> ReadBoundedFile(
    const std::filesystem::path& path,
    std::size_t maximum_bytes)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0U || size > maximum_bytes) {
        throw std::runtime_error(
            "input file is empty, unreadable, or exceeds --max-input-bytes: " +
            PathText(path));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input file: " + PathText(path));
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() !=
            static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error("could not read complete input file");
    }
    return bytes;
}

void WriteBytes(const std::filesystem::path& path,
    std::span<const unsigned char> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create mutation input file");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("could not flush mutation input file");
    }
}

[[nodiscard]] std::string JsonQuoted(std::string_view value)
{
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                output << "\\u00" << std::hex
                       << "0123456789abcdef"[(character >> 4U) & 0xfU]
                       << "0123456789abcdef"[character & 0xfU]
                       << std::dec;
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] std::string CodecErrorName(
    spectiary::SampleLabelingAsdfErrorKind kind)
{
    using Kind = spectiary::SampleLabelingAsdfErrorKind;
    switch (kind) {
    case Kind::None:
        return "None";
    case Kind::OpenFailed:
        return "OpenFailed";
    case Kind::IoFailure:
        return "IoFailure";
    case Kind::MalformedDocument:
        return "MalformedDocument";
    case Kind::UnsupportedProfile:
        return "UnsupportedProfile";
    case Kind::ResourceLimitExceeded:
        return "ResourceLimitExceeded";
    case Kind::SemanticValidationFailed:
        return "SemanticValidationFailed";
    }
    return "Unknown";
}

[[nodiscard]] spectiary::SampleLabelingSourceCompatibility CompatibleSource(
    const spectiary::SampleLabelingDocument& document)
{
    return spectiary::SampleLabelingSourceCompatibility{
        .base_identity = document.source.base_identity,
        .source_kind = document.source.kind,
        .source_name = document.source.name,
        .source_fingerprint = document.source.fingerprint,
        .sample_count = document.source.sample_count,
        .sample_names = document.source.roster.sample_names,
    };
}

struct UnknownMappingObservation {
    mutation::UnknownMappingExpectation expectation;
    std::optional<std::int32_t> label_code;
};

[[nodiscard]] YAML::Node ParseMetadataTree(
    std::span<const unsigned char> bytes)
{
    constexpr std::string_view terminator = "\n...\n";
    const auto marker = std::search(
        bytes.begin(), bytes.end(), terminator.begin(), terminator.end());
    if (marker == bytes.end()) {
        throw std::runtime_error(
            "unknown mapping verification found no YAML terminator");
    }
    const std::size_t metadata_size =
        static_cast<std::size_t>(marker - bytes.begin()) +
        terminator.size();
    try {
        YAML::Node root = YAML::Load(std::string(
            reinterpret_cast<const char*>(bytes.data()), metadata_size));
        if (!root || !root.IsMap()) {
            throw std::runtime_error(
                "unknown mapping metadata root is not a map");
        }
        return root;
    } catch (const YAML::Exception& error) {
        throw std::runtime_error(
            "unknown mapping metadata could not be parsed: " +
            std::string(error.what()));
    }
}

[[nodiscard]] YAML::Node RequireMapChild(const YAML::Node& parent,
    std::string_view key,
    std::string_view path)
{
    if (!parent || !parent.IsMap()) {
        throw std::runtime_error(
            "unknown mapping parent is not a map at " + std::string(path));
    }
    const YAML::Node child = parent[std::string(key)];
    if (!child || !child.IsMap()) {
        throw std::runtime_error(
            "unknown mapping path is absent or not a map at " +
            std::string(path));
    }
    return child;
}

[[nodiscard]] bool UnknownValueMatches(
    const YAML::Node& value,
    const mutation::UnknownMappingExpectation& expectation)
{
    if (!value) {
        return false;
    }
    if (!expectation.mapping_value) {
        return value.IsScalar() && value.Scalar() == expectation.token;
    }
    if (!value.IsMap() || value.size() != 1U) {
        return false;
    }
    const YAML::Node token = value["token"];
    return token && token.IsScalar() &&
        token.Scalar() == expectation.token;
}

[[nodiscard]] YAML::Node UnknownParentForNode(const YAML::Node& root,
    std::string_view node)
{
    if (node == "root") {
        return root;
    }
    if (node == "source") {
        return RequireMapChild(
            root, "source_collection", "source_collection");
    }
    if (node == "annotation") {
        return RequireMapChild(root, "annotation", "annotation");
    }
    const YAML::Node task =
        RequireMapChild(root, "labeling_task", "labeling_task");
    if (node == "task") {
        return task;
    }
    const YAML::Node origin =
        RequireMapChild(task, "origin", "labeling_task.origin");
    if (node == "origin") {
        return origin;
    }
    if (node == "origin_annotation") {
        return RequireMapChild(origin,
            "annotation",
            "labeling_task.origin.annotation");
    }
    throw std::runtime_error(
        "unknown mapping expectation has an unsupported node: " +
        std::string(node));
}

[[nodiscard]] std::vector<UnknownMappingObservation>
CaptureUnknownMappings(
    std::span<const unsigned char> bytes,
    const std::vector<mutation::UnknownMappingExpectation>& expectations)
{
    const YAML::Node root = ParseMetadataTree(bytes);
    std::vector<UnknownMappingObservation> observations;
    observations.reserve(expectations.size());
    for (const mutation::UnknownMappingExpectation& expectation :
        expectations) {
        UnknownMappingObservation observation{
            .expectation = expectation,
        };
        if (expectation.node != "label") {
            const YAML::Node parent =
                UnknownParentForNode(root, expectation.node);
            if (!UnknownValueMatches(
                    parent[expectation.key], expectation)) {
                throw std::runtime_error(
                    "unknown mapping has the wrong key, kind, or value at " +
                    expectation.node + "." + expectation.key);
            }
        } else {
            const YAML::Node task = RequireMapChild(
                root, "labeling_task", "labeling_task");
            const YAML::Node labels = task["labels"];
            if (!labels || !labels.IsSequence()) {
                throw std::runtime_error(
                    "unknown label mapping has no labels sequence");
            }
            for (const YAML::Node& label : labels) {
                if (!label.IsMap() ||
                    !UnknownValueMatches(
                        label[expectation.key], expectation)) {
                    continue;
                }
                if (observation.label_code) {
                    throw std::runtime_error(
                        "unknown label mapping token is not unique");
                }
                try {
                    const YAML::Node code = label["code"];
                    if (!code || !code.IsScalar()) {
                        throw std::runtime_error(
                            "unknown label mapping has no stable code");
                    }
                    observation.label_code = code.as<std::int32_t>();
                } catch (const YAML::Exception& error) {
                    throw std::runtime_error(
                        "unknown label mapping stable code is invalid: " +
                        std::string(error.what()));
                }
            }
            if (!observation.label_code) {
                throw std::runtime_error(
                    "unknown label mapping is absent at its stable code");
            }
        }
        observations.push_back(std::move(observation));
    }
    return observations;
}

void VerifyUnknownMappings(
    std::span<const unsigned char> bytes,
    const std::vector<UnknownMappingObservation>& observations)
{
    const YAML::Node root = ParseMetadataTree(bytes);
    for (const UnknownMappingObservation& observation : observations) {
        const mutation::UnknownMappingExpectation& expectation =
            observation.expectation;
        if (expectation.node != "label") {
            const YAML::Node parent =
                UnknownParentForNode(root, expectation.node);
            if (!UnknownValueMatches(
                    parent[expectation.key], expectation)) {
                throw std::runtime_error(
                    "metadata rewrite changed unknown mapping key, kind, "
                    "value, or path at " +
                    expectation.node + "." + expectation.key);
            }
            continue;
        }
        const YAML::Node task = RequireMapChild(
            root, "labeling_task", "labeling_task");
        const YAML::Node labels = task["labels"];
        bool matched = false;
        if (labels && labels.IsSequence()) {
            for (const YAML::Node& label : labels) {
                try {
                    const YAML::Node code = label["code"];
                    if (!label.IsMap() || !code || !code.IsScalar() ||
                        code.as<std::int32_t>() !=
                            *observation.label_code) {
                        continue;
                    }
                } catch (const YAML::Exception&) {
                    continue;
                }
                matched = UnknownValueMatches(
                    label[expectation.key], expectation);
                break;
            }
        }
        if (!matched) {
            throw std::runtime_error(
                "metadata rewrite changed unknown label mapping at stable "
                "code " +
                std::to_string(*observation.label_code));
        }
    }
}

[[nodiscard]] bool DocumentsEqual(
    const spectiary::SampleLabelingDocument& left,
    const spectiary::SampleLabelingDocument& right)
{
    if (left.format_kind != right.format_kind ||
        left.schema_version != right.schema_version ||
        left.source.base_identity != right.source.base_identity ||
        left.source.kind != right.source.kind ||
        left.source.name != right.source.name ||
        left.source.fingerprint != right.source.fingerprint ||
        left.source.sample_count != right.source.sample_count ||
        left.source.roster.identity_kind !=
            right.source.roster.identity_kind ||
        left.source.roster.sample_names !=
            right.source.roster.sample_names ||
        left.annotation.kind != right.annotation.kind ||
        left.annotation.missing.semantic !=
            right.annotation.missing.semantic ||
        left.annotation.missing.value != right.annotation.missing.value ||
        left.annotation.values != right.annotation.values ||
        left.labeling.id != right.labeling.id ||
        left.labeling.name != right.labeling.name ||
        left.labeling.canonical_metadata !=
            right.labeling.canonical_metadata ||
        left.labeling.labels.size() != right.labeling.labels.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.labeling.labels.size();
         ++index) {
        const spectiary::SampleLabelingDocumentLabel& left_label =
            left.labeling.labels[index];
        const spectiary::SampleLabelingDocumentLabel& right_label =
            right.labeling.labels[index];
        if (left_label.code != right_label.code ||
            left_label.name != right_label.name ||
            left_label.shortcut != right_label.shortcut) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<unsigned char> BuildCanonicalMutationSeed(
    const std::filesystem::path& fixture,
    const std::filesystem::path& verification_path,
    std::size_t maximum_bytes)
{
    const spectiary::SampleLabelingAsdfReadResult loaded =
        spectiary::ReadSampleLabelingAsdfDocument(fixture);
    if (!loaded.succeeded()) {
        throw std::runtime_error(
            "production reader rejected base fixture: " +
            CodecErrorName(loaded.error.kind) + ": " + loaded.error.message);
    }

    spectiary::SampleLabelingDocument document = *loaded.document;
    if (document.source.sample_count < 3U) {
        document.source.sample_count = 3U;
        document.annotation.values.assign(3U, -1);
    }
    document.source.roster.identity_kind =
        std::string(spectiary::kSampleLabelingDocumentExplicitNamesRoster);
    if (document.source.roster.sample_names.size() !=
        document.source.sample_count) {
        document.source.roster.sample_names.clear();
        document.source.roster.sample_names.reserve(
            document.source.sample_count);
        for (std::size_t index = 0;
             index < document.source.sample_count;
             ++index) {
            document.source.roster.sample_names.push_back(
                "mutation-sample-" + std::to_string(index));
        }
    }
    if (document.labeling.labels.empty()) {
        document.labeling.labels.push_back({0, "mutation-label", ""});
    }
    document.labeling.canonical_metadata.origin.kind =
        "annotation_promotion";
    document.labeling.canonical_metadata.origin.annotation =
        spectiary::SampleLabelingAnnotationOrigin{
            .name = "mutation-seed.csv",
            .format = "csv",
        };

    const spectiary::SampleLabelingDocumentValidationResult validation =
        spectiary::diagnostics::ValidateSampleLabelingDocument(document);
    if (!validation.valid()) {
        throw std::runtime_error(
            "normalized mutation seed failed canonical validation");
    }
    std::ostringstream output(std::ios::binary | std::ios::out);
    const spectiary::SampleLabelingAsdfWriteResult written =
        spectiary::WriteSampleLabelingAsdfDocument(output, document);
    if (!written.succeeded()) {
        throw std::runtime_error(
            "production writer rejected normalized mutation seed: " +
            CodecErrorName(written.error.kind) + ": " +
            written.error.message);
    }
    const std::string serialized = output.str();
    std::vector<unsigned char> bytes(serialized.begin(), serialized.end());
    mutation::RebuildBlockIndex(bytes);
    if (bytes.size() > maximum_bytes) {
        throw std::runtime_error(
            "canonical mutation seed exceeds --max-input-bytes");
    }
    WriteBytes(verification_path, bytes);
    const spectiary::SampleLabelingAsdfReadResult verified =
        spectiary::ReadSampleLabelingAsdfDocument(verification_path);
    if (!verified.succeeded() || !verified.durable_base ||
        !spectiary::diagnostics::ValidateSampleLabelingDocument(*verified.document)
             .valid()) {
        throw std::runtime_error(
            "production reader did not accept the canonical durable seed");
    }
    return bytes;
}

void VerifyInvalidWriterEmitsNoBytes(
    const spectiary::SampleLabelingDocument& canonical_seed)
{
    spectiary::SampleLabelingDocument invalid = canonical_seed;
    ++invalid.source.sample_count;
    std::ostringstream output(std::ios::binary | std::ios::out);
    const spectiary::SampleLabelingAsdfWriteResult result =
        spectiary::WriteSampleLabelingAsdfDocument(output, invalid);
    if (result.succeeded() ||
        result.error.kind == spectiary::SampleLabelingAsdfErrorKind::None ||
        !output.str().empty()) {
        throw std::runtime_error(
            "invalid production writer input did not fail before byte zero");
    }
}

[[nodiscard]] std::string EvaluateCase(
    const mutation::GeneratedMutationCase& generated,
    const std::filesystem::path& case_path)
{
    WriteBytes(case_path, generated.bytes);
    const spectiary::SampleLabelingAsdfReadResult read =
        spectiary::ReadSampleLabelingAsdfDocument(case_path);
    if (!read.succeeded()) {
        if (read.error.kind == spectiary::SampleLabelingAsdfErrorKind::None) {
            throw std::runtime_error(
                "production reader rejected with error kind None");
        }
        if (generated.recipe.expected ==
            mutation::ExpectedInvariant::MustAcceptAndPreserveUnknown) {
            throw std::runtime_error(
                "unknown mapping mutation was rejected: " +
                CodecErrorName(read.error.kind) + ": " + read.error.message);
        }
        if (generated.recipe.expected ==
                mutation::ExpectedInvariant::MustRejectResourceLimit &&
            read.error.kind !=
                spectiary::SampleLabelingAsdfErrorKind::
                    ResourceLimitExceeded) {
            throw std::runtime_error(
                "resource-limit mutation rejected with " +
                CodecErrorName(read.error.kind) + " instead of "
                "ResourceLimitExceeded: " + read.error.message);
        }
        return "rejected:" + CodecErrorName(read.error.kind);
    }

    if (generated.recipe.expected ==
            mutation::ExpectedInvariant::MustReject ||
        generated.recipe.expected ==
            mutation::ExpectedInvariant::MustRejectResourceLimit) {
        throw std::runtime_error(
            "mutation marked MustReject was accepted");
    }
    if (!spectiary::diagnostics::ValidateSampleLabelingDocument(*read.document).valid()) {
        throw std::runtime_error(
            "accepted mutation failed canonical document validation");
    }

    const spectiary::SampleLabelingSourceCompatibility source =
        CompatibleSource(*read.document);
    spectiary::SampleLabelingAsdfStoreOpenResult opened =
        spectiary::OpenSampleLabelingAsdfDocumentStore(case_path, source);
    if (!read.durable_base) {
        if (opened.succeeded() ||
            opened.error.kind !=
                spectiary::SampleLabelingAsdfStoreErrorKind::
                    DurableBaseUnavailable) {
            throw std::runtime_error(
                "accepted compatibility profile without a durable base did not return DurableBaseUnavailable");
        }
        return "accepted:canonical:nondurable";
    }
    if (!opened.succeeded()) {
        throw std::runtime_error(
            "durable accepted mutation did not open through production store: " +
            opened.error.message);
    }

    if (generated.recipe.expected ==
        mutation::ExpectedInvariant::MustAcceptAndPreserveUnknown) {
        const std::vector<UnknownMappingObservation> unknown_observations =
            CaptureUnknownMappings(
                generated.bytes, generated.unknown_mappings);
        spectiary::SampleLabelingDocument replacement =
            opened.snapshot->document();
        replacement.labeling.name += " mutation rewrite";
        const auto modified = spectiary::CanonicalTimestamp::FromTimePoint(
            replacement.labeling.canonical_metadata.modified_at.time_point() +
            std::chrono::milliseconds{1});
        if (!modified) {
            throw std::runtime_error(
                "mutation rewrite timestamp fixture did not parse");
        }
        replacement.labeling.canonical_metadata.modified_at = *modified;
        const spectiary::SampleLabelingSourceCompatibility rewrite_source =
            CompatibleSource(replacement);
        const spectiary::SampleLabelingAsdfStoreGenerationWriteResult rewritten =
            spectiary::RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                *opened.snapshot,
                replacement,
                rewrite_source);
        if (!rewritten.succeeded() || !rewritten.document_replaced ||
            !DocumentsEqual(
                rewritten.snapshot->document(), replacement)) {
            throw std::runtime_error(
                "unknown mapping metadata rewrite failed or reopened the "
                "wrong replacement document: " +
                rewritten.error.message);
        }
        const spectiary::SampleLabelingAsdfReadResult reread =
            spectiary::ReadSampleLabelingAsdfDocument(case_path);
        if (!reread.succeeded() ||
            !spectiary::diagnostics::ValidateSampleLabelingDocument(*reread.document)
                 .valid() ||
            !DocumentsEqual(*reread.document, replacement)) {
            throw std::runtime_error(
                "rewritten unknown mapping document did not read as the "
                "expected canonical replacement");
        }
        const std::vector<unsigned char> rewritten_bytes =
            ReadBoundedFile(case_path, mutation::kMaximumInputBytes);
        VerifyUnknownMappings(rewritten_bytes, unknown_observations);
        return "accepted:canonical:unknown-preserved";
    }
    return "accepted:canonical:durable";
}

void WriteReproducer(const Options& options,
    const std::filesystem::path& base_fixture,
    std::size_t case_index,
    const mutation::GeneratedMutationCase& generated,
    std::string_view evaluation_state,
    std::optional<std::string_view> actual,
    std::optional<std::string_view> reason,
    bool announce)
{
    std::error_code error;
    std::filesystem::create_directories(
        options.artifact_directory, error);
    if (error) {
        throw std::runtime_error(
            "could not create reproducer artifact directory");
    }
    const std::filesystem::path asdf_path =
        options.artifact_directory / "reproducer.asdf";
    const std::filesystem::path json_path =
        options.artifact_directory / "reproducer.json";
    WriteBytes(asdf_path, generated.bytes);

    std::ofstream json(json_path, std::ios::binary | std::ios::trunc);
    if (!json) {
        throw std::runtime_error("could not create reproducer.json");
    }
    json << "{\n"
         << "  \"format_kind\": \"spectiary.asdf_mutation_reproducer\",\n"
         << "  \"schema_version\": 1,\n"
         << "  \"base_fixture\": " << JsonQuoted(PathText(base_fixture))
         << ",\n"
         << "  \"seed\": " << JsonQuoted(std::to_string(options.seed))
         << ",\n"
         << "  \"case_index\": " << case_index << ",\n"
         << "  \"category\": "
         << JsonQuoted(mutation::CategoryName(generated.recipe.category))
         << ",\n"
         << "  \"recipe\": " << JsonQuoted(generated.recipe.name) << ",\n"
         << "  \"mutation_chain\": [";
    for (std::size_t index = 0;
         index < generated.mutation_chain.size();
         ++index) {
        json << (index == 0U ? "" : ", ")
             << JsonQuoted(generated.mutation_chain[index]);
    }
    json << "],\n"
         << "  \"unknown_tokens\": [";
    for (std::size_t index = 0;
         index < generated.unknown_tokens.size();
         ++index) {
        json << (index == 0U ? "" : ", ")
             << JsonQuoted(generated.unknown_tokens[index]);
    }
    json << "],\n"
         << "  \"unknown_mappings\": [";
    for (std::size_t index = 0;
         index < generated.unknown_mappings.size();
         ++index) {
        const mutation::UnknownMappingExpectation& expectation =
            generated.unknown_mappings[index];
        json << (index == 0U ? "" : ", ")
             << "{\"node\":" << JsonQuoted(expectation.node)
             << ",\"key\":" << JsonQuoted(expectation.key)
             << ",\"value_kind\":"
             << JsonQuoted(expectation.mapping_value ? "mapping" : "scalar")
             << ",\"token\":" << JsonQuoted(expectation.token) << '}';
    }
    json << "],\n"
         << "  \"expected_invariant\": "
         << JsonQuoted(
                mutation::ExpectedInvariantName(generated.recipe.expected))
         << ",\n"
         << "  \"evaluation_state\": "
         << JsonQuoted(evaluation_state) << ",\n"
         << "  \"actual_result\": ";
    if (actual) {
        json << JsonQuoted(*actual);
    } else {
        json << "null";
    }
    json << ",\n"
         << "  \"failure\": ";
    if (reason) {
        json << JsonQuoted(*reason);
    } else {
        json << "null";
    }
    json << ",\n"
         << "  \"input_size_bytes\": " << generated.bytes.size() << ",\n"
         << "  \"max_input_bytes\": " << options.maximum_input_bytes
         << ",\n"
         << "  \"input_fingerprint_fnv1a64\": "
         << JsonQuoted(mutation::InputFingerprint(generated.bytes)) << ",\n"
         << "  \"replay_arguments\": [\"--seed\", "
         << JsonQuoted(std::to_string(options.seed))
         << ", \"--case-index\", " << JsonQuoted(std::to_string(case_index))
         << ", \"--replay-file\", \"reproducer.asdf\", "
         << "\"--max-input-bytes\", "
         << JsonQuoted(std::to_string(options.maximum_input_bytes)) << "]\n"
         << "}\n";
    json.close();
    if (!json) {
        throw std::runtime_error("could not flush reproducer.json");
    }
    if (announce) {
        std::cerr << "reproducer_asdf=" << PathText(asdf_path) << '\n'
                  << "reproducer_json=" << PathText(json_path) << '\n';
    }
}

void RemoveCompletedReproducer(const Options& options)
{
    const std::array paths{
        options.artifact_directory / "reproducer.asdf",
        options.artifact_directory / "reproducer.json",
    };
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        std::filesystem::remove(path, error);
        if (error) {
            throw std::runtime_error(
                "could not remove completed mutation case artifact: " +
                PathText(path));
        }
    }
}

[[nodiscard]] std::size_t CategoryIndex(
    mutation::MutationCategory category)
{
    switch (category) {
    case mutation::MutationCategory::YamlStructure:
        return 0U;
    case mutation::MutationCategory::YamlScalar:
        return 1U;
    case mutation::MutationCategory::BlockHeader:
        return 2U;
    case mutation::MutationCategory::BlockIndex:
        return 3U;
    case mutation::MutationCategory::Payload:
        return 4U;
    case mutation::MutationCategory::UnknownMetadata:
        return 5U;
    }
    throw std::runtime_error("unknown mutation category");
}

void PopulateReplayUnknownTokens(mutation::GeneratedMutationCase& generated,
    std::uint64_t seed,
    std::size_t case_index)
{
    if (generated.recipe.category !=
        mutation::MutationCategory::UnknownMetadata) {
        return;
    }
    const std::size_t variant = generated.recipe.variant;
    const auto add = [&](std::string_view node, std::string_view key) {
        const std::string token = mutation::Token(seed, case_index, node);
        generated.unknown_tokens.push_back(token);
        generated.unknown_mappings.push_back(
            mutation::UnknownMappingExpectation{
                .node = std::string(node),
                .key = std::string(key),
                .token = token,
                .mapping_value = variant == 5U,
            });
    };
    if (variant == 0U || variant >= 4U) {
        add("root", "mutation_unknown_root");
        add("source", "mutation_unknown_source");
    }
    if (variant == 1U || variant >= 4U) {
        add("annotation", "mutation_unknown_annotation");
        add("task", "mutation_unknown_task");
    }
    if (variant == 2U || variant >= 4U) {
        add("origin", "mutation_unknown_origin");
        add("origin_annotation", "mutation_unknown_origin_annotation");
    }
    if (variant == 3U || variant >= 4U) {
        add("label", "mutation_unknown_label");
    }
}

int Run(const Options& options)
{
    TemporaryDirectory temporary;
    const std::filesystem::path base_fixture =
        ResolveFixture(options.base_fixture);
    std::vector<unsigned char> seed_bytes;
    if (!options.replay_file) {
        seed_bytes = BuildCanonicalMutationSeed(base_fixture,
            temporary.path() / "canonical-seed.asdf",
            options.maximum_input_bytes);
    }

    spectiary::SampleLabelingDocument canonical_document;
    if (!options.replay_file) {
        const spectiary::SampleLabelingAsdfReadResult canonical =
            spectiary::ReadSampleLabelingAsdfDocument(
                temporary.path() / "canonical-seed.asdf");
        if (!canonical.succeeded()) {
            throw std::runtime_error(
                "canonical seed became unreadable before corpus execution");
        }
        canonical_document = *canonical.document;
    }

    const std::size_t first_case = options.case_index.value_or(0U);
    const std::size_t executions =
        options.case_index || options.replay_file ? 1U : options.case_count;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::array<std::size_t, 6> categories{};

    for (std::size_t offset = 0; offset < executions; ++offset) {
        const std::size_t case_index = first_case + offset;
        mutation::GeneratedMutationCase generated;
        if (options.replay_file) {
            generated.bytes = ReadBoundedFile(
                *options.replay_file, options.maximum_input_bytes);
            generated.recipe = mutation::kMutationRecipes[(case_index +
                static_cast<std::size_t>(
                    options.seed % mutation::kMutationRecipes.size())) %
                mutation::kMutationRecipes.size()];
            generated.mutation_chain = {
                "replay-file:" + PathText(*options.replay_file),
            };
            PopulateReplayUnknownTokens(
                generated, options.seed, case_index);
        } else {
            generated = mutation::GenerateMutationCase(seed_bytes,
                options.seed,
                case_index,
                options.maximum_input_bytes);
            const mutation::GeneratedMutationCase repeated =
                mutation::GenerateMutationCase(seed_bytes,
                    options.seed,
                    case_index,
                    options.maximum_input_bytes);
            if (generated.bytes != repeated.bytes ||
                generated.recipe.name != repeated.recipe.name ||
                generated.mutation_chain != repeated.mutation_chain ||
                generated.unknown_tokens != repeated.unknown_tokens ||
                generated.unknown_mappings != repeated.unknown_mappings) {
                throw std::runtime_error(
                    "same seed and case index did not reproduce identical input");
            }
        }

        std::string actual = "not-evaluated";
        try {
            // Keep the one current input outside the temporary tree before
            // any production API can abort, hang, or be killed by CTest.
            WriteReproducer(options,
                base_fixture,
                case_index,
                generated,
                "pending",
                std::nullopt,
                std::nullopt,
                false);
            if (options.inject_abrupt_exit_case == case_index) {
                std::_Exit(86);
            }
            if (!options.replay_file) {
                VerifyInvalidWriterEmitsNoBytes(canonical_document);
            }
            const std::filesystem::path case_path = temporary.path() /
                ("case-" + std::to_string(case_index) + ".asdf");
            actual = EvaluateCase(generated, case_path);
            if (actual.starts_with("accepted:")) {
                ++accepted;
            } else {
                ++rejected;
            }
            ++categories[CategoryIndex(generated.recipe.category)];
            if (options.inject_failure_case == case_index) {
                throw std::runtime_error(
                    "injected harness assertion failure");
            }
            RemoveCompletedReproducer(options);
        } catch (const std::exception& exception) {
            WriteReproducer(options,
                base_fixture,
                case_index,
                generated,
                "failed",
                std::string_view(actual),
                std::string_view(exception.what()),
                true);
            std::cerr << "case_index=" << case_index
                      << " recipe=" << generated.recipe.name
                      << " failure=" << exception.what() << '\n';
            return 1;
        } catch (...) {
            WriteReproducer(options,
                base_fixture,
                case_index,
                generated,
                "failed",
                std::string_view(actual),
                std::string_view("non-standard exception"),
                true);
            std::cerr << "case_index=" << case_index
                      << " failure=non-standard exception\n";
            return 1;
        }
    }

    std::cout << "{\"success\":true,\"seed\":" << options.seed
              << ",\"first_case_index\":" << first_case
              << ",\"case_count\":" << executions
              << ",\"accepted\":" << accepted
              << ",\"rejected\":" << rejected
              << ",\"max_input_bytes\":" << options.maximum_input_bytes
              << ",\"categories\":{"
              << "\"yaml_structure\":" << categories[0] << ','
              << "\"yaml_scalar\":" << categories[1] << ','
              << "\"block_header\":" << categories[2] << ','
              << "\"block_index\":" << categories[3] << ','
              << "\"payload\":" << categories[4] << ','
              << "\"unknown_metadata\":" << categories[5]
              << "}}\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = ParseOptions(argc, argv);
        if (options.show_help) {
            PrintHelp();
            return 0;
        }
        return Run(options);
    } catch (const std::exception& exception) {
        std::cerr << "mutation corpus setup failed: " << exception.what()
                  << '\n';
        return 2;
    } catch (...) {
        std::cerr << "mutation corpus setup failed: non-standard exception\n";
        return 2;
    }
}
