#include "domain/sample_label_export.h"
#include "domain/csv_record_codec.h"
#include "domain/sample_labeling_source_compatibility.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

std::vector<specforge::CsvRecord> ReadCsvRecords(
    const std::filesystem::path& path)
{
    specforge::BoundedCsvFileReader reader(path);
    std::vector<specforge::CsvRecord> records;
    while (true) {
        specforge::CsvRecordReadResult result =
            reader.ReadRecord();
        if (result.status ==
            specforge::CsvRecordReadStatus::End) {
            return records;
        }
        if (!result.has_record()) {
            throw std::runtime_error(
                result.error.message.empty()
                    ? "could not read exported CSV"
                    : result.error.message);
        }
        records.push_back(std::move(result.record));
    }
}

std::string ReadBinaryText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void TestUnlabeledSentinelCannotCollideWithLabelName()
{
    specforge::SampleLabelSet labels;
    Require(
        specforge::UpsertSampleLabel(
            labels,
            specforge::SampleLabelDefinition{
                5,
                "unlabeled",
                'u'}),
        "the labeling domain should continue to accept the existing legal name");

    const std::string sentinel =
        specforge::SerializeSampleLabelValueForExport(
            labels,
            specforge::kUnlabeledSampleLabelCode);
    const std::string labeled =
        specforge::SerializeSampleLabelValueForExport(
            labels,
            5);
    Require(
        sentinel == "unlabeled" &&
            labeled == "\\unlabeled" &&
            sentinel != labeled,
        "unlabeled sentinel text must not collide with a legal label name");

    const specforge::DeserializedSampleLabelExportValue
        sentinel_value =
            specforge::DeserializeSampleLabelValueFromExport(
                sentinel);
    const specforge::DeserializedSampleLabelExportValue
        labeled_value =
            specforge::DeserializeSampleLabelValueFromExport(
                labeled);
    Require(
        sentinel_value.represents_unlabeled &&
            sentinel_value.label_text.empty() &&
            !labeled_value.represents_unlabeled &&
            labeled_value.label_text == "unlabeled",
        "shared ingestion decoding should preserve the sentinel/label distinction");
}

void TestLeadingEscapePrefixRoundTrips()
{
    const std::string label_name = "\\unlabeled";
    const std::string serialized =
        specforge::SerializeSampleLabelNameForExport(
            label_name);
    Require(
        serialized == "\\\\unlabeled",
        "an existing leading escape prefix should be escaped exactly once");

    const specforge::DeserializedSampleLabelExportValue
        deserialized =
            specforge::DeserializeSampleLabelValueFromExport(
                serialized);
    Require(
        !deserialized.represents_unlabeled &&
            deserialized.label_text == label_name,
        "ingestion decoding should remove exactly one escape prefix");
}

void TestOrdinaryLabelTextRemainsReadableAndRoundTrips()
{
    const std::string label_name = "星系,A\n可信";
    const std::string serialized =
        specforge::SerializeSampleLabelNameForExport(
            label_name);
    Require(
        serialized == label_name,
        "ordinary label text should not receive unnecessary encoding");

    const specforge::DeserializedSampleLabelExportValue
        deserialized =
            specforge::DeserializeSampleLabelValueFromExport(
                serialized);
    Require(
        !deserialized.represents_unlabeled &&
            deserialized.label_text == label_name,
        "ordinary Unicode label text should round-trip independently of CSV quoting");
}

void TestFolderCsvExportUsesCanonicalFilenameRosterAndStableLabels()
{
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "quality",
            "Quality",
            4);
    Require(
        specforge::UpsertSampleLabel(
            task.label_set,
            specforge::SampleLabelDefinition{
                5,
                "星系,\"A\"\n可信",
                'a'}) &&
            specforge::UpsertSampleLabel(
                task.label_set,
                specforge::SampleLabelDefinition{
                    6,
                    "unlabeled",
                    'u'}) &&
            specforge::UpsertSampleLabel(
                task.label_set,
                specforge::SampleLabelDefinition{
                    7,
                    "\\leading",
                    'l'}),
        "folder CSV fixture should define stable label text");
    Require(
        specforge::AssignSampleLabel(task, 0, 5).accepted &&
            specforge::AssignSampleLabel(task, 2, 6).accepted &&
            specforge::AssignSampleLabel(task, 3, 7).accepted,
        "folder CSV fixture should assign canonical rows");
    task.persistence.output_path = "owner.asdf";
    task.persistence.output_format =
        specforge::SampleLabelingOutputArtifactFormat::
            CanonicalAsdf;
    task.session.remembered_position = 2;
    task.persistence.metadata_save_pending = true;
    const specforge::SampleLabelingTask before = task;

    const specforge::SampleLabelingCanonicalSourceDescriptor source{
        .base_identity = "folder-source",
        .source_kind = "folder",
        .source_name = "spectra",
        .source_fingerprint = "folder-fingerprint",
        .sample_count = 4,
        .sample_names = {
            "zeta,\n\"one\".fits",
            "alpha.fits",
            "beta.fits",
            "gamma.fits"},
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
            ? "folder CSV snapshot should build"
            : error);

    const std::filesystem::path directory =
        FreshTestDirectory("specforge-folder-label-csv-export");
    const std::filesystem::path path =
        directory / "labels.csv";
    Require(
        specforge::ExportSampleLabelSnapshot(
            path,
            *snapshot,
            &error),
        error.empty()
            ? "folder CSV export should succeed"
            : error);
    const std::vector<specforge::CsvRecord> expected{
        {"filename", "label"},
        {"zeta,\n\"one\".fits", "星系,\"A\"\n可信"},
        {"alpha.fits", "unlabeled"},
        {"beta.fits", "\\unlabeled"},
        {"gamma.fits", "\\\\leading"},
    };
    Require(
        ReadCsvRecords(path) == expected,
        "folder CSV should preserve canonical filename order and stable label encoding");
    Require(
        task.task_id == before.task_id &&
            task.persistence.output_path == before.persistence.output_path &&
            task.persistence.output_format == before.persistence.output_format &&
            task.values == before.values &&
            task.session.remembered_position ==
                before.session.remembered_position &&
            task.persistence.pending_sample_indices ==
                before.persistence.pending_sample_indices &&
            task.persistence.metadata_save_pending ==
                before.persistence.metadata_save_pending &&
            task.persistence.save_state.kind == before.persistence.save_state.kind &&
            task.persistence.save_state.pending_count ==
                before.persistence.save_state.pending_count,
        "CSV export should not mutate task owner, save state, pending overlay, or remembered sample");

    std::ofstream(path, std::ios::binary | std::ios::trunc)
        << "existing CSV";
    specforge::SampleLabelExportSnapshot invalid = *snapshot;
    invalid.sample_names[1] = std::string("\xc3\x28", 2);
    error.clear();
    Require(
        !specforge::ExportSampleLabelSnapshot(
            path,
            invalid,
            &error) &&
            !error.empty() &&
            ReadBinaryText(path) == "existing CSV",
        "failed CSV encoding should preserve the previous target atomically");

    specforge::SampleLabelingCanonicalSourceDescriptor missing_roster =
        source;
    missing_roster.sample_names.clear();
    error.clear();
    Require(
        !specforge::BuildSampleLabelExportSnapshot(
            specforge::SampleLabelExportFormat::Csv,
            task,
            missing_roster,
            &error) &&
            error.find("filename roster") !=
                std::string::npos,
        "folder CSV should reject a missing canonical filename roster");
}

