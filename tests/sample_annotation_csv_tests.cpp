#include "legacy_annotation_fixture_io.h"
#include "domain/csv_record_codec.h"
#include "domain/sample_annotation_io.h"
#include "domain/sample_filter.h"
#include "domain/sample_label_export.h"
#include "domain/sample_labeling_source_compatibility.h"
#include "platform/file_sha256.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path FreshTestDirectory(std::string_view name)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::string(name);
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path);
    return path;
}

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : original_(std::filesystem::current_path())
    {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath()
    {
        std::error_code error;
        std::filesystem::current_path(original_, error);
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

private:
    std::filesystem::path original_;
};

void WriteCsv(
    const std::filesystem::path& path,
    const std::vector<spectiary::CsvRecord>& records)
{
    const spectiary::CsvRecordWriteResult result =
        spectiary::WriteCsvRecordsAtomically(
            path,
            records);
    Require(
        result.succeeded(),
        result.error.message.empty()
            ? "CSV fixture should write"
            : result.error.message);
}

std::vector<std::string> TextValues(
    const spectiary::SampleAnnotationResult& annotation)
{
    std::vector<std::string> values;
    values.reserve(annotation.values.size());
    for (const spectiary::SampleAnnotationValue& value :
         annotation.values) {
        const std::string* text =
            std::get_if<std::string>(&value.semantic);
        Require(
            text != nullptr,
            "CSV annotation values should be text");
        values.push_back(*text);
    }
    return values;
}

std::optional<spectiary::SampleAnnotationResult> LoadCsv(
    const std::filesystem::path& path,
    std::string_view source_kind,
    const std::vector<std::string>& sample_names,
    std::size_t sample_count,
    std::string* error)
{
    return spectiary::test_support::LegacyFixtureIo{}.
        LoadForSource(
            path,
            spectiary::SampleAnnotationSourceCompatibility{
                .base_identity = "csv-test-source",
                .source_kind = source_kind,
                .source_name = "source",
                .source_fingerprint = "fingerprint",
                .sample_count = sample_count,
                .sample_names = sample_names,
            },
            error);
}

std::optional<spectiary::SampleAnnotationResult> LoadCsvCancelable(
    const std::filesystem::path& path,
    std::size_t sample_count,
    const spectiary::SampleAnnotationCancellationCheckpoint& checkpoint,
    std::string* error)
{
    return spectiary::test_support::LegacyFixtureIo{}.
        LoadForSourceCancelable(
            path,
            spectiary::SampleAnnotationSourceCompatibility{
                .base_identity = "csv-test-source",
                .source_kind = "npy",
                .source_name = "source",
                .source_fingerprint = "fingerprint",
                .sample_count = sample_count,
            },
            checkpoint,
            error);
}

void TestNamedCsvMapsRowsToCanonicalRoster()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "spectiary-named-sample-annotation-csv");
    const std::filesystem::path path =
        directory / "labels.csv";
    WriteCsv(
        path,
        {
            {"sample", "label"},
            {"gamma", "line\nbreak"},
            {"alpha", "A"},
            {"beta", "\\unlabeled"},
        });

    const std::vector<std::string> sample_names{
        "alpha",
        "beta",
        "gamma",
    };
    std::string error;
    const std::optional<spectiary::SampleAnnotationResult>
        annotation = LoadCsv(
            path,
            "npy",
            sample_names,
            sample_names.size(),
            &error);
    Require(
        annotation.has_value(),
        error.empty()
            ? "named CSV annotation should load"
            : error);
    Require(
        annotation->kind ==
                spectiary::SampleAnnotationKind::Text &&
            annotation->dtype_name == "utf8" &&
            annotation->relationship ==
                spectiary::
                    SampleAnnotationWorkflowRelationship::
                        PlainAnnotation &&
            !annotation->label_metadata.has_value() &&
            annotation->labeling_document == nullptr,
        "CSV ingestion should create only a plain text annotation");
    const std::optional<std::string> artifact_digest =
        spectiary::ComputeFileSha256(path);
    Require(
        artifact_digest.has_value() &&
            annotation->artifact_provenance.has_value() &&
            annotation->artifact_provenance->name == "labels.csv" &&
            annotation->artifact_provenance->format == "csv" &&
            annotation->artifact_provenance->fingerprint ==
                std::optional<std::string>{
                    "sha256:" + *artifact_digest},
        "CSV ingestion should capture portable display-name, format, and full-artifact SHA-256 provenance");
    Require(
        TextValues(*annotation) ==
            std::vector<std::string>({
                "A",
                "unlabeled",
                "line\nbreak",
            }),
        "named CSV rows should be reordered into canonical source order and decode stable label text");

    error.clear();
    Require(
        !spectiary::test_support::LegacyFixtureIo{}.
             Load(path, sample_names.size(), &error) &&
            error.find("source collection") !=
                std::string::npos,
        "count-only loading must reject source-aware CSV annotations");
}

