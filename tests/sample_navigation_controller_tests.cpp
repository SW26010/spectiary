#include "domain/csv_record_codec.h"
#include "domain/sample_label_export.h"
#include "domain/spectrum_snapshot.h"
#include "domain/sample_labeling_asdf_codec.h"
#include "domain/sample_labeling_document.h"
#include "profile/navigation_latency_trace.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_navigation_state_cache_io.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/sample_workflow_coordinator.h"
#include "ui/source_collection_session.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
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

std::string ShapeText(const std::vector<std::size_t>& shape)
{
    std::ostringstream stream;
    stream << '(';
    for (std::size_t index = 0; index < shape.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << shape[index];
    }
    if (shape.size() == 1) {
        stream << ',';
    }
    stream << ')';
    return stream.str();
}

template <typename T>
std::vector<unsigned char> BytesFor(const std::vector<T>& values)
{
    std::vector<unsigned char> bytes(values.size() * sizeof(T));
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), values.data(), bytes.size());
    }
    return bytes;
}

std::vector<unsigned char> UnicodeNpyBytesFor(std::initializer_list<std::string_view> values, std::size_t code_units)
{
    std::vector<unsigned char> bytes;
    bytes.reserve(values.size() * code_units * sizeof(std::uint32_t));
    for (std::string_view value : values) {
        for (std::size_t index = 0; index < code_units; ++index) {
            const std::uint32_t code_point = index < value.size() ? static_cast<unsigned char>(value[index]) : 0U;
            bytes.push_back(static_cast<unsigned char>(code_point & 0xffU));
            bytes.push_back(static_cast<unsigned char>((code_point >> 8U) & 0xffU));
            bytes.push_back(static_cast<unsigned char>((code_point >> 16U) & 0xffU));
            bytes.push_back(static_cast<unsigned char>((code_point >> 24U) & 0xffU));
        }
    }
    return bytes;
}

void WriteNpy(
    const std::filesystem::path& path,
    std::string_view descr,
    const std::vector<std::size_t>& shape,
    const std::vector<unsigned char>& payload)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open test fixture for writing");

    std::string header = "{'descr': '";
    header += descr;
    header += "', 'fortran_order': False, 'shape': ";
    header += ShapeText(shape);
    header += ", }";

    constexpr std::size_t kPreambleSize = 10;
    const std::size_t header_with_newline = header.size() + 1;
    const std::size_t padding = (16 - ((kPreambleSize + header_with_newline) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    Require(header.size() <= std::numeric_limits<std::uint16_t>::max(), "test NPY header is too large");

    constexpr unsigned char kMagic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    stream.write(reinterpret_cast<const char*>(kMagic), static_cast<std::streamsize>(sizeof(kMagic)));
    constexpr char kVersion[] = {1, 0};
    stream.write(kVersion, static_cast<std::streamsize>(sizeof(kVersion)));

    const auto header_length = static_cast<std::uint16_t>(header.size());
    const char length_bytes[] = {
        static_cast<char>(header_length & 0xffU),
        static_cast<char>((header_length >> 8U) & 0xffU),
    };
    stream.write(length_bytes, static_cast<std::streamsize>(sizeof(length_bytes)));
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!payload.empty()) {
        stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    Require(stream.good(), "could not write test NPY fixture");
}

spectiary::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::string source_id,
    std::size_t spectrum_count,
    std::size_t current_index)
{
    auto snapshot = std::make_shared<spectiary::SpectrumSnapshot>();
    snapshot->source.id = std::move(source_id);
    snapshot->source.path = path;
    snapshot->collection.spectrum_count = spectrum_count;
    snapshot->collection.current_index = current_index;
    snapshot->collection.can_move_previous = current_index > 0;
    snapshot->collection.can_move_next = current_index + 1 < spectrum_count;
    snapshot->capabilities.can_switch_spectrum = spectrum_count > 1;
    return snapshot;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    Require(stream.good(), "could not open text file");
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void WriteTextFile(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream stream(path, std::ios::trunc);
    Require(stream.good(), "could not open text file for writing");
    stream << contents;
    Require(stream.good(), "could not write text file");
}

void TestNavigationStateCacheRoundTrip()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_adapter_roundtrip.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    spectiary::SampleNavigationStateCache cache;
    cache.last_indices_by_source_identity.emplace("source-a", 2);
    cache.last_indices_by_source_identity.emplace("source-b", 0);
    Require(spectiary::SaveSampleNavigationStateCache(cache_path, cache), "navigation cache should save");

    const spectiary::SampleNavigationStateCacheLoadResult loaded =
        spectiary::LoadSampleNavigationStateCache(cache_path);
    Require(
        loaded.cache.last_indices_by_source_identity.size() == 2,
        "navigation cache should restore all source indices");
    Require(
        loaded.cache.last_indices_by_source_identity.at("source-a") == 2,
        "navigation cache should restore source-a index");
    Require(
        loaded.cache.last_indices_by_source_identity.at("source-b") == 0,
        "navigation cache should restore source-b index");
    Require(loaded.warning.empty(), "valid navigation cache should load without warning");
}

void TestNavigationStateCacheIgnoresCorruptJson()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_adapter_corrupt.json";
    WriteTextFile(cache_path, "{ invalid json");

    const spectiary::SampleNavigationStateCacheLoadResult loaded =
        spectiary::LoadSampleNavigationStateCache(cache_path);
    Require(
        loaded.cache.last_indices_by_source_identity.empty(),
        "corrupt navigation cache should be ignored");
    Require(!loaded.warning.empty(), "corrupt navigation cache should report a warning");
}

void TestNavigationStateCacheIgnoresUnsupportedSchema()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_adapter_schema.json";
    WriteTextFile(
        cache_path,
        "{\n"
        "  \"format_kind\": \"spectiary.sample_navigation_state.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"sources\": [\n"
        "    { \"identity\": \"source-a\", \"last_index\": 2 }\n"
        "  ]\n"
        "}\n");

    const spectiary::SampleNavigationStateCacheLoadResult loaded =
        spectiary::LoadSampleNavigationStateCache(cache_path);
    Require(
        loaded.cache.last_indices_by_source_identity.empty(),
        "unsupported navigation cache schema should be ignored");
    Require(!loaded.warning.empty(), "unsupported navigation cache should report a warning");
}

void TestControllerOwnsNavigationState()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "spectiary_nav_controller.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "spectiary_nav_controller_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));
    WriteNpy(path.parent_path() / "spectiary_nav_controller_name.npy", "<U5", {3}, UnicodeNpyBytesFor({"alpha", "beta", "gamma"}, 5));

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 3, 1));
    Require(controller.current_index() && *controller.current_index() == 1, "controller should own initial current index");
    Require(controller.can_move_previous(), "index 1 should move previous");
    Require(controller.can_move_next(), "index 1 should move next");

    spectiary::SampleNavigationResult result =
        controller.Navigate(spectiary::SampleNavigationRequest::Previous());
    Require(result.has_active_source, "previous request should resolve against active source");
    Require(result.target_found, "previous target should be found");
    Require(result.moved, "previous request should move from index 1");
    Require(result.previous_index == 1, "previous result should report old index");
    Require(result.current_index == 0, "previous result should report actual index");
    Require(controller.current_index() && *controller.current_index() == 0, "controller should store previous result index");
    Require(!controller.can_move_previous(), "index 0 should not move previous");

    result = controller.Navigate(spectiary::SampleNavigationRequest::Previous());
    Require(result.target_found, "boundary previous request should still resolve");
    Require(!result.moved, "boundary previous request should not move");
    Require(result.current_index == 0, "boundary previous result should keep actual index");

    result = controller.Navigate(spectiary::SampleNavigationRequest::Next());
    Require(result.moved, "next request should move");
    Require(result.current_index == 1, "next request should return index 1");

    result = controller.Navigate(spectiary::SampleNavigationRequest::LocateRow(2));
    Require(result.target_found, "valid row locate should resolve");
    Require(result.moved, "row locate should move");
    Require(result.current_index == 2, "row locate should return requested row");

    result = controller.Navigate(spectiary::SampleNavigationRequest::LocateRow(42));
    Require(!result.target_found, "invalid row locate should not resolve");
    Require(!result.moved, "invalid row locate should not move");
    Require(result.current_index == 2, "invalid row locate should report unchanged actual index");

    result = controller.Navigate(spectiary::SampleNavigationRequest::LocateSampleName("beta"));
    Require(result.target_found, "valid sample-name locate should resolve");
    Require(result.moved, "sample-name locate should move");
    Require(result.current_index == 1, "sample-name locate should return matched index");

    result = controller.Navigate(spectiary::SampleNavigationRequest::LocateSampleName("missing"));
    Require(!result.target_found, "missing sample-name locate should not resolve");
    Require(result.current_index == 1, "missing sample-name locate should report unchanged actual index");
}

void TestControllerReloadsCompanionContextOnReactivate()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "spectiary_nav_context.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "spectiary_nav_context_state.json";
    const std::filesystem::path name_path = path.parent_path() / "spectiary_nav_context_name.npy";
    const std::filesystem::path annotation_path = path.parent_path() / "spectiary_nav_context_y.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {2, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0}));
    WriteNpy(name_path, "<U5", {2}, UnicodeNpyBytesFor({"alpha", "beta"}, 5));
    WriteNpy(annotation_path, "<i4", {2}, BytesFor<std::int32_t>({1, 2}));

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 2, 0));
    const spectiary::SourceCollectionManifest* context = controller.active_context();
    Require(context != nullptr, "active context should exist");
    Require(context->sample_names[0] == "alpha", "initial name should load");
    Require(
        spectiary::FormatSampleAnnotationValue(
            context->annotations[0],
            context->annotations[0].values[0]) == "1",
        "initial annotation should load");

    spectiary::SampleNavigationResult result = controller.Navigate(spectiary::SampleNavigationRequest::Next());
    Require(result.current_index == 1, "test should move before reactivation");
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 2, 0));
    Require(controller.current_index() && *controller.current_index() == 1, "unchanged reactivation should preserve session index");

    WriteNpy(name_path, "<U5", {2}, UnicodeNpyBytesFor({"delta", "omega"}, 5));
    WriteNpy(annotation_path, "<i4", {2}, BytesFor<std::int32_t>({42, 99}));
    const std::filesystem::file_time_type next_time = std::filesystem::last_write_time(annotation_path) + std::chrono::seconds(2);
    std::filesystem::last_write_time(name_path, next_time);
    std::filesystem::last_write_time(annotation_path, next_time);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 2, 0));

    context = controller.active_context();
    Require(context != nullptr, "reactivated context should exist");
    Require(context->sample_names[0] == "delta", "reactivate should reload changed sample names");
    Require(
        spectiary::FormatSampleAnnotationValue(
            context->annotations[0],
            context->annotations[0].values[0]) == "42",
        "reactivate should reload changed annotations");
    Require(controller.current_index() && *controller.current_index() == 1, "context reload should preserve current index");
}

