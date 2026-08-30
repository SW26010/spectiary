#include "domain/csv_record_codec.h"
#include "domain/sample_annotation_io.h"
#include "domain/sample_label_export.h"
#include "domain/sample_labeling_source_compatibility.h"

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

void WriteCsv(
    const std::filesystem::path& path,
    const std::vector<specforge::CsvRecord>& records)
{
    const specforge::CsvRecordWriteResult result =
        specforge::WriteCsvRecordsAtomically(
            path,
            records);
    Require(
        result.succeeded(),
        result.error.message.empty()
            ? "CSV fixture should write"
            : result.error.message);
}

std::vector<std::string> TextValues(
    const specforge::SampleAnnotationResult& annotation)
{
    std::vector<std::string> values;
    values.reserve(annotation.values.size());
    for (const specforge::SampleAnnotationValue& value :
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

std::optional<specforge::SampleAnnotationResult> LoadCsv(
    const std::filesystem::path& path,
    std::string_view source_kind,
    const std::vector<std::string>& sample_names,
    std::size_t sample_count,
    std::string* error)
{
    return specforge::SampleAnnotationIoAdapter{}.
        LoadForSource(
            path,
            specforge::SampleAnnotationSourceCompatibility{
                .base_identity = "csv-test-source",
                .source_kind = source_kind,
                .source_name = "source",
                .source_fingerprint = "fingerprint",
                .sample_count = sample_count,
                .sample_names = sample_names,
            },
            error);
}

void TestNamedCsvMapsRowsToCanonicalRoster()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge-named-sample-annotation-csv");
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
    const std::optional<specforge::SampleAnnotationResult>
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
                specforge::SampleAnnotationKind::Text &&
            annotation->dtype_name == "utf8" &&
            annotation->relationship ==
                specforge::
                    SampleAnnotationWorkflowRelationship::
                        PlainAnnotation &&
            !annotation->label_metadata.has_value() &&
            annotation->labeling_document == nullptr,
        "CSV ingestion should create only a plain text annotation");
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
        !specforge::SampleAnnotationIoAdapter{}.
             Load(path, sample_names.size(), &error) &&
            error.find("source collection") !=
                std::string::npos,
        "count-only loading must reject source-aware CSV annotations");
}

void TestFolderExportReloadsWithoutCanonicalTaskProvenance()
{
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "quality",
            "Quality",
            2);
    Require(
        specforge::UpsertSampleLabel(
            task.label_set,
            specforge::SampleLabelDefinition{
                5,
                "selected",
                's'}) &&
            specforge::AssignSampleLabel(
                task,
                0,
                5)
                .accepted,
        "folder CSV round-trip fixture should assign a label");
    const specforge::
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
    const std::optional<specforge::SampleLabelExportSnapshot>
        snapshot = specforge::BuildSampleLabelExportSnapshot(
            specforge::SampleLabelExportFormat::Csv,
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
            "specforge-folder-sample-annotation-csv");
    const std::filesystem::path path =
        directory / "labels.csv";
    Require(
        specforge::ExportSampleLabelSnapshot(
            path,
            *snapshot,
            &error),
        error.empty()
            ? "folder CSV export should write"
            : error);

    const std::optional<specforge::SampleAnnotationResult>
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
                specforge::
                    SampleAnnotationWorkflowRelationship::
                        PlainAnnotation &&
            !annotation->label_metadata &&
            !annotation->labeling_document,
        "CSV reload should preserve annotation values without inventing labeling task provenance");
}

void TestSourceIndexCsvMapsByCanonicalDecimalIdentity()
{
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge-source-index-sample-annotation-csv");
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
    const std::optional<specforge::SampleAnnotationResult>
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

void RequireCsvRejected(
    const std::filesystem::path& path,
    const std::vector<specforge::CsvRecord>& records,
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
            "specforge-invalid-sample-annotation-csv");
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
        TestSourceIndexCsvMapsByCanonicalDecimalIdentity();
        TestCsvIdentityContractRejectsInvalidMappings();
        std::cout << "sample annotation CSV tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