void TestFolderExportReloadsWithoutCanonicalTaskProvenance()
{
    spectiary::SampleLabelingTask task =
        spectiary::CreateSampleLabelingTask(
            "quality",
            "Quality",
            2);
    Require(
        spectiary::UpsertSampleLabel(
            task.label_set,
            spectiary::SampleLabelDefinition{
                5,
                "selected",
                's'}) &&
            spectiary::AssignSampleLabel(
                task,
                0,
                5)
                .accepted,
        "folder CSV round-trip fixture should assign a label");
    const spectiary::
        SampleLabelingCanonicalSourceDescriptor source{
            .base_identity = "folder-source",
            .source_kind = "folder",
            .source_name = "spectra",
            .source_fingerprint = "folder-fingerprint",
            .sample_count = 2,
            .sample_names = {
                "zeta.fits",
                "alpha.fits",
            },
        };
    std::string error;
    const std::optional<spectiary::SampleLabelExportSnapshot>
        snapshot = spectiary::BuildSampleLabelExportSnapshot(
            spectiary::SampleLabelExportFormat::Csv,
            task,
            source,
            &error);
    Require(
        snapshot.has_value(),
        error.empty()
            ? "folder CSV export snapshot should build"
            : error);
    const std::filesystem::path directory =
        FreshTestDirectory(
            "spectiary-folder-sample-annotation-csv");
    const std::filesystem::path path =
        directory / "labels.csv";
    Require(
        spectiary::ExportSampleLabelSnapshot(
            path,
            *snapshot,
            &error),
        error.empty()
            ? "folder CSV export should write"
            : error);

    const std::optional<spectiary::SampleAnnotationResult>
        annotation = LoadCsv(
            path,
            "folder",
            source.sample_names,
            source.sample_count,
            &error);
    Require(
        annotation.has_value(),
        error.empty()
            ? "exported folder CSV should reload"
            : error);
    Require(
        TextValues(*annotation) ==
            std::vector<std::string>({
                "selected",
                "unlabeled",
            }) &&
            annotation->relationship ==
                spectiary::
                    SampleAnnotationWorkflowRelationship::
                        PlainAnnotation &&
            !annotation->label_metadata &&
            !annotation->labeling_document,
        "CSV reload should preserve annotation values without inventing labeling task provenance");
}

void TestNpyLoadCapturesPortableArtifactProvenance()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "spectiary-sample-annotation-npy-provenance");
    const std::filesystem::path path =
        directory / "selected-labels.npy";
    const std::vector<int> values{1, 2, 3};
    std::string error;
    Require(
        spectiary::ExportLabelValuesToNpy(path, values, &error),
        error.empty()
            ? "NPY provenance fixture should write"
            : error);

    const std::optional<spectiary::SampleAnnotationResult> annotation =
        spectiary::test_support::LegacyFixtureIo{}.Load(
            path,
            values.size(),
            &error);
    const std::optional<std::string> artifact_digest =
        spectiary::ComputeFileSha256(path);
    Require(
        annotation.has_value() &&
            artifact_digest.has_value() &&
            annotation->artifact_provenance.has_value() &&
            annotation->artifact_provenance->name ==
                "selected-labels.npy" &&
            annotation->artifact_provenance->format == "npy" &&
            annotation->artifact_provenance->fingerprint ==
                std::optional<std::string>{
                    "sha256:" + *artifact_digest},
        error.empty()
            ? "NPY ingestion should capture portable display-name, format, and full-artifact SHA-256 provenance"
            : error);
}