void TestControllerAddsManualAnnotationToActiveContext()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "spectiary_nav_manual_annotation.npy";
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_manual_annotation_state.json";
    const std::filesystem::path annotation_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_manual_annotation_score.npy";
    const std::filesystem::path mismatched_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_manual_annotation_mismatch.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {2, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0}));
    WriteNpy(annotation_path, "<i4", {2}, BytesFor<std::int32_t>({7, -1}));
    WriteNpy(mismatched_path, "<i4", {1}, BytesFor<std::int32_t>({42}));

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 2, 0));
    const std::uint64_t initial_generation =
        controller.active_context_generation();
    (void)controller.Navigate(
        spectiary::SampleNavigationRequest::Next());
    Require(
        controller.active_context_generation() ==
            initial_generation,
        "ordinary navigation should preserve the active context generation");

    std::string message;
    Require(
        controller.AddReadOnlyAnnotationToActiveSource(annotation_path, &message),
        "manual annotation should attach to active source");
    const std::uint64_t added_generation =
        controller.active_context_generation();
    Require(
        added_generation > initial_generation,
        "loading an annotation should advance the active context generation");
    const spectiary::SourceCollectionManifest* context = controller.active_context();
    Require(context != nullptr, "active context should exist after manual annotation");
    Require(context->annotations.size() == 1, "manual annotation should be appended");
    Require(context->annotations[0].name == "spectiary_nav_manual_annotation_score.npy", "annotation name should be file name");
    Require(
        spectiary::FormatSampleAnnotationValue(
            context->annotations[0],
            context->annotations[0].values[0]) == "7",
        "manual annotation should load first value");

    WriteNpy(annotation_path, "<i4", {2}, BytesFor<std::int32_t>({99, 100}));
    Require(
        controller.AddReadOnlyAnnotationToActiveSource(annotation_path, &message),
        "reopened manual annotation should replace same path");
    const std::uint64_t replaced_generation =
        controller.active_context_generation();
    Require(
        replaced_generation > added_generation,
        "reloading an annotation in place should advance the active context generation");
    context = controller.active_context();
    Require(context != nullptr, "active context should still exist after replacement");
    Require(context->annotations.size() == 1, "same annotation path should replace instead of duplicating");
    Require(
        spectiary::FormatSampleAnnotationValue(
            context->annotations[0],
            context->annotations[0].values[0]) == "99",
        "replacement should refresh annotation values");

    Require(
        !controller.AddReadOnlyAnnotationToActiveSource(mismatched_path, &message),
        "mismatched manual annotation should be rejected");
    Require(
        controller.active_context_generation() ==
            replaced_generation,
        "a rejected annotation should not advance the source projection generation");
    context = controller.active_context();
    Require(context != nullptr, "active context should still exist after rejected annotation");
    Require(context->annotations.size() == 1, "rejected annotation should not be appended");
    Require(
        !context->diagnostics.empty() &&
            context->diagnostics.back().kind ==
                spectiary::
                    SourceCollectionManifestDiagnosticKind::
                        AnnotationIgnored &&
            context->diagnostics.back().path ==
                mismatched_path &&
            context->diagnostics.back().detail.find(
                "NPY array length does not match") !=
                std::string::npos,
        "rejected annotation should add a structured context diagnostic");
}

void TestControllerRestoresAndRemovesProvidedAnnotations()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "spectiary_nav_provided_annotation_source.npy";
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_provided_annotation_state.json";
    const std::filesystem::path annotation_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_provided_annotation_result.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));
    WriteNpy(annotation_path, "<i4", {3}, BytesFor<std::int32_t>({5, -1, 7}));

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 3, 0));
    const std::uint64_t initial_generation =
        controller.active_context_generation();
    Require(
        controller.RestoreReadOnlyAnnotationsForActiveSource({annotation_path}),
        "provided annotation should restore into active navigation context");
    const std::uint64_t restored_generation =
        controller.active_context_generation();
    Require(
        restored_generation > initial_generation,
        "restoring annotations should advance the active context generation");
    const spectiary::SourceCollectionManifest* context = controller.active_context();
    Require(context != nullptr && context->annotations.size() == 1, "provided annotation should be visible");
    Require(context->annotations[0].path == annotation_path, "restored annotation should keep its path");
    Require(
        spectiary::FormatSampleAnnotationValue(
            context->annotations[0],
            context->annotations[0].values[0]) == "5",
        "restored annotation should reload values");

    Require(
        controller.RemoveReadOnlyAnnotationFromActiveSource(annotation_path),
        "provided annotation should be removable");
    Require(
        controller.active_context_generation() >
            restored_generation,
        "removing an annotation should advance the active context generation");
    context = controller.active_context();
    Require(context != nullptr && context->annotations.empty(), "removed annotation should leave active context");
}

spectiary::SampleLabelingTaskCanonicalMetadata TestCanonicalMetadata()
{
    const auto timestamp =
        spectiary::ParseCanonicalTimestamp("2026-01-02T03:04:05.006Z");
    Require(timestamp.has_value(), "test canonical timestamp should parse");
    spectiary::SampleLabelingTaskCanonicalMetadata metadata;
    metadata.created_at = *timestamp;
    metadata.modified_at = *timestamp;
    metadata.origin.kind = "manual";
    return metadata;
}

void TestControllerAttachesAsdfLabelingDocumentForActiveSource()
{
    const std::filesystem::path source_path =
        std::filesystem::temp_directory_path() /
        "spectiary_nav_asdf_annotation_source.npy";
    const std::filesystem::path annotation_path =
        std::filesystem::temp_directory_path() /
        "spectiary_nav_asdf_annotation.asdf";
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() /
        "spectiary_nav_asdf_annotation_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(
        source_path,
        "<f8",
        {3, 2},
        BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource(
        "source",
        MakeSnapshot(source_path, "file:source", 3, 0));
    const std::optional<spectiary::SourceCollectionIdentity> identity =
        controller.active_source_identity();
    Require(identity.has_value(), "active source identity should exist");

    spectiary::SampleLabelingDocument document;
    document.source.base_identity = identity->id;
    document.source.kind = "npy";
    document.source.name = identity->source_name;
    document.source.fingerprint = identity->source_fingerprint;
    document.source.sample_count = identity->spectrum_count;
    document.source.roster.identity_kind =
        std::string{spectiary::kSampleLabelingDocumentSourceIndexRoster};
    document.annotation.values = {-1, 4, 4};
    document.labeling.id = "22222222-2222-4222-8222-222222222222";
    document.labeling.name = "Review task";
    document.labeling.canonical_metadata = TestCanonicalMetadata();
    document.labeling.labels = {{4, "reviewed", "r"}};
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        Require(stream.good(), "navigation ASDF fixture should open");
        const spectiary::SampleLabelingAsdfWriteResult write =
            spectiary::WriteSampleLabelingAsdfDocument(stream, document);
        Require(
            write.succeeded(),
            write.error.message.empty()
                ? "navigation ASDF fixture should write"
                : write.error.message);
    }

    std::string message;
    Require(
        controller.AddReadOnlyAnnotationToActiveSource(
            annotation_path,
            &message),
        message.empty()
            ? "ASDF annotation should attach to active navigation source"
            : message);
    const spectiary::SourceCollectionManifest* context =
        controller.active_context();
    Require(
        context != nullptr && context->annotations.size() == 1,
        "attached ASDF annotation should enter the active manifest");
    const spectiary::SampleAnnotationResult& annotation =
        context->annotations.front();
    Require(
        annotation.labeling_document != nullptr &&
            annotation.labeling_document->labeling.id ==
                "22222222-2222-4222-8222-222222222222" &&
            annotation.name == "Review task" &&
            annotation.labeling_document->labeling.labels.front().name ==
                "reviewed" &&
            !annotation.label_metadata.has_value(),
        "navigation attach should preserve canonical ASDF task and label semantics");

    document.source.base_identity = "wrong-source";
    {
        std::ofstream stream(
            annotation_path,
            std::ios::binary | std::ios::trunc);
        const spectiary::SampleLabelingAsdfWriteResult write =
            spectiary::WriteSampleLabelingAsdfDocument(stream, document);
        Require(write.succeeded(), "mismatched ASDF fixture should write");
    }
    Require(
        !controller.AddReadOnlyAnnotationToActiveSource(
            annotation_path,
            &message),
        "ASDF annotation from another source must not replace the attachment");
    context = controller.active_context();
    Require(
        context != nullptr && context->annotations.size() == 1 &&
            context->annotations.front().labeling_document->source
                    .base_identity == identity->id,
        "rejected ASDF replacement should leave the compatible snapshot attached");

    std::filesystem::remove(source_path, cleanup_error);
    std::filesystem::remove(annotation_path, cleanup_error);
    std::filesystem::remove(cache_path, cleanup_error);
}