void TestNonFolderCsvUsesNamesOrStableSourceIndices()
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
            specforge::AssignSampleLabel(task, 0, 5).accepted,
        "non-folder CSV fixture should assign its first source row");

    specforge::SampleLabelingCanonicalSourceDescriptor source{
        .base_identity = "npy-source",
        .source_kind = "npy",
        .source_name = "spectra.npy",
        .source_fingerprint = "npy-fingerprint",
        .sample_count = 2,
        .sample_names = {"beta", "alpha"},
    };
    const std::filesystem::path directory =
        FreshTestDirectory("specforge-npy-label-csv-export");
    std::string error;
    const std::optional<specforge::SampleLabelExportSnapshot>
        named = specforge::BuildSampleLabelExportSnapshot(
            specforge::SampleLabelExportFormat::Csv,
            task,
            source,
            &error);
    Require(
        named.has_value() &&
            specforge::ExportSampleLabelSnapshot(
                directory / "named.csv",
                *named,
                &error),
        error.empty()
            ? "named NPY CSV export should succeed"
            : error);
    Require(
        ReadCsvRecords(directory / "named.csv") ==
            std::vector<specforge::CsvRecord>({
                {"sample", "label"},
                {"beta", "selected"},
                {"alpha", "unlabeled"},
            }),
        "non-folder explicit names should use the sample header in canonical order");

    source.sample_names.clear();
    error.clear();
    const std::optional<specforge::SampleLabelExportSnapshot>
        indexed = specforge::BuildSampleLabelExportSnapshot(
            specforge::SampleLabelExportFormat::Csv,
            task,
            source,
            &error);
    Require(
        indexed.has_value() && indexed->uses_source_index &&
            specforge::ExportSampleLabelSnapshot(
                directory / "indexed.csv",
                *indexed,
                &error),
        error.empty()
            ? "source-index CSV export should succeed"
            : error);
    Require(
        ReadCsvRecords(directory / "indexed.csv") ==
            std::vector<specforge::CsvRecord>({
                {"sample", "label"},
                {"0", "selected"},
                {"1", "unlabeled"},
            }),
        "unnamed non-folder rows should place zero-based canonical source indices in the sample column");
}

void TestDispatcherRejectsInvalidExplicitNameRoster()
{
    specforge::SampleLabelExportSnapshot snapshot{
        .format = specforge::SampleLabelExportFormat::Csv,
        .source_kind = "npy",
        .sample_names = {"duplicate", "duplicate"},
        .uses_source_index = false,
        .values = {
            specforge::kUnlabeledSampleLabelCode,
            specforge::kUnlabeledSampleLabelCode},
    };
    const std::filesystem::path directory =
        FreshTestDirectory(
            "specforge-invalid-label-export-roster");
    const std::filesystem::path path =
        directory / "invalid.csv";
    std::string error;
    Require(
        !specforge::ExportSampleLabelSnapshot(
            path,
            snapshot,
            &error) &&
            error.find("roster") != std::string::npos &&
            !std::filesystem::exists(path),
        "dispatcher should reject duplicate explicit sample names before writing");

    snapshot.sample_names = {"valid", ""};
    error.clear();
    Require(
        !specforge::ExportSampleLabelSnapshot(
            path,
            snapshot,
            &error) &&
            error.find("roster") != std::string::npos &&
            !std::filesystem::exists(path),
        "dispatcher should reject empty explicit sample names before writing");
}

}  // namespace

int main()
{
    try {
        TestUnlabeledSentinelCannotCollideWithLabelName();
        TestLeadingEscapePrefixRoundTrips();
        TestOrdinaryLabelTextRemainsReadableAndRoundTrips();
        TestFolderCsvExportUsesCanonicalFilenameRosterAndStableLabels();
        TestNonFolderCsvUsesNamesOrStableSourceIndices();
        TestDispatcherRejectsInvalidExplicitNameRoster();
        std::cout << "sample label export tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