void TestNpySidecarTaskNameCannotLeakPathsIntoArtifactProvenance()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "spectiary-sample-annotation-npy-portable-provenance-name");
    const std::filesystem::path path =
        directory / "selected-labels.npy";
    const std::vector<int> values{1, 2, 3};
    std::string error;
    Require(
        spectiary::ExportLabelValuesToNpy(path, values, &error),
        error.empty()
            ? "NPY portable provenance fixture should write"
            : error);

    spectiary::SampleLabelingTask task =
        spectiary::CreateSampleLabelingTask(
            "portable-provenance-task",
            "initial",
            values.size());
    const std::vector<std::string> nonportable_names{
        "/home/user/labels.npy",
        R"(C:\labels.npy)",
        R"(\\server\share\labels.npy)",
        "nested/labels.npy",
        R"(nested\labels.npy)",
        R"(C:labels.npy)",
    };
    for (const std::string& nonportable_name : nonportable_names) {
        task.task_name = nonportable_name;
        error.clear();
        Require(
            spectiary::test_support::LegacyFixtureIo{}.
                SaveLabelMetadata(path, task, nullptr, &error),
            error.empty()
                ? "NPY nonportable sidecar fixture should write"
                : error);

        const std::optional<spectiary::SampleAnnotationResult> annotation =
            spectiary::test_support::LegacyFixtureIo{}.
                Load(path, values.size(), &error);
        Require(
            annotation.has_value(),
            error.empty()
                ? "NPY nonportable sidecar fixture should load"
                : error);
        Require(
            annotation->artifact_provenance.has_value() &&
                annotation->artifact_provenance->name ==
                    "selected-labels.npy",
            "NPY provenance must replace absolute paths and directory components with the artifact basename");
    }

    task.task_name = "初始标签-😀.npy";
    error.clear();
    Require(
        spectiary::test_support::LegacyFixtureIo{}.
            SaveLabelMetadata(path, task, nullptr, &error),
        error.empty()
            ? "NPY Unicode sidecar fixture should write"
            : error);
    const std::optional<spectiary::SampleAnnotationResult> annotation =
        spectiary::test_support::LegacyFixtureIo{}.
            Load(path, values.size(), &error);
    Require(
        annotation.has_value() &&
            annotation->artifact_provenance.has_value() &&
            annotation->artifact_provenance->name ==
                "初始标签-😀.npy",
        "a valid Unicode portable display name should remain preferred provenance");
}

void TestCsvProvenanceUsesTheParsedFileGeneration()
{
    const std::filesystem::path root =
        FreshTestDirectory(
            "spectiary-sample-annotation-csv-generation");
    const std::filesystem::path generation_a = root / "generation-a";
    const std::filesystem::path generation_b = root / "generation-b";
    std::filesystem::create_directories(generation_a);
    std::filesystem::create_directories(generation_b);
    const std::filesystem::path path = "labels.csv";
    WriteCsv(
        generation_a / path,
        {
            {"sample", "label"},
            {"0", "A"},
            {"1", "B"},
        });
    WriteCsv(
        generation_b / path,
        {
            {"sample", "label"},
            {"0", "X"},
            {"1", "Y"},
        });
    const std::optional<std::string> parsed_generation_digest =
        spectiary::ComputeFileSha256(generation_a / path);
    Require(
        parsed_generation_digest.has_value(),
        "CSV generation fixture should hash");

    std::size_t checkpoint_calls = 0;
    std::string error;
    ScopedCurrentPath current_path(generation_a);
    const std::optional<spectiary::SampleAnnotationResult> annotation =
        LoadCsvCancelable(
            path,
            2,
            [&]() {
                ++checkpoint_calls;
                if (checkpoint_calls == 5U) {
                    std::filesystem::current_path(generation_b);
                }
            },
            &error);

    Require(
        checkpoint_calls == 5U,
        "CSV regression seam should rebind the path after parsing");
    Require(
        annotation.has_value(),
        error.empty()
            ? "CSV generation fixture should load"
            : error);
    Require(
        TextValues(*annotation) ==
            std::vector<std::string>({"A", "B"}),
        "CSV load should retain values from the parsed generation");
    Require(
        annotation->artifact_provenance.has_value() &&
            annotation->artifact_provenance->fingerprint ==
                std::optional<std::string>{
                    "sha256:" + *parsed_generation_digest},
        "CSV provenance must hash the same opened generation as the parsed values");
}