void TestControllerAttachesFolderCsvByFilenameIdentity()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        "spectiary_nav_folder_csv_annotation";
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    std::filesystem::create_directories(directory);
    const std::filesystem::path annotation_path =
        directory / "labels.csv";
    const std::filesystem::path cache_path =
        directory / "navigation.json";
    Require(
        spectiary::WriteCsvRecordsAtomically(
            annotation_path,
            std::vector<spectiary::CsvRecord>{
                {"filename", "label"},
                {"alpha.fits", "A"},
                {"zeta.fits", "Z"},
            })
            .succeeded(),
        "folder CSV annotation fixture should write");

    spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(
            directory,
            "folder-source",
            2,
            1);
    auto mutable_snapshot =
        std::const_pointer_cast<spectiary::SpectrumSnapshot>(
            snapshot);
    mutable_snapshot->source.metadata.push_back(
        spectiary::SpectrumMetadataEntry{
            "source_type",
            "folder_collection",
            "test",
        });
    spectiary::SourceCollectionManifest manifest;
    manifest.sample_names = {
        "zeta.fits",
        "alpha.fits",
    };

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource(
        "source",
        snapshot,
        spectiary::SourceCollectionIdentity{
            .id = "folder-source-identity",
            .source_name = "spectra",
            .source_fingerprint = "folder-fingerprint",
            .context_fingerprint = "folder-context",
            .spectrum_count = 2,
        },
        std::move(manifest));
    const std::optional<spectiary::SampleLabelingSourceCompatibility>
        source_compatibility =
            controller.active_source_compatibility();
    Require(
        source_compatibility &&
            source_compatibility->source_kind == "folder" &&
            controller.current_index() &&
            *controller.current_index() == 1,
        "navigation should retain the canonical folder kind and current sample");

    std::string message;
    Require(
        controller.AddReadOnlyAnnotationToActiveSource(
            annotation_path,
            &message),
        message.empty()
            ? "folder CSV annotation should attach"
            : message);
    const std::uint64_t attached_generation =
        controller.active_context_generation();
    const spectiary::SourceCollectionManifest* context =
        controller.active_context();
    Require(
        context != nullptr &&
            context->annotations.size() == 1 &&
            context->annotations.front().kind ==
                spectiary::SampleAnnotationKind::Text &&
            context->annotations.front().relationship ==
                spectiary::
                    SampleAnnotationWorkflowRelationship::
                        PlainAnnotation &&
            !context->annotations.front().label_metadata &&
            !context->annotations.front().labeling_document,
        "folder CSV should attach only as a plain text annotation");
    Require(
        spectiary::FormatSampleAnnotationValue(
            context->annotations.front(),
            context->annotations.front().values[0]) == "Z" &&
            spectiary::FormatSampleAnnotationValue(
                context->annotations.front(),
                context->annotations.front().values[1]) == "A" &&
            controller.current_index() &&
            *controller.current_index() == 1,
        "folder CSV attachment should reorder by filename without moving the current sample");

    Require(
        spectiary::WriteCsvRecordsAtomically(
            annotation_path,
            std::vector<spectiary::CsvRecord>{
                {"filename", "label"},
                {"zeta.fits", "changed"},
                {"zeta.fits", "duplicate"},
                {"alpha.fits", "A"},
            })
            .succeeded(),
        "duplicate folder CSV fixture should write");
    Require(
        !controller.AddReadOnlyAnnotationToActiveSource(
            annotation_path,
            &message) &&
            controller.active_context_generation() ==
                attached_generation,
        "duplicate CSV identity should not replace the attached annotation");
    context = controller.active_context();
    Require(
        context != nullptr &&
            context->annotations.size() == 1 &&
            spectiary::FormatSampleAnnotationValue(
                context->annotations.front(),
                context->annotations.front().values[0]) == "Z" &&
            !context->diagnostics.empty() &&
            context->diagnostics.back().detail.find(
                "duplicate sample identity") !=
                std::string::npos,
        "rejected duplicate CSV should preserve the previous attachment and report a diagnostic");

    std::filesystem::remove_all(directory, cleanup_error);
}

void TestPreparedSourceAttachesFolderCsvByCanonicalIdentity()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        "spectiary-prepared-folder-csv-annotation";
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    std::filesystem::create_directories(directory, cleanup_error);
    const std::filesystem::path cache_path =
        directory / "navigation-state.json";
    const std::filesystem::path annotation_path =
        directory / "labels.csv";
    Require(
        spectiary::WriteCsvRecordsAtomically(
            annotation_path,
            std::vector<spectiary::CsvRecord>{
                {"filename", "label"},
                {"alpha.fits", "A"},
                {"zeta.fits", "Z"},
            })
            .succeeded(),
        "prepared folder CSV annotation fixture should write");

    spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(
            directory,
            "prepared-folder-source",
            2,
            0);
    auto mutable_snapshot =
        std::const_pointer_cast<spectiary::SpectrumSnapshot>(
            snapshot);
    mutable_snapshot->source.metadata.push_back(
        spectiary::SpectrumMetadataEntry{
            "source_type",
            "folder_collection",
            "test",
        });
    const spectiary::SourceCollectionIdentity identity{
        .id = "prepared-folder-source-identity",
        .source_name = "spectra",
        .source_fingerprint = "prepared-folder-fingerprint",
        .context_fingerprint = "prepared-folder-context",
        .spectrum_count = 2,
    };
    spectiary::SourceCollectionManifest manifest;
    manifest.sample_names = {
        "zeta.fits",
        "alpha.fits",
    };
    spectiary::PreparedSampleWorkflowState prepared;
    prepared.current_index = 0;

    spectiary::SourceCollectionContext context;
    context.identity = identity;
    context.manifest = std::move(manifest);
    spectiary::SourceCollectionSession session(
        {}, cache_path,
        directory / "labeling-state.json",
        directory / "workflow-state.json");
    (void)session.CommitPreparedOpen({
        .path = directory, .spectrum_index = snapshot->collection.current_index, .snapshot = snapshot,
        .payload = spectiary::PreparedSourceCollectionPlan{std::move(context), std::move(prepared)},
    });
    const spectiary::SourceCollectionSessionResult outcome =
        session.Submit(spectiary::SourceCollectionSessionIntent::EditSourceCollection(
            spectiary::SourceCollectionIntent::AddReadOnlyAnnotationResult(annotation_path)));
    Require(
        outcome.loaded,
        outcome.message.empty()
            ? "prepared folder source should attach CSV"
            : outcome.message);
    const spectiary::SourceCollectionNavigationView view =
        session.View().navigation;
    const spectiary::SourceCollectionLabelingView labeling_view =
        session.View().labeling;
    Require(
        view.current_annotations.size() == 1 &&
            view.current_annotations.front().display_text == "Z" &&
            labeling_view.source_kind == "folder",
        "session prepared-source projection should expose the canonical folder kind and CSV roster");

    std::filesystem::remove_all(directory, cleanup_error);
}

void TestInvalidDisplayNamesExportAndAttachByCanonicalIndex()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        "spectiary-invalid-display-names-csv-annotation";
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    std::filesystem::create_directories(directory, cleanup_error);
    const std::filesystem::path cache_path =
        directory / "navigation-state.json";
    const std::filesystem::path annotation_path =
        directory / "labels.csv";
    spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(
            directory / "spectra.npy",
            "invalid-display-name-source",
            2,
            0);
    auto mutable_snapshot =
        std::const_pointer_cast<spectiary::SpectrumSnapshot>(
            snapshot);
    mutable_snapshot->source.metadata.push_back(
        spectiary::SpectrumMetadataEntry{
            "format",
            "npy",
            "test",
        });
    const spectiary::SourceCollectionIdentity identity{
        .id = "invalid-display-name-source-identity",
        .source_name = "spectra.npy",
        .source_fingerprint = "invalid-display-name-fingerprint",
        .context_fingerprint = "invalid-display-name-context",
        .spectrum_count = 2,
    };
    spectiary::SourceCollectionManifest manifest;
    manifest.sample_names = {
        "duplicate",
        "duplicate",
    };
    const spectiary::SampleLabelingCanonicalSourceDescriptor source =
        spectiary::BuildSampleLabelingCanonicalSourceDescriptor(
            *snapshot,
            identity,
            manifest);
    Require(
        source.source_kind == "npy" &&
            source.sample_names.empty(),
        "invalid display names should select the canonical source-index roster");

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
                1,
                5)
                .accepted,
        "source-index CSV fixture should assign a label");
    std::string message;
    const std::optional<spectiary::SampleLabelExportSnapshot>
        export_snapshot =
            spectiary::BuildSampleLabelExportSnapshot(
                spectiary::SampleLabelExportFormat::Csv,
                task,
                source,
                &message);
    Require(
        export_snapshot &&
            spectiary::ExportSampleLabelSnapshot(
                annotation_path,
                *export_snapshot,
                &message),
        message.empty()
            ? "source-index CSV export should write"
            : message);

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource(
        "source",
        snapshot,
        identity,
        std::move(manifest));
    const std::optional<spectiary::SampleLabelingSourceCompatibility>
        active_compatibility =
            controller.active_source_compatibility();
    Require(
        active_compatibility &&
            active_compatibility->source_kind == "npy" &&
            active_compatibility->sample_names.empty(),
        "live and fallback attachment should borrow the canonical source-index roster view");
    Require(
        controller.AddReadOnlyAnnotationToActiveSource(
            annotation_path,
            &message),
        message.empty()
            ? "source-index CSV export should reattach"
            : message);
    const spectiary::SourceCollectionManifest* context =
        controller.active_context();
    Require(
        context != nullptr &&
            context->annotations.size() == 1 &&
            spectiary::FormatSampleAnnotationValue(
                context->annotations.front(),
                context->annotations.front().values[0]) ==
                "Unlabeled" &&
            spectiary::FormatSampleAnnotationValue(
                context->annotations.front(),
                context->annotations.front().values[1]) ==
                "selected",
        "CSV reattach should ignore invalid display names and use canonical source indexes");

    std::filesystem::remove_all(directory, cleanup_error);
}

void TestAnnotationPathLookupUsesOnlyInMemorySourceIdentity()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_annotation_path_lookup_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    spectiary::SampleNavigationController controller(cache_path);
    const spectiary::SpectrumSnapshotHandle first_snapshot =
        MakeSnapshot("C:/synthetic/first-source.npy", "first-source", 1, 0);
    spectiary::SourceCollectionManifest first_manifest;
    first_manifest.annotations.push_back(spectiary::SampleAnnotationResult{
        .path = "C:/unavailable/../annotations/result.npy",
    });
    first_manifest.annotations.push_back(spectiary::SampleAnnotationResult{
        .path = "c:/annotations/RESULT.npy",
    });
    controller.ActivateSource(
        "first-key",
        first_snapshot,
        spectiary::SourceCollectionIdentity{
            .id = "first-identity",
            .source_name = "first",
            .source_fingerprint = "first-source-fingerprint",
            .context_fingerprint = "first-context-fingerprint",
            .spectrum_count = 1,
        },
        std::move(first_manifest));

    const spectiary::SpectrumSnapshotHandle second_snapshot =
        MakeSnapshot("C:/synthetic/second-source.npy", "second-source", 1, 0);
    spectiary::SourceCollectionManifest second_manifest;
    second_manifest.annotations.push_back(spectiary::SampleAnnotationResult{
        .path = "//offline-server/share/second-result.npy",
    });
    controller.ActivateSource(
        "second-key",
        second_snapshot,
        spectiary::SourceCollectionIdentity{
            .id = "second-identity",
            .source_name = "second",
            .source_fingerprint = "second-source-fingerprint",
            .context_fingerprint = "second-context-fingerprint",
            .spectrum_count = 1,
        },
        std::move(second_manifest));

    const std::vector<std::filesystem::path> first_paths =
        controller.AnnotationPathsForSourceKey("first-key");
    Require(first_paths.size() == 1, "lexically equivalent annotation paths should deduplicate in memory");
    Require(
        first_paths.front() == std::filesystem::path("C:/unavailable/../annotations/result.npy"),
        "annotation lookup should preserve the first stored spelling");
    Require(
        controller.AnnotationPathsForSourceKey("missing-key").empty(),
        "unknown source lookup should not fall back to scanning every source");

    const std::vector<std::filesystem::path> second_paths =
        controller.AnnotationPathsForSourceKey("second-key");
    Require(
        second_paths.size() == 1 && second_paths.front() == std::filesystem::path("//offline-server/share/second-result.npy"),
        "source-local lookup should return only the requested source annotations");
}

void TestControllerPersistsLastIndexBySourceIdentity()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "spectiary_nav_persist.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "spectiary_nav_persist_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));

    {
        spectiary::SampleNavigationController controller(cache_path);
        controller.ActivateSource("source-a", MakeSnapshot(path, "file:any-path-a", 3, 0));
        const spectiary::SampleNavigationResult result =
            controller.Navigate(spectiary::SampleNavigationRequest::LocateRow(2));
        Require(result.current_index == 2, "first controller should navigate to row 2");
        Require(controller.FlushStateCache(), "normal shutdown flush should persist the final row");
    }

    {
        spectiary::SampleNavigationController controller(cache_path);
        controller.ActivateSource("source-b", MakeSnapshot(path, "file:any-path-b", 3, 0));
        Require(controller.current_index() && *controller.current_index() == 2, "new controller should restore last row");
    }

    const std::string cache_text = ReadTextFile(cache_path);
    Require(cache_text.find(PathToUtf8(path.parent_path())) == std::string::npos, "cache should not key state by absolute directory path");
}

void TestControllerDebouncesNavigationStatePersistence()
{
    using namespace std::chrono_literals;

    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_debounce_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    constexpr std::string_view kIdentity = "navigation-debounce-identity";
    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource(
        "source",
        MakeSnapshot("C:/synthetic/navigation-debounce.npy", "navigation-debounce", 3, 0),
        spectiary::SourceCollectionIdentity{
            .id = std::string{kIdentity},
            .source_name = "navigation-debounce",
            .source_fingerprint = "source-v1",
            .context_fingerprint = "context-v1",
            .spectrum_count = 3,
        },
        {});

    Require(
        !std::filesystem::exists(cache_path),
        "activating a source should only dirty the navigation cache");
    const auto activation_deadline = controller.NextMaintenanceDeadline();
    Require(activation_deadline.has_value(), "dirty navigation state should expose a maintenance deadline");
    controller.RunMaintenance(*activation_deadline);
    Require(
        spectiary::LoadSampleNavigationStateCache(cache_path)
                .cache
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 0,
        "maintenance should persist the activated row");

    const spectiary::SampleNavigationResult result =
        controller.Navigate(spectiary::SampleNavigationRequest::LocateRow(1));
    Require(result.current_index == 1, "fixture should navigate to row 1");
    Require(
        spectiary::LoadSampleNavigationStateCache(cache_path)
                .cache
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 0,
        "navigation should not synchronously rewrite the cache");

    const auto navigation_deadline = controller.NextMaintenanceDeadline();
    Require(navigation_deadline.has_value(), "navigation should schedule debounced persistence");
    controller.RunMaintenance(*navigation_deadline - 1ms);
    Require(
        spectiary::LoadSampleNavigationStateCache(cache_path)
                .cache
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 0,
        "maintenance before the debounce deadline should not save");
    controller.RunMaintenance(*navigation_deadline);
    Require(
        spectiary::LoadSampleNavigationStateCache(cache_path)
                .cache
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 1,
        "maintenance at the debounce deadline should save the latest row");
}

void TestControllerCoalescesNavigationStateAndRetriesFailure()
{
    using namespace std::chrono_literals;

    const std::filesystem::path blocker =
        std::filesystem::temp_directory_path() / "spectiary_nav_retry_blocker";
    const std::filesystem::path cache_path = blocker / "navigation-state.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(blocker, cleanup_error);
    WriteTextFile(blocker, "block parent directory creation");

    constexpr std::string_view kIdentity = "navigation-retry-identity";
    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource(
        "source",
        MakeSnapshot("C:/synthetic/navigation-retry.npy", "navigation-retry", 4, 0),
        spectiary::SourceCollectionIdentity{
            .id = std::string{kIdentity},
            .source_name = "navigation-retry",
            .source_fingerprint = "source-v1",
            .context_fingerprint = "context-v1",
            .spectrum_count = 4,
        },
        {});
    (void)controller.Navigate(spectiary::SampleNavigationRequest::LocateRow(1));
    (void)controller.Navigate(spectiary::SampleNavigationRequest::LocateRow(3));

    Require(
        !std::filesystem::exists(cache_path),
        "continuous navigation should not write an intermediate row");
    const auto debounce_deadline = controller.NextMaintenanceDeadline();
    Require(debounce_deadline.has_value(), "coalesced navigation should retain one save deadline");
    controller.RunMaintenance(*debounce_deadline);

    const auto retry_deadline = controller.NextMaintenanceDeadline();
    Require(
        controller.PersistenceStatus().retrying,
        "failed navigation save should expose retrying status");
    Require(
        retry_deadline && *retry_deadline > *debounce_deadline,
        "a failed save should remain dirty and schedule a future retry deadline");

    std::filesystem::remove(blocker, cleanup_error);
    std::filesystem::create_directories(blocker);
    controller.RunMaintenance(*retry_deadline - 1ms);
    Require(
        !std::filesystem::exists(cache_path),
        "maintenance before the retry deadline should not save");
    controller.RunMaintenance(*retry_deadline);
    Require(
        controller.PersistenceStatus().recovered,
        "successful navigation retry should expose recovered status");
    Require(
        spectiary::LoadSampleNavigationStateCache(cache_path)
                .cache
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 3,
        "retry should persist only the final coalesced row");

    (void)controller.Navigate(
        spectiary::SampleNavigationRequest::LocateRow(2));
    Require(
        !controller.PersistenceStatus().recovered,
        "the next navigation mutation should clear recovered status");
}

void TestControllerClearsNavigationLoadWarningAfterFlush()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() /
        "spectiary_nav_load_warning_flush.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteTextFile(cache_path, "{ invalid json");

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource(
        "source",
        MakeSnapshot(
            "C:/synthetic/navigation-load-warning.npy",
            "navigation-load-warning",
            2,
            0),
        spectiary::SourceCollectionIdentity{
            .id = "navigation-load-warning",
            .source_name = "navigation-load-warning",
            .source_fingerprint = "source-v1",
            .context_fingerprint = "context-v1",
            .spectrum_count = 2,
        },
        {});
    Require(
        !controller.PersistenceStatus().load_warning.empty(),
        "a corrupt navigation cache should expose its load warning");
    Require(
        controller.FlushStateCache(),
        "navigation shutdown flush should repair a corrupt cache");
    Require(
        controller.PersistenceStatus().load_warning.empty(),
        "a successful navigation flush should clear its load warning");

    std::filesystem::remove(cache_path, cleanup_error);
}

void TestCoordinatorFlushesWorkflowIndependentlyAndRecovers()
{
    using namespace std::chrono_literals;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "spectiary_workflow_persistence_lifecycle";
    const std::filesystem::path navigation_cache = root / "navigation.json";
    const std::filesystem::path labeling_cache = root / "labeling.json";
    const std::filesystem::path workflow_parent = root / "workflow-parent";
    const std::filesystem::path workflow_cache = workflow_parent / "workflow.json";
    const std::filesystem::path source_path = root / "source.npy";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(workflow_parent);
    WriteNpy(
        source_path,
        "<f8",
        {2, 2},
        BytesFor<double>({1.0, 2.0, 3.0, 4.0}));
    WriteTextFile(workflow_cache, "{ invalid json");

    spectiary::SampleWorkflowCoordinator coordinator(
        navigation_cache,
        labeling_cache,
        workflow_cache);
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, "workflow-persistence", 2, 0);
    (void)coordinator.SyncActiveSource("source", snapshot);
    const std::optional<spectiary::SourceCollectionIdentity> active_identity =
        coordinator.ActiveSourceIdentity();
    Require(
        active_identity.has_value(),
        "workflow persistence fixture should activate a source identity");
    const std::string workflow_identity = active_identity->id;
    (void)coordinator.Apply(
        spectiary::SampleSortingIntent::SetSortDirection(
            spectiary::SampleNavigationSortDirection::Descending),
        snapshot);
    Require(
        !coordinator.PersistenceStatus().workflow.load_warning.empty(),
        "a corrupt workflow cache should expose its load warning");

    std::filesystem::remove(workflow_cache, cleanup_error);
    std::filesystem::remove_all(workflow_parent, cleanup_error);
    WriteTextFile(workflow_parent, "block workflow cache parent");

    const spectiary::SampleWorkflowStateFlushResult failed_flush =
        coordinator.FlushStateCachesWithStatus();
    Require(
        failed_flush.navigation_saved &&
            failed_flush.labeling_saved &&
            !failed_flush.workflow_saved,
        "a failed workflow owner must not suppress independent navigation and labeling flushes");
    const spectiary::SampleWorkflowPersistenceStatus failed_status =
        coordinator.PersistenceStatus();
    Require(
        failed_status.workflow.retrying &&
            !failed_status.workflow.save_message.empty() &&
            !failed_status.workflow.load_warning.empty(),
        "a failed workflow flush should retain retry and load-warning status");

    const auto retry_deadline = coordinator.NextMaintenanceDeadline();
    Require(
        retry_deadline.has_value(),
        "a failed workflow flush should expose its retry deadline");
    (void)coordinator.RunMaintenance(*retry_deadline - 1ms, snapshot);
    Require(
        coordinator.PersistenceStatus().workflow.retrying,
        "workflow maintenance before the retry deadline should remain retrying");

    std::filesystem::remove(workflow_parent, cleanup_error);
    std::filesystem::create_directories(workflow_parent);
    (void)coordinator.RunMaintenance(*retry_deadline, snapshot);
    const spectiary::SampleWorkflowPersistenceStatus recovered_status =
        coordinator.PersistenceStatus();
    Require(
        recovered_status.workflow.recovered &&
            recovered_status.workflow.load_warning.empty() &&
            !recovered_status.workflow.save_message.empty(),
        "a successful workflow retry should clear the load warning and expose recovery");
    const spectiary::SampleWorkflowStateCacheLoadResult restored =
        spectiary::LoadSampleWorkflowStateCache(spectiary::RuntimePaths{}, workflow_cache);
    Require(
        restored.cache.sources_by_identity.contains(workflow_identity) &&
            restored.cache.sources_by_identity.at(workflow_identity)
                    .selected_sample_sort_direction ==
                spectiary::SampleNavigationSortDirection::Descending,
        "workflow retry should persist the owner state");

    (void)coordinator.Apply(
        spectiary::SampleSortingIntent::SetSortDirection(
            spectiary::SampleNavigationSortDirection::Ascending),
        snapshot);
    Require(
        !coordinator.PersistenceStatus().workflow.recovered &&
            coordinator.PersistenceStatus().workflow.save_message.empty() &&
            coordinator.NextMaintenanceDeadline().has_value(),
        "a later workflow mutation should clear recovery and schedule debounced saving");
    Require(
        coordinator.FlushStateCaches(),
        "workflow shutdown flush should save the later mutation");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestControllerAdoptsAndMergesPreparedNavigationCache()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() /
        "spectiary_nav_prepared_cache_merge.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    auto prepared =
        std::make_shared<
            spectiary::SampleNavigationStateCacheLoadResult>();
    prepared->cache.last_indices_by_source_identity.emplace(
        "prepared-source",
        2);
    prepared->cache.last_indices_by_source_identity.emplace(
        "unrelated-source",
        7);

    spectiary::SampleNavigationController controller(
        cache_path);
    (void)controller.AdoptPreparedStateCache(prepared);
    controller.ActivateSource(
        "source",
        MakeSnapshot(
            "C:/synthetic/prepared-navigation.npy",
            "prepared-navigation",
            4,
            0),
        spectiary::SourceCollectionIdentity{
            .id = "prepared-source",
            .source_name = "prepared-navigation",
            .source_fingerprint = "source-v1",
            .context_fingerprint = "context-v1",
            .spectrum_count = 4,
        },
        {});
    Require(
        controller.current_index() &&
            *controller.current_index() == 2,
        "prepared navigation cache should restore the active source index");

    (void)controller.Navigate(
        spectiary::SampleNavigationRequest::LocateRow(3));
    Require(
        controller.FlushStateCache(),
        "prepared navigation cache should flush after a live update");
    const spectiary::SampleNavigationStateCacheLoadResult loaded =
        spectiary::LoadSampleNavigationStateCache(cache_path);
    Require(
        loaded.cache.last_indices_by_source_identity.at(
            "prepared-source") == 3,
        "live navigation should override its prepared snapshot entry");
    Require(
        loaded.cache.last_indices_by_source_identity.at(
            "unrelated-source") == 7,
        "saving a prepared navigation snapshot must preserve unrelated entries");
}