void TestNpyProvenanceUsesTheParsedFileGeneration()
{
    const std::filesystem::path root =
        FreshTestDirectory(
            "spectiary-sample-annotation-npy-generation");
    const std::filesystem::path generation_a = root / "generation-a";
    const std::filesystem::path generation_b = root / "generation-b";
    std::filesystem::create_directories(generation_a);
    std::filesystem::create_directories(generation_b);
    const std::filesystem::path path = "labels.npy";
    std::string error;
    Require(
        spectiary::ExportLabelValuesToNpy(
            generation_a / path,
            std::vector<int>{1, 2, 3},
            &error) &&
            spectiary::ExportLabelValuesToNpy(
                generation_b / path,
                std::vector<int>{9, 8, 7},
                &error),
        error.empty()
            ? "NPY generation fixtures should write"
            : error);
    const std::optional<std::string> parsed_generation_digest =
        spectiary::ComputeFileSha256(generation_a / path);
    Require(
        parsed_generation_digest.has_value(),
        "NPY generation fixture should hash");

    std::size_t checkpoint_calls = 0;
    ScopedCurrentPath current_path(generation_a);
    const std::optional<spectiary::SampleAnnotationResult> annotation =
        spectiary::test_support::LegacyFixtureIo{}.LoadCancelable(
            path,
            3,
            [&]() {
                ++checkpoint_calls;
                if (checkpoint_calls == 6U) {
                    std::filesystem::current_path(generation_b);
                }
            },
            &error);

    Require(
        checkpoint_calls == 6U,
        "NPY regression seam should rebind the path after parsing");
    Require(
        annotation.has_value(),
        error.empty()
            ? "NPY generation fixture should load"
            : error);
    Require(
        annotation->values.size() == 3U &&
            spectiary::SampleAnnotationValueAsInt(
                annotation->values[0]) == 1 &&
            spectiary::SampleAnnotationValueAsInt(
                annotation->values[1]) == 2 &&
            spectiary::SampleAnnotationValueAsInt(
                annotation->values[2]) == 3,
        "NPY load should retain values from the parsed generation");
    Require(
        annotation->artifact_provenance.has_value() &&
            annotation->artifact_provenance->fingerprint ==
                std::optional<std::string>{
                    "sha256:" + *parsed_generation_digest},
        "NPY provenance must hash the same opened generation as the parsed values");
}

void TestSourceIndexCsvMapsByCanonicalDecimalIdentity()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "spectiary-source-index-sample-annotation-csv");
    const std::filesystem::path path =
        directory / "labels.csv";
    WriteCsv(
        path,
        {
            {"sample", "label"},
            {"2", "C"},
            {"0", "A"},
            {"1", "B"},
        });

    std::string error;
    const std::optional<spectiary::SampleAnnotationResult>
        annotation = LoadCsv(
            path,
            "npy",
            {},
            3,
            &error);
    Require(
        annotation.has_value(),
        error.empty()
            ? "source-index CSV annotation should load"
            : error);
    Require(
        TextValues(*annotation) ==
            std::vector<std::string>({"A", "B", "C"}),
        "source-index identities should map to canonical rows independently of CSV row order");
}

void TestCsvUnlabeledSemanticsRemainDistinctAfterIngestion()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "spectiary-unlabeled-semantics-sample-annotation-csv");
    const std::filesystem::path path =
        directory / "labels.csv";
    WriteCsv(
        path,
        {
            {"sample", "label"},
            {"0", "unlabeled"},
            {"1", "\\unlabeled"},
            {"2", "\\\\unlabeled"},
        });

    std::string error;
    const std::optional<spectiary::SampleAnnotationResult>
        annotation = LoadCsv(
            path,
            "npy",
            {},
            3,
            &error);
    Require(
        annotation.has_value(),
        error.empty()
            ? "CSV unlabeled semantics fixture should load"
            : error);
    Require(
        TextValues(*annotation) ==
                std::vector<std::string>({
                    "unlabeled",
                    "unlabeled",
                    "\\unlabeled",
                }) &&
            annotation->values[0].missing &&
            !annotation->values[1].missing &&
            !annotation->values[2].missing,
        "CSV ingestion should retain missing separately from decoded label text");
    Require(
        spectiary::FormatSampleAnnotationValue(
            *annotation,
            annotation->values[0]) == "Unlabeled" &&
            spectiary::FormatSampleAnnotationValue(
                *annotation,
                annotation->values[1]) == "unlabeled" &&
            spectiary::FormatSampleAnnotationValue(
                *annotation,
                annotation->values[2]) == "\\unlabeled",
        "CSV missing display must not expose wire escaping or merge with labeled text");

    const std::string missing_key =
        spectiary::SampleAnnotationValueKey(
            annotation->values[0]);
    const std::string labeled_unlabeled_key =
        spectiary::SampleAnnotationValueKey(
            annotation->values[1]);
    const std::string leading_backslash_key =
        spectiary::SampleAnnotationValueKey(
            annotation->values[2]);
    Require(
        missing_key != labeled_unlabeled_key &&
            missing_key != leading_backslash_key &&
            labeled_unlabeled_key != leading_backslash_key,
        "CSV sentinel and escaped labels should retain distinct sample annotation keys");

    const spectiary::SampleFilterSource filter_source =
        spectiary::BuildAnnotationFilterSource(
            *annotation);
    const auto unlabeled_option = std::find_if(
        filter_source.options.begin(),
        filter_source.options.end(),
        [](const auto& option) {
            return option.represents_unlabeled_value;
        });
    const auto labeled_unlabeled_option = std::find_if(
        filter_source.options.begin(),
        filter_source.options.end(),
        [](const auto& option) {
            return !option.represents_unlabeled_value &&
                   option.display_text == "unlabeled";
        });
    Require(
        filter_source.options.size() == 3 &&
            unlabeled_option != filter_source.options.end() &&
            labeled_unlabeled_option !=
                filter_source.options.end() &&
            unlabeled_option->key !=
                labeled_unlabeled_option->key,
        "CSV missing and labeled text should produce distinct sample filter options");

    spectiary::SampleFilterController filter;
    filter.SetCondition(
        filter_source.id,
        {missing_key});
    const spectiary::SampleFilterEvaluation evaluation =
        filter.Evaluate(
            {filter_source},
            3);
    Require(
        evaluation.included_samples ==
            std::vector<bool>({true, false, false}),
        "selecting the CSV missing option should not include labeled text with the same name");
}