void TestCoordinatorMaintainsFlushesAndRestoresNavigationState()
{
    const std::filesystem::path source_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_coordinator.npy";
    const std::filesystem::path navigation_cache =
        std::filesystem::temp_directory_path() / "spectiary_nav_coordinator_state.json";
    const std::filesystem::path labeling_cache =
        std::filesystem::temp_directory_path() / "spectiary_nav_coordinator_labeling.json";
    const std::filesystem::path workflow_cache =
        std::filesystem::temp_directory_path() / "spectiary_nav_coordinator_workflow.json";
    std::error_code cleanup_error;
    std::filesystem::remove(navigation_cache, cleanup_error);
    std::filesystem::remove(labeling_cache, cleanup_error);
    std::filesystem::remove(workflow_cache, cleanup_error);
    WriteNpy(
        source_path,
        "<f8",
        {3, 2},
        BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));

    std::string source_identity;
    {
        spectiary::SampleWorkflowCoordinator coordinator(
            navigation_cache,
            labeling_cache,
            workflow_cache);
        const spectiary::SpectrumSnapshotHandle snapshot =
            MakeSnapshot(source_path, "navigation-coordinator", 3, 0);
        (void)coordinator.SyncActiveSource("source", snapshot);
        source_identity = coordinator.ActiveSourceIdentity()->id;
        coordinator.SetDeferredSampleNavigation(true);

        spectiary::SampleWorkflowTransitionOutcome navigation =
            coordinator.Apply(
                spectiary::SampleNavigationIntent::Move(
                    spectiary::SampleNavigationRequest::LocateRow(1)),
                snapshot);
        Require(
            navigation.snapshot_index_to_load == 1 &&
                coordinator.CommitPreparedSource("source",
                    MakeSnapshot(source_path, "navigation-coordinator", 3, 1), 1, 0,
                    spectiary::PreparedSourceCollectionReuse{*coordinator.KnownSourceIdentity("source")})
                    .disposition == spectiary::PreparedSourceDisposition::Adopt,
            "coordinator should commit the first deferred row");
        Require(
            !std::filesystem::exists(navigation_cache),
            "deferred activation should not synchronously persist navigation state");

        for (int attempt = 0; attempt < 4 && !std::filesystem::exists(navigation_cache); ++attempt) {
            const auto deadline = coordinator.NextMaintenanceDeadline();
            Require(deadline.has_value(), "coordinator should expose navigation maintenance");
            (void)coordinator.RunMaintenance(
                *deadline,
                snapshot);
        }
        Require(
            std::filesystem::exists(navigation_cache),
            "coordinator maintenance should reach the navigation deadline");
        Require(
            spectiary::LoadSampleNavigationStateCache(navigation_cache)
                    .cache
                    .last_indices_by_source_identity.at(source_identity) == 1,
            "coordinator maintenance should persist the committed row");

        navigation = coordinator.Apply(
            spectiary::SampleNavigationIntent::Move(
                spectiary::SampleNavigationRequest::LocateRow(2)),
            snapshot);
        Require(
            navigation.snapshot_index_to_load == 2 &&
                coordinator.CommitPreparedSource("source",
                    MakeSnapshot(source_path, "navigation-coordinator", 3, 2), 2, 0,
                    spectiary::PreparedSourceCollectionReuse{*coordinator.KnownSourceIdentity("source")})
                    .disposition == spectiary::PreparedSourceDisposition::Adopt,
            "coordinator should commit the final deferred row");
        Require(
            spectiary::LoadSampleNavigationStateCache(navigation_cache)
                    .cache
                    .last_indices_by_source_identity.at(source_identity) == 1,
            "the final row should remain memory-only until flush");
        Require(coordinator.FlushStateCaches(), "normal shutdown flush should save navigation state");
    }

    spectiary::SampleWorkflowCoordinator restored(
        navigation_cache,
        labeling_cache,
        workflow_cache);
    (void)restored.SyncActiveSource(
        "source",
        MakeSnapshot(source_path, "navigation-coordinator", 3, 0));
    Require(
        restored.current_index() && *restored.current_index() == 2,
        "restart should restore the final flushed navigation row");
}

void TestControllerLoadsLongFolderIdentityState()
{
    const std::filesystem::path folder_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_long_folder_identity";
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_long_folder_identity_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder_path, cleanup_error);
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::create_directories(folder_path);

    constexpr std::size_t kSampleCount = 40;
    for (std::size_t index = 0; index < kSampleCount; ++index) {
        std::ostringstream name;
        name << "spectiary-long-folder-identity-sample-" << index << "-with-extra-cache-text.csv";
        std::ofstream stream(folder_path / name.str());
        Require(stream.good(), "could not write folder identity sample");
        stream << "wavelength,flux\n5000,1\n5001,2\n";
    }

    spectiary::SpectrumSnapshotHandle snapshot = MakeSnapshot(folder_path, "folder:long-identity", kSampleCount, 0);
    spectiary::SourceCollectionContext context = spectiary::LoadSourceCollectionContext(*snapshot);
    const spectiary::SourceCollectionFolderListing listing = spectiary::ScanSourceCollectionFolder(folder_path);
    std::string legacy_fingerprint = "folder";
    for (const spectiary::SourceCollectionFolderSpectrumFile& sample : listing.spectra) {
        legacy_fingerprint += ";" + PathToUtf8(sample.path.filename()) + ":" + sample.stat_fingerprint;
    }
    const std::string legacy_identity =
        "name=" + PathToUtf8(folder_path.filename()) + "|fingerprint=" + legacy_fingerprint +
        "|count=" + std::to_string(kSampleCount);
    Require(legacy_identity.size() > 1000, "legacy fixture should cover long cache migration");
    Require(context.identity.id.size() == 74, "current folder identity should remain fixed-size");

    {
        std::ofstream stream(cache_path);
        Require(stream.good(), "could not write navigation cache fixture");
        stream << "{\n";
        stream << "  \"format_kind\": \"spectiary.sample_navigation_state.cache\",\n";
        stream << "  \"schema_version\": 1,\n";
        stream << "  \"sources\": [\n";
        stream << "    { \"identity\": \"short-source\", \"last_index\": 0 },\n";
        stream << "    { \"identity\": \"" << legacy_identity << "\", \"last_index\": 7 }\n";
        stream << "  ]\n";
        stream << "}\n";
    }

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource(
        "folder-source",
        snapshot,
        context.identity,
        std::move(context.manifest));
    Require(
        controller.current_index() && *controller.current_index() == 7,
        "legacy long cached identity should migrate and restore its index");
}

void TestRemoveSourceUsesExternalSourceKey()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "spectiary_nav_remove.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "spectiary_nav_remove_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {2, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0}));

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source-list-key", MakeSnapshot(path, "file:any-path", 2, 1));
    Require(controller.current_index() && *controller.current_index() == 1, "test source should activate");

    (void)controller.RemoveSource("source-list-key");
    Require(!controller.current_index(), "removed source should clear the active session");
    const spectiary::SampleNavigationResult result =
        controller.Navigate(spectiary::SampleNavigationRequest::Previous());
    Require(!result.has_active_source, "removed source should not handle navigation requests");
}

void TestExplicitSampleOutsideNavigationSequence()
{
    using namespace spectiary;
    SampleNavigationController controller(std::filesystem::path{});
    SourceCollectionManifest manifest;
    manifest.sample_names = {"alpha", "beta", "gamma", "delta", "omega"};
    controller.ActivateSource("explicit", MakeSnapshot("C:/synthetic/explicit.npy", "explicit", 5, 0),
        SourceCollectionIdentity{.id = "explicit", .source_fingerprint = "v1",
            .context_fingerprint = "v1", .spectrum_count = 5}, std::move(manifest));
    (void)controller.SetSampleFilter({false, true, false, true, false});
    SampleNavigationSortChoice sort;
    sort.active = true;
    sort.direction = SampleNavigationSortDirection::Descending;
    for (int i = 0; i < 5; ++i) sort.values.push_back(MakeSampleNavigationSortValue(double(i)));
    (void)controller.SetSampleSorting(std::move(sort));
    Require(controller.PresentExplicitSample(2), "explicit source row should be accepted");
    Require(controller.current_index() == 2 && !controller.current_sample_in_filter() &&
        !controller.current_sequence().current_sequence_position &&
        !controller.can_move_previous() && !controller.can_move_next(),
        "out-of-sequence display must have no navigation cursor");
    auto next = controller.Navigate(SampleNavigationRequest::Next());
    Require(!next.target_found && next.has_current_sample && next.current_source_row == 2 &&
        !next.current_sequence_position && !next.current_sample_in_filter,
        "unavailable next must retain the displayed source row");
    Require(!controller.Navigate(SampleNavigationRequest::LocateSourceRowInSequence(0)).target_found &&
        !controller.Navigate(SampleNavigationRequest::LocateSampleName("alpha")).target_found,
        "ordinary location must still reject excluded samples");
    Require(controller.Navigate(SampleNavigationRequest::LocateSampleName("delta")).target_found &&
        controller.current_sequence().current_sequence_position == 0 && controller.can_move_next(),
        "valid name location must restore the sorted sample-filter cursor");
    Require(controller.PresentExplicitSample(1) &&
        controller.current_sequence().current_sequence_position == 1,
        "included explicit sample must retain its sorted position");
    Require(controller.PresentExplicitSample(2), "explicit sample should reopen");
    (void)controller.ClearSampleFilter(true);
    Require(controller.current_index() == 2 && !controller.pending_index() &&
        controller.current_sequence().current_sequence_position == 2 &&
        controller.can_move_previous() && controller.can_move_next(),
        "disabling sample filtering must retain explicit sample in sorted full sequence");
    (void)controller.SetSampleFilter({false, false, false, false, false});
    Require(controller.PresentExplicitSample(4) && controller.current_index() == 4 &&
        controller.current_sequence().empty && !controller.current_sequence().current_sequence_position,
        "empty sample-filter results must still allow explicit display");
    Require(!controller.PresentExplicitSample(5) && controller.current_index() == 4,
        "invalid explicit row must not mutate current sample");
}

void TestFilterConstrainsSequentialNavigation()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "spectiary_nav_filter.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "spectiary_nav_filter_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(
        path,
        "<f8",
        {5, 2},
        BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0}));
    WriteNpy(
        path.parent_path() / "spectiary_nav_filter_name.npy",
        "<U7",
        {5},
        UnicodeNpyBytesFor({"alpha", "beta", "gamma", "delta", "omega"}, 7));

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 5, 0));
    controller.SetSampleFilter({false, true, false, true, false});
    Require(controller.filter_active(), "filter should be active");
    Require(controller.filtered_sample_count() == 2, "filter should count included samples");
    Require(controller.current_index() && *controller.current_index() == 1, "filter should move to the first included row");
    Require(controller.current_sample_in_filter(), "current row should be inside the sequence after filter reconciliation");
    Require(controller.can_move_next(), "first filtered row should move next");
    Require(!controller.can_move_previous(), "first filtered row should not move previous inside the filter");

    spectiary::SampleNavigationResult result =
        controller.Navigate(spectiary::SampleNavigationRequest::Next());
    Require(result.target_found && result.moved, "filtered next should move to the next included row");
    Require(result.current_index == 3, "filtered next should skip excluded rows");
    Require(!controller.can_move_next(), "last filtered row should not move next");

    result = controller.Navigate(spectiary::SampleNavigationRequest::Previous());
    Require(result.target_found && result.moved, "filtered previous should move to prior included row");
    Require(result.current_index == 1, "filtered previous should skip excluded rows");

    result = controller.Navigate(spectiary::SampleNavigationRequest::LabelAdvance());
    Require(result.target_found && result.moved, "label advance should move within the filtered source order");
    Require(result.current_index == 3, "label advance should let navigation choose the filtered target");

    result = controller.Navigate(spectiary::SampleNavigationRequest::Previous());
    Require(result.target_found && result.moved, "test should return to the first filtered row");
    Require(result.current_index == 1, "test should return to row 1");

    result = controller.Navigate(
        spectiary::SampleNavigationRequest::LabelAdvanceToEligible({false, false, false, true, false}));
    Require(result.target_found && result.moved, "eligible label advance should move to the next eligible filtered row");
    Require(result.current_index == 3, "eligible label advance should use navigation-owned filter state");

    result = controller.Navigate(spectiary::SampleNavigationRequest::Previous());
    Require(result.target_found && result.moved, "test should return to the first filtered row again");
    result = controller.Navigate(
        spectiary::SampleNavigationRequest::LabelAdvanceToEligible({false, false, true, false, false}));
    Require(result.target_found && !result.moved, "eligible label advance should ignore excluded rows");
    Require(result.current_index == 1, "label advance with no eligible filtered target should keep the current row");

    (void)controller.SetSampleNameQuery("a");
    const std::vector<std::size_t>& matches = controller.sample_name_matches();
    Require(matches.size() == 2, "sample-name matches should be filtered to included rows");
    Require(matches[0] == 1 && matches[1] == 3, "filtered sample-name matches should preserve source order");

    result = controller.Navigate(spectiary::SampleNavigationRequest::LocateRow(4));
    Require(result.blocked_by_filter, "row locate should be blocked while filtering changes the sequence");
    Require(!result.target_found, "blocked row locate should not produce a target");
    Require(result.current_index == 1, "blocked row locate should keep the current sequence row");

    result = controller.Navigate(spectiary::SampleNavigationRequest::LocateSampleName("omega"));
    Require(!result.target_found, "sample-name locate should not jump to an excluded exact match");
    Require(result.current_index == 1, "excluded sample-name locate should keep the current row");

    result = controller.Navigate(spectiary::SampleNavigationRequest::LocateSampleName("beta"));
    Require(result.target_found && result.current_sample_in_filter, "sample-name locate should jump to an included match");
    Require(result.current_index == 1, "sample-name locate should return the filtered match");

    result = controller.Navigate(spectiary::SampleNavigationRequest::RestoreLabelUndoPosition(4));
    Require(
        result.target_found && result.moved,
        "label undo should restore its source row outside the active sample navigation sequence");
    Require(result.current_index == 4, "label undo should return to the affected source row");
    Require(
        !result.current_sample_in_filter,
        "restored undo row may remain outside the active sample navigation sequence");
}

void TestEmptyFilterClearsCurrentSequenceRow()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "spectiary_nav_empty_filter.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "spectiary_nav_empty_filter_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 3, 1));
    Require(controller.current_index() && *controller.current_index() == 1, "test should start at row 1");

    controller.SetSampleFilter({false, false, false});
    Require(controller.filter_active(), "empty filter should still be active");
    Require(controller.filtered_sample_count() == 0, "empty filter should expose zero sequence rows");
    Require(!controller.current_index(), "empty active sequence should not expose a current row");
    Require(!controller.current_sample_in_filter(), "empty active filter should not report an in-filter current row");
    Require(!controller.can_move_previous() && !controller.can_move_next(), "empty sequence should not move");

    const spectiary::SampleNavigationResult result =
        controller.Navigate(spectiary::SampleNavigationRequest::Next());
    Require(result.has_active_source, "empty sequence should still belong to the active source");
    Require(result.sequence_active && result.sequence_empty, "result should expose empty active sequence state");
    Require(!result.has_current_sample, "empty sequence result should not expose a current sample");
    Require(!result.target_found, "empty sequence navigation should not produce a target");

    controller.ClearSampleFilter();
    Require(controller.current_index() && *controller.current_index() == 1, "clearing filter should restore the pre-filter row");
}

void TestSortOnlyRowLocateIsUnavailableButNotBlockedByFilter()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "spectiary_nav_sort_only.npy";
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_sort_only_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));

    spectiary::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 3, 0));
    spectiary::SampleNavigationSortChoice sort;
    sort.values = {
        spectiary::MakeSampleNavigationSortValue(3.0),
        spectiary::MakeSampleNavigationSortValue(2.0),
        spectiary::MakeSampleNavigationSortValue(1.0),
    };
    (void)controller.SetSampleSorting(std::move(sort));
    Require(controller.sorting_active(), "sort-only fixture should activate sorting");
    Require(!controller.filter_active(), "sort-only fixture must not activate filtering");

    const spectiary::SampleNavigationResult result =
        controller.Navigate(spectiary::SampleNavigationRequest::LocateRow(2));
    Require(!result.target_found, "ordinary row locate should be unavailable in sorted order");
    Require(
        !result.blocked_by_filter,
        "sort-only row location must not claim that an inactive filter blocked the target");
}

void TestSequencePositionLocateFollowsFilteredSortedOrder()
{
    spectiary::SampleNavigationController controller(
        std::filesystem::path{});
    controller.ActivateSource(
        "source",
        MakeSnapshot(
            "C:/synthetic/sequence-position.npy",
            "sequence-position",
            5,
            0),
        spectiary::SourceCollectionIdentity{
            .id = "sequence-position-identity",
            .source_name = "sequence-position",
            .source_fingerprint = "source",
            .context_fingerprint = "context",
            .spectrum_count = 5,
        },
        {});
    (void)controller.SetSampleFilter(
        {false, true, true, false, true});
    spectiary::SampleNavigationSortChoice sort;
    sort.values = {
        spectiary::MakeSampleNavigationSortValue(50.0),
        spectiary::MakeSampleNavigationSortValue(40.0),
        spectiary::MakeSampleNavigationSortValue(20.0),
        spectiary::MakeSampleNavigationSortValue(60.0),
        spectiary::MakeSampleNavigationSortValue(10.0),
    };
    (void)controller.SetSampleSorting(std::move(sort));

    spectiary::SampleNavigationResult result =
        controller.Navigate(
            spectiary::SampleNavigationRequest::
                LocateSequencePosition(0));
    Require(
        result.target_found && result.moved &&
            result.current_index == 4 &&
            result.current_sequence_position == 0,
        "sequence position 0 should resolve the first filtered and sorted source row");

    result = controller.NavigateDeferred(
        spectiary::SampleNavigationRequest::
            LocateSequencePosition(1),
        false);
    Require(
        result.target_found && result.moved &&
            result.current_index == 2 &&
            result.current_sequence_position == 1 &&
            controller.pending_index() == 2,
        "deferred sequence-position locate should target the source row at the requested active position");

    Require(
        controller.CommitDeferredNavigation(2),
        "the deferred sequence-position target should commit");
    result = controller.Navigate(
        spectiary::SampleNavigationRequest::
            LocateSequencePosition(3));
    Require(
        !result.target_found && !result.moved &&
            result.current_index == 2,
        "sequence-position locate should reject positions outside the active sequence");
}