void RequireCsvRejected(
    const std::filesystem::path& path,
    const std::vector<spectiary::CsvRecord>& records,
    std::string_view source_kind,
    const std::vector<std::string>& sample_names,
    std::size_t sample_count,
    std::string_view expected_error)
{
    WriteCsv(path, records);
    std::string error;
    Require(
        !LoadCsv(
             path,
             source_kind,
             sample_names,
             sample_count,
             &error) &&
            error.find(expected_error) !=
                std::string::npos,
        "invalid CSV annotation should be rejected with a controlled diagnostic");
}

void TestCsvIdentityContractRejectsInvalidMappings()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "spectiary-invalid-sample-annotation-csv");
    const std::vector<std::string> names{
        "alpha",
        "beta",
    };
    RequireCsvRejected(
        directory / "header.csv",
        {
            {"filename", "label"},
            {"alpha", "A"},
            {"beta", "B"},
        },
        "npy",
        names,
        names.size(),
        "exact header sample,label");
    RequireCsvRejected(
        directory / "folder-header.csv",
        {
            {"sample", "label"},
            {"alpha", "A"},
            {"beta", "B"},
        },
        "folder",
        names,
        names.size(),
        "exact header filename,label");
    RequireCsvRejected(
        directory / "duplicate.csv",
        {
            {"sample", "label"},
            {"alpha", "A"},
            {"alpha", "B"},
            {"beta", "C"},
        },
        "npy",
        names,
        names.size(),
        "duplicate sample identity");
    RequireCsvRejected(
        directory / "unknown.csv",
        {
            {"sample", "label"},
            {"alpha", "A"},
            {"beta", "B"},
            {"omega", "C"},
        },
        "npy",
        names,
        names.size(),
        "unknown sample identity");
    RequireCsvRejected(
        directory / "missing.csv",
        {
            {"sample", "label"},
            {"alpha", "A"},
        },
        "npy",
        names,
        names.size(),
        "missing canonical sample identity");
    RequireCsvRejected(
        directory / "noncanonical-index.csv",
        {
            {"sample", "label"},
            {"0", "A"},
            {"01", "B"},
        },
        "npy",
        {},
        2,
        "unknown sample identity");
}

}  // namespace

int main()
{
    try {
        TestNamedCsvMapsRowsToCanonicalRoster();
        TestFolderExportReloadsWithoutCanonicalTaskProvenance();
        TestNpyLoadCapturesPortableArtifactProvenance();
        TestNpySidecarTaskNameCannotLeakPathsIntoArtifactProvenance();
        TestCsvProvenanceUsesTheParsedFileGeneration();
        TestNpyProvenanceUsesTheParsedFileGeneration();
        TestSourceIndexCsvMapsByCanonicalDecimalIdentity();
        TestCsvUnlabeledSemanticsRemainDistinctAfterIngestion();
        TestCsvIdentityContractRejectsInvalidMappings();
        std::cout << "sample annotation CSV tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