void TestDeferredNumericPrefixesAdmitOnlyTheLatestTarget()
{
    spectiary::SampleNavigationController controller(
        std::filesystem::path{});
    controller.ActivateSource(
        "source",
        MakeSnapshot(
            "C:/synthetic/live-numeric-navigation.npy",
            "live-numeric-navigation",
            50,
            0));

    const spectiary::SampleNavigationResult prefix =
        controller.NavigateDeferred(
            spectiary::SampleNavigationRequest::LocateRow(3),
            false);
    const spectiary::SampleNavigationResult completed =
        controller.NavigateDeferred(
            spectiary::SampleNavigationRequest::LocateRow(44),
            false);
    Require(
        prefix.target_found && prefix.current_index == 3 &&
            completed.target_found &&
            completed.current_index == 44 &&
            controller.pending_index() == 44,
        "rapid numeric prefixes should replace the pending navigation target with the latest row");
    Require(
        !controller.CommitDeferredNavigation(3) &&
            controller.current_index() == 0 &&
            controller.pending_index() == 44,
        "completion for an older numeric prefix should be rejected as stale");
    Require(
        controller.CommitDeferredNavigation(44) &&
            controller.current_index() == 44 &&
            !controller.pending_index(),
        "the latest numeric target should remain admissible");
}

void TestDeferredNavigationReusesSequenceStateAcrossCursorTransitions()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_deferred_sequence_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    spectiary::SampleNavigationController controller(cache_path);
    spectiary::SourceCollectionManifest manifest;
    manifest.sample_names = {"zero", "one", "two", "three", "four"};
    controller.ActivateSource(
        "source",
        MakeSnapshot("C:/synthetic/deferred-sequence.npy", "deferred-sequence", 5, 1),
        spectiary::SourceCollectionIdentity{
            .id = "deferred-sequence-identity",
            .source_name = "deferred-sequence",
            .source_fingerprint = "deferred-sequence-source",
            .context_fingerprint = "deferred-sequence-context",
            .spectrum_count = 5,
        },
        std::move(manifest));

    spectiary::NavigationTargetResolutionReport report;
    const auto navigate_warm =
        [&controller, &report](
            const spectiary::SampleNavigationRequest& request,
            bool remember_labeling_position = false,
            std::optional<std::size_t> base_index = std::nullopt) {
            report = {};
            const spectiary::SampleNavigationResult result = controller.NavigateDeferred(
                request,
                remember_labeling_position,
                base_index,
                &report);
            Require(report.sequence_cache_hit, "warm deferred navigation should reuse sequence state");
            Require(
                report.sequence_build_count == 0,
                "warm deferred navigation should not rebuild its sequence state");
            Require(
                report.base_sequence_ns >= 0 && report.target_sequence_ns >= 0,
                "cursor projection timings should remain valid non-negative measurements");
            return result;
        };

    spectiary::SampleNavigationResult result =
        navigate_warm(spectiary::SampleNavigationRequest::Next());
    Require(
        result.target_found && result.current_index == 2 &&
            controller.current_index() == 1 && controller.pending_index() == 2 &&
            !report.pending_present,
        "first deferred next should project from committed row 1 without committing it");

    result = navigate_warm(spectiary::SampleNavigationRequest::Next());
    Require(
        result.current_index == 3 && controller.current_index() == 1 &&
            controller.pending_index() == 3 && report.pending_present,
        "rapid next should project from the pending cursor");

    Require(controller.RetargetDeferredNavigation(4), "retarget should preserve deferred ownership");
    result = navigate_warm(spectiary::SampleNavigationRequest::Previous());
    Require(
        result.current_index == 3 && controller.current_index() == 1 &&
            controller.pending_index() == 3,
        "retargeted previous should project from the replacement pending cursor");

    controller.CancelDeferredNavigation();
    Require(
        controller.current_index() == 1 && !controller.pending_index(),
        "cancel should retain the committed cursor and clear only pending intent");

    result = navigate_warm(spectiary::SampleNavigationRequest::Next(), true);
    Require(
        result.current_index == 2 && controller.pending_navigation_remembers_labeling_position(),
        "deferred navigation should retain labeling-position intent");
    result = navigate_warm(spectiary::SampleNavigationRequest::Next(), false, 1);
    Require(
        result.current_index == 2 && controller.pending_navigation_remembers_labeling_position(),
        "retargeting the same pending row must preserve the stronger labeling-position intent");
    Require(controller.CommitDeferredNavigation(2), "matching pending navigation should commit");
    Require(
        controller.current_index() == 2 && !controller.pending_index() &&
            !controller.pending_navigation_remembers_labeling_position(),
        "commit should promote pending to committed and clear pending metadata");

    result = navigate_warm(spectiary::SampleNavigationRequest::LabelAdvanceToEligible(
        {false, false, false, false, true}));
    Require(result.current_index == 4, "label advance should evaluate the first eligibility set");
    controller.CancelDeferredNavigation();
    result = navigate_warm(spectiary::SampleNavigationRequest::LabelAdvanceToEligible(
        {false, false, false, true, false}));
    Require(
        result.current_index == 3,
        "label advance eligibility must be recomputed instead of cached with sequence topology");
    controller.CancelDeferredNavigation();

    for (std::size_t repetition = 0; repetition < 100; ++repetition) {
        result = navigate_warm(spectiary::SampleNavigationRequest::Next());
        Require(result.current_index == 3, "repeated warm next should resolve from committed row 2");
        controller.CancelDeferredNavigation();
        result = navigate_warm(spectiary::SampleNavigationRequest::Previous());
        Require(result.current_index == 1, "repeated warm previous should resolve from committed row 2");
        controller.CancelDeferredNavigation();
    }
}

void TestSequenceStateInvalidatesWithNavigationInputsAndContext()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "spectiary_nav_sequence_invalidation.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    spectiary::SampleNavigationController controller(cache_path);
    spectiary::SourceCollectionManifest manifest;
    manifest.sample_names = {"alpha", "beta", "gamma", "delta", "omega"};
    const auto activate = [&controller](
                              std::string source_fingerprint,
                              std::string context_fingerprint,
                              spectiary::SourceCollectionManifest next_manifest) {
        controller.ActivateSource(
            "source",
            MakeSnapshot("C:/synthetic/sequence-invalidation.npy", "sequence-invalidation", 5, 0),
            spectiary::SourceCollectionIdentity{
                .id = "sequence-invalidation-identity",
                .source_name = "sequence-invalidation",
                .source_fingerprint = std::move(source_fingerprint),
                .context_fingerprint = std::move(context_fingerprint),
                .spectrum_count = 5,
            },
            std::move(next_manifest));
    };
    activate("source-v1", "context-v1", std::move(manifest));

    (void)controller.SetSampleNameQuery("ta");
    Require(
        controller.sample_name_matches() == std::vector<std::size_t>({1, 3}),
        "query change should build matches from the current context");
    Require(
        controller.current_sequence().sample_name_matches.empty(),
        "search matches should remain separate from cached navigation topology");

    (void)controller.SetSampleFilter({false, true, false, true, false});
    Require(controller.current_index() == 1, "filter change should reconcile onto row 1");
    spectiary::NavigationTargetResolutionReport report;
    spectiary::SampleNavigationResult result = controller.NavigateDeferred(
        spectiary::SampleNavigationRequest::Next(),
        false,
        std::nullopt,
        &report);
    Require(
        result.current_index == 3 && report.sequence_cache_hit &&
            report.sequence_build_count == 0,
        "deferred next should use the rebuilt filtered topology");
    controller.CancelDeferredNavigation();

    spectiary::SampleNavigationSortChoice sort;
    sort.values = {
        spectiary::MakeSampleNavigationSortValue(0.0),
        spectiary::MakeSampleNavigationSortValue(1.0),
        spectiary::MakeSampleNavigationSortValue(2.0),
        spectiary::MakeSampleNavigationSortValue(3.0),
        spectiary::MakeSampleNavigationSortValue(4.0),
    };
    sort.direction = spectiary::SampleNavigationSortDirection::Descending;
    (void)controller.SetSampleSorting(std::move(sort));
    Require(
        controller.sample_name_matches() == std::vector<std::size_t>({3, 1}),
        "query matches should follow the filtered and sorted topology");
    report = {};
    result = controller.NavigateDeferred(
        spectiary::SampleNavigationRequest::Previous(),
        false,
        std::nullopt,
        &report);
    Require(
        result.current_index == 3 && report.sequence_cache_hit &&
            report.sequence_build_count == 0,
        "sort change should replace the cached topology before deferred navigation");
    controller.CancelDeferredNavigation();

    spectiary::SourceCollectionManifest changed_manifest;
    changed_manifest.sample_names = {"zero", "one", "two", "three", "four"};
    activate("source-v2", "context-v2", std::move(changed_manifest));
    Require(
        controller.sample_name_matches().empty(),
        "source/context change must not retain query matches from the old manifest");
    (void)controller.SetSampleNameQuery("thr");
    Require(
        controller.sample_name_matches() == std::vector<std::size_t>({3}),
        "query matches should rebuild from the replacement context");

    spectiary::SourceCollectionManifest second_manifest;
    second_manifest.sample_names = {"a", "b", "c", "d", "e"};
    controller.ActivateSource(
        "source-two",
        MakeSnapshot("C:/synthetic/sequence-invalidation-two.npy", "sequence-invalidation-two", 5, 0),
        spectiary::SourceCollectionIdentity{
            .id = "sequence-invalidation-identity-two",
            .source_name = "sequence-invalidation-two",
            .source_fingerprint = "source-two",
            .context_fingerprint = "context-two",
            .spectrum_count = 5,
        },
        std::move(second_manifest));
    Require(
        controller.ActivateKnownSource("source", "C:/synthetic/sequence-invalidation.npy").has_value(),
        "source switch fixture should restore the first known source");
    report = {};
    result = controller.NavigateDeferred(
        spectiary::SampleNavigationRequest::Previous(),
        false,
        std::nullopt,
        &report);
    Require(
        result.current_index == 3 && !report.sequence_cache_hit &&
            report.sequence_build_count == 1,
        "switching sources should fully invalidate and cold-build the restored sequence state once");
    controller.CancelDeferredNavigation();
    report = {};
    result = controller.NavigateDeferred(
        spectiary::SampleNavigationRequest::Previous(),
        false,
        std::nullopt,
        &report);
    Require(
        result.current_index == 3 && report.sequence_cache_hit &&
            report.sequence_build_count == 0,
        "the next same-source navigation should use the newly warmed state");
}

void TestAdjacentRowsFollowFilteredSortedRawSequence()
{
    spectiary::SampleNavigationController controller(
        std::filesystem::path{});
    spectiary::SourceCollectionManifest manifest;
    manifest.sample_names = {
        "zero",
        "one",
        "two",
        "three",
        "four",
        "five",
    };
    controller.ActivateSource(
        "source",
        MakeSnapshot(
            "C:/synthetic/prefetch-sequence.npy",
            "prefetch-sequence",
            6,
            2),
        spectiary::SourceCollectionIdentity{
            .id = "prefetch-sequence-identity",
            .source_name = "prefetch-sequence",
            .source_fingerprint = "prefetch-source",
            .context_fingerprint = "prefetch-context",
            .spectrum_count = 6,
        },
        std::move(manifest));
    (void)controller.SetSampleFilter(
        {false, true, true, false, true, true});
    spectiary::SampleNavigationSortChoice sort;
    sort.active = true;
    sort.values = {
        spectiary::MakeSampleNavigationSortValue(50.0),
        spectiary::MakeSampleNavigationSortValue(40.0),
        spectiary::MakeSampleNavigationSortValue(20.0),
        spectiary::MakeSampleNavigationSortValue(60.0),
        spectiary::MakeSampleNavigationSortValue(10.0),
        spectiary::MakeSampleNavigationSortValue(30.0),
    };
    (void)controller.SetSampleSorting(std::move(sort));

    const std::vector<std::size_t> next_rows =
        controller.AdjacentRows(
            spectiary::SampleNavigationDirection::Next,
            {.ahead = 2, .behind = 1});
    const std::vector<std::size_t> previous_rows =
        controller.AdjacentRows(
            spectiary::SampleNavigationDirection::Previous,
            {.ahead = 2, .behind = 1});

    Require(
        next_rows ==
            std::vector<std::size_t>({5, 1, 4}),
        "next prefetch policy must return raw rows from the filtered and sorted sequence");
    Require(
        previous_rows ==
            std::vector<std::size_t>({4, 5}),
        "previous prefetch policy must reverse direction without leaving the active sequence");
}

void TestSequenceTopologyRevisionExcludesCursorMovement()
{
    spectiary::SampleNavigationController controller(
        std::filesystem::path{});
    controller.ActivateSource(
        "source",
        MakeSnapshot(
            "C:/synthetic/sequence-topology-revision.npy",
            "sequence-topology-revision",
            5,
            0),
        spectiary::SourceCollectionIdentity{
            .id = "sequence-topology-revision-identity",
            .source_name = "sequence-topology-revision",
            .source_fingerprint = "source",
            .context_fingerprint = "context",
            .spectrum_count = 5,
        },
        {});
    const std::uint64_t activated_revision =
        controller.sequence_topology_revision();

    const spectiary::SampleNavigationResult deferred =
        controller.NavigateDeferred(
            spectiary::SampleNavigationRequest::
                LocateSequencePosition(2),
            false);
    Require(
        deferred.target_found &&
            controller.sequence_topology_revision() ==
                activated_revision,
        "pending sequence-position navigation must not change the topology revision");
    Require(
        controller.CommitDeferredNavigation(2) &&
            controller.sequence_topology_revision() ==
                activated_revision,
        "committing a pending cursor must not change the topology revision");

    (void)controller.SetSampleFilter(
        {true, true, true, true, true});
    const std::uint64_t filtered_revision =
        controller.sequence_topology_revision();
    Require(
        filtered_revision > activated_revision,
        "changing the active filter should advance the topology revision");

    (void)controller.SetSampleFilter(
        {true, true, true, true, true});
    Require(
        controller.sequence_topology_revision() ==
            filtered_revision,
        "reapplying the same effective filter topology must not advance the revision");

    (void)controller.SetSampleNameQuery("sample");
    Require(
        controller.sequence_topology_revision() ==
            filtered_revision,
        "changing a search query must not advance the sequence topology revision");

    spectiary::SampleNavigationSortChoice sort;
    sort.values = {
        spectiary::MakeSampleNavigationSortValue(5.0),
        spectiary::MakeSampleNavigationSortValue(4.0),
        spectiary::MakeSampleNavigationSortValue(3.0),
        spectiary::MakeSampleNavigationSortValue(2.0),
        spectiary::MakeSampleNavigationSortValue(1.0),
    };
    (void)controller.SetSampleSorting(std::move(sort));
    const std::uint64_t sorted_revision =
        controller.sequence_topology_revision();
    Require(
        sorted_revision > filtered_revision,
        "changing sorting should advance the sequence topology revision");

    spectiary::SampleNavigationSortChoice equivalent_sort;
    equivalent_sort.values = {
        spectiary::MakeSampleNavigationSortValue(50.0),
        spectiary::MakeSampleNavigationSortValue(40.0),
        spectiary::MakeSampleNavigationSortValue(30.0),
        spectiary::MakeSampleNavigationSortValue(20.0),
        spectiary::MakeSampleNavigationSortValue(10.0),
    };
    (void)controller.SetSampleSorting(
        std::move(equivalent_sort));
    Require(
        controller.sequence_topology_revision() ==
            sorted_revision,
        "sorting input changes that retain the effective row order must not advance the topology revision");
}

void TestExactNameResolutionIsDeterministic()
{
    spectiary::SampleNavigationController controller(
        std::filesystem::path{});
    spectiary::SourceCollectionManifest manifest;
    manifest.sample_names = {
        "Alpha",
        "bravo",
        "ALPHA",
        "charlie",
        "Étoile",
    };
    controller.ActivateSource(
        "source",
        MakeSnapshot(
            "C:/synthetic/exact-name.npy",
            "exact-name",
            5,
            0),
        spectiary::SourceCollectionIdentity{
            .id = "exact-name-identity",
            .source_name = "exact-name",
            .source_fingerprint = "source",
            .context_fingerprint = "context",
            .spectrum_count = 5,
        },
        std::move(manifest));

    const auto unique =
        controller.ResolveExactSampleName("BRAVO");
    const auto ambiguous =
        controller.ResolveExactSampleName("alpha");
    const auto missing =
        controller.ResolveExactSampleName("delta");
    const auto unicode =
        controller.ResolveExactSampleName(
            "éTOILE");
    (void)controller.SetSampleFilter(
        {true, false, true, true, true});
    const auto filtered =
        controller.ResolveExactSampleName("bravo");

    Require(
        unique.names_available &&
            unique.matching_rows ==
                std::vector<std::size_t>{1} &&
            unique.first_match_in_active_sequence,
        "exact spectrum names should resolve case-insensitively to one active source row");
    Require(
        ambiguous.matching_rows ==
            std::vector<std::size_t>({0, 2}),
        "duplicate exact spectrum names should remain explicitly ambiguous");
    Require(
        missing.names_available &&
            missing.matching_rows.empty(),
        "an available name column should distinguish a missing exact name");
    Require(
        unicode.matching_rows ==
                std::vector<std::size_t>{4} &&
            unicode.first_match_in_active_sequence,
        "exact spectrum names should use Windows Unicode ordinal ignore-case matching rather than ASCII-only folding");
    Require(
        filtered.matching_rows ==
                std::vector<std::size_t>{1} &&
            !filtered.first_match_in_active_sequence,
        "an exact name outside the active sequence should remain identifiable but blocked");
}

}  // namespace

int main()
{
    try {
    TestNavigationStateCacheRoundTrip();
    TestNavigationStateCacheIgnoresCorruptJson();
    TestNavigationStateCacheIgnoresUnsupportedSchema();
    TestControllerOwnsNavigationState();
    TestControllerReloadsCompanionContextOnReactivate();
    TestControllerAddsManualAnnotationToActiveContext();
    TestControllerRestoresAndRemovesProvidedAnnotations();
    TestControllerAttachesAsdfLabelingDocumentForActiveSource();
    TestControllerAttachesFolderCsvByFilenameIdentity();
    TestInvalidDisplayNamesExportAndAttachByCanonicalIndex();
    TestPreparedSourceAttachesFolderCsvByCanonicalIdentity();
    TestAnnotationPathLookupUsesOnlyInMemorySourceIdentity();
    TestControllerPersistsLastIndexBySourceIdentity();
    TestControllerDebouncesNavigationStatePersistence();
    TestControllerCoalescesNavigationStateAndRetriesFailure();
    TestControllerClearsNavigationLoadWarningAfterFlush();
    TestCoordinatorFlushesWorkflowIndependentlyAndRecovers();
    TestControllerAdoptsAndMergesPreparedNavigationCache();
    TestCoordinatorMaintainsFlushesAndRestoresNavigationState();
    TestControllerLoadsLongFolderIdentityState();
    TestRemoveSourceUsesExternalSourceKey();
    TestFilterConstrainsSequentialNavigation();
    TestExplicitSampleOutsideNavigationSequence();
    TestEmptyFilterClearsCurrentSequenceRow();
    TestSortOnlyRowLocateIsUnavailableButNotBlockedByFilter();
    TestSequencePositionLocateFollowsFilteredSortedOrder();
    TestDeferredNumericPrefixesAdmitOnlyTheLatestTarget();
    TestDeferredNavigationReusesSequenceStateAcrossCursorTransitions();
    TestSequenceStateInvalidatesWithNavigationInputsAndContext();
    TestSequenceTopologyRevisionExcludesCursorMovement();
    TestAdjacentRowsFollowFilteredSortedRawSequence();
    TestExactNameResolutionIsDeterministic();
    } catch (const std::exception& error) {
        std::cerr << "sample navigation controller test failure: "
                  << error.what() << '\n';
        return 1;
    }
    return 0;
}
