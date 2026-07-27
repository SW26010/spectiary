#include "domain/spectrum_snapshot.h"
#include "profile/navigation_latency_trace.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_navigation_state_cache_io.h"
#include "ui/sample_workflow_coordinator.h"
#include "ui/source_collection_session.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
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

specforge::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::string source_id,
    std::size_t spectrum_count,
    std::size_t current_index)
{
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
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
        std::filesystem::temp_directory_path() / "specforge_nav_adapter_roundtrip.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    specforge::SampleNavigationStateCache cache;
    cache.last_indices_by_source_identity.emplace("source-a", 2);
    cache.last_indices_by_source_identity.emplace("source-b", 0);
    Require(specforge::SaveSampleNavigationStateCache(cache_path, cache), "navigation cache should save");

    const specforge::SampleNavigationStateCache loaded =
        specforge::LoadSampleNavigationStateCache(cache_path);
    Require(
        loaded.last_indices_by_source_identity.size() == 2,
        "navigation cache should restore all source indices");
    Require(
        loaded.last_indices_by_source_identity.at("source-a") == 2,
        "navigation cache should restore source-a index");
    Require(
        loaded.last_indices_by_source_identity.at("source-b") == 0,
        "navigation cache should restore source-b index");
}

void TestNavigationStateCacheIgnoresCorruptJson()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_nav_adapter_corrupt.json";
    WriteTextFile(cache_path, "{ invalid json");

    const specforge::SampleNavigationStateCache loaded =
        specforge::LoadSampleNavigationStateCache(cache_path);
    Require(
        loaded.last_indices_by_source_identity.empty(),
        "corrupt navigation cache should be ignored");
}

void TestNavigationStateCacheIgnoresUnsupportedSchema()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_nav_adapter_schema.json";
    WriteTextFile(
        cache_path,
        "{\n"
        "  \"format_kind\": \"specforge.sample_navigation_state.cache\",\n"
        "  \"schema_version\": 999,\n"
        "  \"sources\": [\n"
        "    { \"identity\": \"source-a\", \"last_index\": 2 }\n"
        "  ]\n"
        "}\n");

    const specforge::SampleNavigationStateCache loaded =
        specforge::LoadSampleNavigationStateCache(cache_path);
    Require(
        loaded.last_indices_by_source_identity.empty(),
        "unsupported navigation cache schema should be ignored");
}

void TestControllerOwnsNavigationState()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_nav_controller.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "specforge_nav_controller_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));
    WriteNpy(path.parent_path() / "specforge_nav_controller_name.npy", "<U5", {3}, UnicodeNpyBytesFor({"alpha", "beta", "gamma"}, 5));

    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 3, 1));
    Require(controller.current_index() && *controller.current_index() == 1, "controller should own initial current index");
    Require(controller.can_move_previous(), "index 1 should move previous");
    Require(controller.can_move_next(), "index 1 should move next");

    specforge::SampleNavigationResult result =
        controller.Navigate(specforge::SampleNavigationRequest::Previous());
    Require(result.has_active_source, "previous request should resolve against active source");
    Require(result.target_found, "previous target should be found");
    Require(result.moved, "previous request should move from index 1");
    Require(result.previous_index == 1, "previous result should report old index");
    Require(result.current_index == 0, "previous result should report actual index");
    Require(controller.current_index() && *controller.current_index() == 0, "controller should store previous result index");
    Require(!controller.can_move_previous(), "index 0 should not move previous");

    result = controller.Navigate(specforge::SampleNavigationRequest::Previous());
    Require(result.target_found, "boundary previous request should still resolve");
    Require(!result.moved, "boundary previous request should not move");
    Require(result.current_index == 0, "boundary previous result should keep actual index");

    result = controller.Navigate(specforge::SampleNavigationRequest::Next());
    Require(result.moved, "next request should move");
    Require(result.current_index == 1, "next request should return index 1");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateRow(2));
    Require(result.target_found, "valid row locate should resolve");
    Require(result.moved, "row locate should move");
    Require(result.current_index == 2, "row locate should return requested row");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateRow(42));
    Require(!result.target_found, "invalid row locate should not resolve");
    Require(!result.moved, "invalid row locate should not move");
    Require(result.current_index == 2, "invalid row locate should report unchanged actual index");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateSampleName("beta"));
    Require(result.target_found, "valid sample-name locate should resolve");
    Require(result.moved, "sample-name locate should move");
    Require(result.current_index == 1, "sample-name locate should return matched index");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateSampleName("missing"));
    Require(!result.target_found, "missing sample-name locate should not resolve");
    Require(result.current_index == 1, "missing sample-name locate should report unchanged actual index");
}

void TestControllerReloadsCompanionContextOnReactivate()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_nav_context.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "specforge_nav_context_state.json";
    const std::filesystem::path name_path = path.parent_path() / "specforge_nav_context_name.npy";
    const std::filesystem::path annotation_path = path.parent_path() / "specforge_nav_context_y.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {2, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0}));
    WriteNpy(name_path, "<U5", {2}, UnicodeNpyBytesFor({"alpha", "beta"}, 5));
    WriteNpy(annotation_path, "<i4", {2}, BytesFor<std::int32_t>({1, 2}));

    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 2, 0));
    const specforge::SourceCollectionManifest* context = controller.active_context();
    Require(context != nullptr, "active context should exist");
    Require(context->sample_names[0] == "alpha", "initial name should load");
    Require(
        specforge::FormatSampleAnnotationValue(
            context->annotations[0],
            context->annotations[0].values[0]) == "1",
        "initial annotation should load");

    specforge::SampleNavigationResult result = controller.Navigate(specforge::SampleNavigationRequest::Next());
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
        specforge::FormatSampleAnnotationValue(
            context->annotations[0],
            context->annotations[0].values[0]) == "42",
        "reactivate should reload changed annotations");
    Require(controller.current_index() && *controller.current_index() == 1, "context reload should preserve current index");
}

void TestControllerAddsManualAnnotationToActiveContext()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_nav_manual_annotation.npy";
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_nav_manual_annotation_state.json";
    const std::filesystem::path annotation_path =
        std::filesystem::temp_directory_path() / "specforge_nav_manual_annotation_score.npy";
    const std::filesystem::path mismatched_path =
        std::filesystem::temp_directory_path() / "specforge_nav_manual_annotation_mismatch.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {2, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0}));
    WriteNpy(annotation_path, "<i4", {2}, BytesFor<std::int32_t>({7, -1}));
    WriteNpy(mismatched_path, "<i4", {1}, BytesFor<std::int32_t>({42}));

    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 2, 0));
    const std::uint64_t initial_generation =
        controller.active_context_generation();
    (void)controller.Navigate(
        specforge::SampleNavigationRequest::Next());
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
    const specforge::SourceCollectionManifest* context = controller.active_context();
    Require(context != nullptr, "active context should exist after manual annotation");
    Require(context->annotations.size() == 1, "manual annotation should be appended");
    Require(context->annotations[0].name == "specforge_nav_manual_annotation_score.npy", "annotation name should be file name");
    Require(
        specforge::FormatSampleAnnotationValue(
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
        specforge::FormatSampleAnnotationValue(
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
        !context->messages.empty() &&
            context->messages.back().find("NPY array length does not match") != std::string::npos,
        "rejected annotation should add a visible context message");
}

void TestControllerRestoresAndRemovesProvidedAnnotations()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_nav_provided_annotation_source.npy";
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_nav_provided_annotation_state.json";
    const std::filesystem::path annotation_path =
        std::filesystem::temp_directory_path() / "specforge_nav_provided_annotation_result.npy";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));
    WriteNpy(annotation_path, "<i4", {3}, BytesFor<std::int32_t>({5, -1, 7}));

    specforge::SampleNavigationController controller(cache_path);
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
    const specforge::SourceCollectionManifest* context = controller.active_context();
    Require(context != nullptr && context->annotations.size() == 1, "provided annotation should be visible");
    Require(context->annotations[0].path == annotation_path, "restored annotation should keep its path");
    Require(
        specforge::FormatSampleAnnotationValue(
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

void TestAnnotationPathLookupUsesOnlyInMemorySourceIdentity()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_nav_annotation_path_lookup_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    specforge::SampleNavigationController controller(cache_path);
    const specforge::SpectrumSnapshotHandle first_snapshot =
        MakeSnapshot("C:/synthetic/first-source.npy", "first-source", 1, 0);
    specforge::SourceCollectionManifest first_manifest;
    first_manifest.annotations.push_back(specforge::SampleAnnotationResult{
        .path = "C:/unavailable/../annotations/result.npy",
    });
    first_manifest.annotations.push_back(specforge::SampleAnnotationResult{
        .path = "c:/annotations/RESULT.npy",
    });
    controller.ActivateSource(
        "first-key",
        first_snapshot,
        specforge::SourceCollectionIdentity{
            .id = "first-identity",
            .source_name = "first",
            .source_fingerprint = "first-source-fingerprint",
            .context_fingerprint = "first-context-fingerprint",
            .spectrum_count = 1,
        },
        std::move(first_manifest));

    const specforge::SpectrumSnapshotHandle second_snapshot =
        MakeSnapshot("C:/synthetic/second-source.npy", "second-source", 1, 0);
    specforge::SourceCollectionManifest second_manifest;
    second_manifest.annotations.push_back(specforge::SampleAnnotationResult{
        .path = "//offline-server/share/second-result.npy",
    });
    controller.ActivateSource(
        "second-key",
        second_snapshot,
        specforge::SourceCollectionIdentity{
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
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_nav_persist.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "specforge_nav_persist_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));

    {
        specforge::SampleNavigationController controller(cache_path);
        controller.ActivateSource("source-a", MakeSnapshot(path, "file:any-path-a", 3, 0));
        const specforge::SampleNavigationResult result =
            controller.Navigate(specforge::SampleNavigationRequest::LocateRow(2));
        Require(result.current_index == 2, "first controller should navigate to row 2");
        Require(controller.FlushStateCache(), "normal shutdown flush should persist the final row");
    }

    {
        specforge::SampleNavigationController controller(cache_path);
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
        std::filesystem::temp_directory_path() / "specforge_nav_debounce_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    constexpr std::string_view kIdentity = "navigation-debounce-identity";
    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource(
        "source",
        MakeSnapshot("C:/synthetic/navigation-debounce.npy", "navigation-debounce", 3, 0),
        specforge::SourceCollectionIdentity{
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
        specforge::LoadSampleNavigationStateCache(cache_path)
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 0,
        "maintenance should persist the activated row");

    const specforge::SampleNavigationResult result =
        controller.Navigate(specforge::SampleNavigationRequest::LocateRow(1));
    Require(result.current_index == 1, "fixture should navigate to row 1");
    Require(
        specforge::LoadSampleNavigationStateCache(cache_path)
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 0,
        "navigation should not synchronously rewrite the cache");

    const auto navigation_deadline = controller.NextMaintenanceDeadline();
    Require(navigation_deadline.has_value(), "navigation should schedule debounced persistence");
    controller.RunMaintenance(*navigation_deadline - 1ms);
    Require(
        specforge::LoadSampleNavigationStateCache(cache_path)
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 0,
        "maintenance before the debounce deadline should not save");
    controller.RunMaintenance(*navigation_deadline);
    Require(
        specforge::LoadSampleNavigationStateCache(cache_path)
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 1,
        "maintenance at the debounce deadline should save the latest row");
}

void TestControllerCoalescesNavigationStateAndRetriesFailure()
{
    using namespace std::chrono_literals;

    const std::filesystem::path blocker =
        std::filesystem::temp_directory_path() / "specforge_nav_retry_blocker";
    const std::filesystem::path cache_path = blocker / "navigation-state.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(blocker, cleanup_error);
    WriteTextFile(blocker, "block parent directory creation");

    constexpr std::string_view kIdentity = "navigation-retry-identity";
    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource(
        "source",
        MakeSnapshot("C:/synthetic/navigation-retry.npy", "navigation-retry", 4, 0),
        specforge::SourceCollectionIdentity{
            .id = std::string{kIdentity},
            .source_name = "navigation-retry",
            .source_fingerprint = "source-v1",
            .context_fingerprint = "context-v1",
            .spectrum_count = 4,
        },
        {});
    (void)controller.Navigate(specforge::SampleNavigationRequest::LocateRow(1));
    (void)controller.Navigate(specforge::SampleNavigationRequest::LocateRow(3));

    Require(
        !std::filesystem::exists(cache_path),
        "continuous navigation should not write an intermediate row");
    const auto debounce_deadline = controller.NextMaintenanceDeadline();
    Require(debounce_deadline.has_value(), "coalesced navigation should retain one save deadline");
    controller.RunMaintenance(*debounce_deadline);

    const auto retry_deadline = controller.NextMaintenanceDeadline();
    Require(
        retry_deadline && *retry_deadline >= *debounce_deadline + 2s,
        "a failed save should remain dirty and schedule the existing retry backoff");

    std::filesystem::remove(blocker, cleanup_error);
    std::filesystem::create_directories(blocker);
    controller.RunMaintenance(*retry_deadline - 1ms);
    Require(
        !std::filesystem::exists(cache_path),
        "maintenance before the retry deadline should not save");
    controller.RunMaintenance(*retry_deadline);
    Require(
        specforge::LoadSampleNavigationStateCache(cache_path)
                .last_indices_by_source_identity.at(std::string{kIdentity}) == 3,
        "retry should persist only the final coalesced row");
}

void TestCoordinatorMaintainsFlushesAndRestoresNavigationState()
{
    const std::filesystem::path source_path =
        std::filesystem::temp_directory_path() / "specforge_nav_coordinator.npy";
    const std::filesystem::path navigation_cache =
        std::filesystem::temp_directory_path() / "specforge_nav_coordinator_state.json";
    const std::filesystem::path labeling_cache =
        std::filesystem::temp_directory_path() / "specforge_nav_coordinator_labeling.json";
    const std::filesystem::path workflow_cache =
        std::filesystem::temp_directory_path() / "specforge_nav_coordinator_workflow.json";
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
        specforge::SampleWorkflowCoordinator coordinator(
            navigation_cache,
            labeling_cache,
            workflow_cache);
        const specforge::SpectrumSnapshotHandle snapshot =
            MakeSnapshot(source_path, "navigation-coordinator", 3, 0);
        (void)coordinator.SyncActiveSource("source", snapshot);
        source_identity = coordinator.ActiveSourceIdentity()->id;
        coordinator.SetDeferredSampleNavigation(true);

        specforge::SampleWorkflowTransitionOutcome navigation =
            coordinator.Apply(
                specforge::SampleNavigationIntent::Move(
                    specforge::SampleNavigationRequest::LocateRow(1)),
                snapshot);
        Require(
            navigation.snapshot_index_to_load == 1 &&
                coordinator.CommitDeferredSampleNavigation(1),
            "coordinator should commit the first deferred row");
        Require(
            !std::filesystem::exists(navigation_cache),
            "deferred activation should not synchronously persist navigation state");

        for (int attempt = 0; attempt < 4 && !std::filesystem::exists(navigation_cache); ++attempt) {
            const auto deadline = coordinator.NextMaintenanceDeadline();
            Require(deadline.has_value(), "coordinator should expose navigation maintenance");
            (void)coordinator.RunMaintenance(*deadline);
        }
        Require(
            std::filesystem::exists(navigation_cache),
            "coordinator maintenance should reach the navigation deadline");
        Require(
            specforge::LoadSampleNavigationStateCache(navigation_cache)
                    .last_indices_by_source_identity.at(source_identity) == 1,
            "coordinator maintenance should persist the committed row");

        navigation = coordinator.Apply(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::LocateRow(2)),
            snapshot);
        Require(
            navigation.snapshot_index_to_load == 2 &&
                coordinator.CommitDeferredSampleNavigation(2),
            "coordinator should commit the final deferred row");
        Require(
            specforge::LoadSampleNavigationStateCache(navigation_cache)
                    .last_indices_by_source_identity.at(source_identity) == 1,
            "the final row should remain memory-only until flush");
        Require(coordinator.FlushStateCaches(), "normal shutdown flush should save navigation state");
    }

    specforge::SampleWorkflowCoordinator restored(
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
        std::filesystem::temp_directory_path() / "specforge_nav_long_folder_identity";
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_nav_long_folder_identity_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder_path, cleanup_error);
    std::filesystem::remove(cache_path, cleanup_error);
    std::filesystem::create_directories(folder_path);

    constexpr std::size_t kSampleCount = 40;
    for (std::size_t index = 0; index < kSampleCount; ++index) {
        std::ostringstream name;
        name << "specforge-long-folder-identity-sample-" << index << "-with-extra-cache-text.csv";
        std::ofstream stream(folder_path / name.str());
        Require(stream.good(), "could not write folder identity sample");
        stream << "wavelength,flux\n5000,1\n5001,2\n";
    }

    specforge::SpectrumSnapshotHandle snapshot = MakeSnapshot(folder_path, "folder:long-identity", kSampleCount, 0);
    specforge::SourceCollectionContext context = specforge::LoadSourceCollectionContext(*snapshot);
    const specforge::SourceCollectionFolderListing listing = specforge::ScanSourceCollectionFolder(folder_path);
    std::string legacy_fingerprint = "folder";
    for (const specforge::SourceCollectionFolderSpectrumFile& sample : listing.spectra) {
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
        stream << "  \"format_kind\": \"specforge.sample_navigation_state.cache\",\n";
        stream << "  \"schema_version\": 1,\n";
        stream << "  \"sources\": [\n";
        stream << "    { \"identity\": \"short-source\", \"last_index\": 0 },\n";
        stream << "    { \"identity\": \"" << legacy_identity << "\", \"last_index\": 7 }\n";
        stream << "  ]\n";
        stream << "}\n";
    }

    specforge::SampleNavigationController controller(cache_path);
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
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_nav_remove.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "specforge_nav_remove_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {2, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0}));

    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source-list-key", MakeSnapshot(path, "file:any-path", 2, 1));
    Require(controller.current_index() && *controller.current_index() == 1, "test source should activate");

    (void)controller.RemoveSource("source-list-key");
    Require(!controller.current_index(), "removed source should clear the active session");
    const specforge::SampleNavigationResult result =
        controller.Navigate(specforge::SampleNavigationRequest::Previous());
    Require(!result.has_active_source, "removed source should not handle navigation requests");
}

void TestFilterConstrainsSequentialNavigation()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_nav_filter.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "specforge_nav_filter_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(
        path,
        "<f8",
        {5, 2},
        BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0}));
    WriteNpy(
        path.parent_path() / "specforge_nav_filter_name.npy",
        "<U7",
        {5},
        UnicodeNpyBytesFor({"alpha", "beta", "gamma", "delta", "omega"}, 7));

    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 5, 0));
    controller.SetSampleFilter({false, true, false, true, false});
    Require(controller.filter_active(), "filter should be active");
    Require(controller.filtered_sample_count() == 2, "filter should count included samples");
    Require(controller.current_index() && *controller.current_index() == 1, "filter should move to the first included row");
    Require(controller.current_sample_in_filter(), "current row should be inside the sequence after filter reconciliation");
    Require(controller.can_move_next(), "first filtered row should move next");
    Require(!controller.can_move_previous(), "first filtered row should not move previous inside the filter");

    specforge::SampleNavigationResult result =
        controller.Navigate(specforge::SampleNavigationRequest::Next());
    Require(result.target_found && result.moved, "filtered next should move to the next included row");
    Require(result.current_index == 3, "filtered next should skip excluded rows");
    Require(!controller.can_move_next(), "last filtered row should not move next");

    result = controller.Navigate(specforge::SampleNavigationRequest::Previous());
    Require(result.target_found && result.moved, "filtered previous should move to prior included row");
    Require(result.current_index == 1, "filtered previous should skip excluded rows");

    result = controller.Navigate(specforge::SampleNavigationRequest::LabelAdvance());
    Require(result.target_found && result.moved, "label advance should move within the filtered source order");
    Require(result.current_index == 3, "label advance should let navigation choose the filtered target");

    result = controller.Navigate(specforge::SampleNavigationRequest::Previous());
    Require(result.target_found && result.moved, "test should return to the first filtered row");
    Require(result.current_index == 1, "test should return to row 1");

    result = controller.Navigate(
        specforge::SampleNavigationRequest::LabelAdvanceToEligible({false, false, false, true, false}));
    Require(result.target_found && result.moved, "eligible label advance should move to the next eligible filtered row");
    Require(result.current_index == 3, "eligible label advance should use navigation-owned filter state");

    result = controller.Navigate(specforge::SampleNavigationRequest::Previous());
    Require(result.target_found && result.moved, "test should return to the first filtered row again");
    result = controller.Navigate(
        specforge::SampleNavigationRequest::LabelAdvanceToEligible({false, false, true, false, false}));
    Require(result.target_found && !result.moved, "eligible label advance should ignore excluded rows");
    Require(result.current_index == 1, "label advance with no eligible filtered target should keep the current row");

    (void)controller.SetSampleNameQuery("a");
    const std::vector<std::size_t>& matches = controller.sample_name_matches();
    Require(matches.size() == 2, "sample-name matches should be filtered to included rows");
    Require(matches[0] == 1 && matches[1] == 3, "filtered sample-name matches should preserve source order");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateRow(4));
    Require(result.blocked_by_filter, "row locate should be blocked while filtering changes the sequence");
    Require(!result.target_found, "blocked row locate should not produce a target");
    Require(result.current_index == 1, "blocked row locate should keep the current sequence row");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateSampleName("omega"));
    Require(!result.target_found, "sample-name locate should not jump to an excluded exact match");
    Require(result.current_index == 1, "excluded sample-name locate should keep the current row");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateSampleName("beta"));
    Require(result.target_found && result.current_sample_in_filter, "sample-name locate should jump to an included match");
    Require(result.current_index == 1, "sample-name locate should return the filtered match");

    result = controller.Navigate(specforge::SampleNavigationRequest::RestoreLabelUndoPosition(4));
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
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_nav_empty_filter.npy";
    const std::filesystem::path cache_path = std::filesystem::temp_directory_path() / "specforge_nav_empty_filter_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));

    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 3, 1));
    Require(controller.current_index() && *controller.current_index() == 1, "test should start at row 1");

    controller.SetSampleFilter({false, false, false});
    Require(controller.filter_active(), "empty filter should still be active");
    Require(controller.filtered_sample_count() == 0, "empty filter should expose zero sequence rows");
    Require(!controller.current_index(), "empty active sequence should not expose a current row");
    Require(!controller.current_sample_in_filter(), "empty active filter should not report an in-filter current row");
    Require(!controller.can_move_previous() && !controller.can_move_next(), "empty sequence should not move");

    const specforge::SampleNavigationResult result =
        controller.Navigate(specforge::SampleNavigationRequest::Next());
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
        std::filesystem::temp_directory_path() / "specforge_nav_sort_only.npy";
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_nav_sort_only_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);
    WriteNpy(path, "<f8", {3, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));

    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource("source", MakeSnapshot(path, "file:source", 3, 0));
    specforge::SampleNavigationSortChoice sort;
    sort.values = {
        specforge::MakeSampleNavigationSortValue(3.0),
        specforge::MakeSampleNavigationSortValue(2.0),
        specforge::MakeSampleNavigationSortValue(1.0),
    };
    (void)controller.SetSampleSorting(std::move(sort));
    Require(controller.sorting_active(), "sort-only fixture should activate sorting");
    Require(!controller.filter_active(), "sort-only fixture must not activate filtering");

    const specforge::SampleNavigationResult result =
        controller.Navigate(specforge::SampleNavigationRequest::LocateRow(2));
    Require(!result.target_found, "ordinary row locate should be unavailable in sorted order");
    Require(
        !result.blocked_by_filter,
        "sort-only row location must not claim that an inactive filter blocked the target");
}

void TestDeferredNavigationReusesSequenceStateAcrossCursorTransitions()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_nav_deferred_sequence_state.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    specforge::SampleNavigationController controller(cache_path);
    specforge::SourceCollectionManifest manifest;
    manifest.sample_names = {"zero", "one", "two", "three", "four"};
    controller.ActivateSource(
        "source",
        MakeSnapshot("C:/synthetic/deferred-sequence.npy", "deferred-sequence", 5, 1),
        specforge::SourceCollectionIdentity{
            .id = "deferred-sequence-identity",
            .source_name = "deferred-sequence",
            .source_fingerprint = "deferred-sequence-source",
            .context_fingerprint = "deferred-sequence-context",
            .spectrum_count = 5,
        },
        std::move(manifest));

    specforge::NavigationTargetResolutionReport report;
    const auto navigate_warm =
        [&controller, &report](
            const specforge::SampleNavigationRequest& request,
            bool remember_labeling_position = false,
            std::optional<std::size_t> base_index = std::nullopt) {
            report = {};
            const specforge::SampleNavigationResult result = controller.NavigateDeferred(
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

    specforge::SampleNavigationResult result =
        navigate_warm(specforge::SampleNavigationRequest::Next());
    Require(
        result.target_found && result.current_index == 2 &&
            controller.current_index() == 1 && controller.pending_index() == 2 &&
            !report.pending_present,
        "first deferred next should project from committed row 1 without committing it");

    result = navigate_warm(specforge::SampleNavigationRequest::Next());
    Require(
        result.current_index == 3 && controller.current_index() == 1 &&
            controller.pending_index() == 3 && report.pending_present,
        "rapid next should project from the pending cursor");

    Require(controller.RetargetDeferredNavigation(4), "retarget should preserve deferred ownership");
    result = navigate_warm(specforge::SampleNavigationRequest::Previous());
    Require(
        result.current_index == 3 && controller.current_index() == 1 &&
            controller.pending_index() == 3,
        "retargeted previous should project from the replacement pending cursor");

    controller.CancelDeferredNavigation();
    Require(
        controller.current_index() == 1 && !controller.pending_index(),
        "cancel should retain the committed cursor and clear only pending intent");

    result = navigate_warm(specforge::SampleNavigationRequest::Next(), true);
    Require(
        result.current_index == 2 && controller.pending_navigation_remembers_labeling_position(),
        "deferred navigation should retain labeling-position intent");
    result = navigate_warm(specforge::SampleNavigationRequest::Next(), false, 1);
    Require(
        result.current_index == 2 && controller.pending_navigation_remembers_labeling_position(),
        "retargeting the same pending row must preserve the stronger labeling-position intent");
    Require(controller.CommitDeferredNavigation(2), "matching pending navigation should commit");
    Require(
        controller.current_index() == 2 && !controller.pending_index() &&
            !controller.pending_navigation_remembers_labeling_position(),
        "commit should promote pending to committed and clear pending metadata");

    result = navigate_warm(specforge::SampleNavigationRequest::LabelAdvanceToEligible(
        {false, false, false, false, true}));
    Require(result.current_index == 4, "label advance should evaluate the first eligibility set");
    controller.CancelDeferredNavigation();
    result = navigate_warm(specforge::SampleNavigationRequest::LabelAdvanceToEligible(
        {false, false, false, true, false}));
    Require(
        result.current_index == 3,
        "label advance eligibility must be recomputed instead of cached with sequence topology");
    controller.CancelDeferredNavigation();

    for (std::size_t repetition = 0; repetition < 100; ++repetition) {
        result = navigate_warm(specforge::SampleNavigationRequest::Next());
        Require(result.current_index == 3, "repeated warm next should resolve from committed row 2");
        controller.CancelDeferredNavigation();
        result = navigate_warm(specforge::SampleNavigationRequest::Previous());
        Require(result.current_index == 1, "repeated warm previous should resolve from committed row 2");
        controller.CancelDeferredNavigation();
    }
}

void TestSequenceStateInvalidatesWithNavigationInputsAndContext()
{
    const std::filesystem::path cache_path =
        std::filesystem::temp_directory_path() / "specforge_nav_sequence_invalidation.json";
    std::error_code cleanup_error;
    std::filesystem::remove(cache_path, cleanup_error);

    specforge::SampleNavigationController controller(cache_path);
    specforge::SourceCollectionManifest manifest;
    manifest.sample_names = {"alpha", "beta", "gamma", "delta", "omega"};
    const auto activate = [&controller](
                              std::string source_fingerprint,
                              std::string context_fingerprint,
                              specforge::SourceCollectionManifest next_manifest) {
        controller.ActivateSource(
            "source",
            MakeSnapshot("C:/synthetic/sequence-invalidation.npy", "sequence-invalidation", 5, 0),
            specforge::SourceCollectionIdentity{
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
    specforge::NavigationTargetResolutionReport report;
    specforge::SampleNavigationResult result = controller.NavigateDeferred(
        specforge::SampleNavigationRequest::Next(),
        false,
        std::nullopt,
        &report);
    Require(
        result.current_index == 3 && report.sequence_cache_hit &&
            report.sequence_build_count == 0,
        "deferred next should use the rebuilt filtered topology");
    controller.CancelDeferredNavigation();

    specforge::SampleNavigationSortChoice sort;
    sort.values = {
        specforge::MakeSampleNavigationSortValue(0.0),
        specforge::MakeSampleNavigationSortValue(1.0),
        specforge::MakeSampleNavigationSortValue(2.0),
        specforge::MakeSampleNavigationSortValue(3.0),
        specforge::MakeSampleNavigationSortValue(4.0),
    };
    sort.direction = specforge::SampleNavigationSortDirection::Descending;
    (void)controller.SetSampleSorting(std::move(sort));
    Require(
        controller.sample_name_matches() == std::vector<std::size_t>({3, 1}),
        "query matches should follow the filtered and sorted topology");
    report = {};
    result = controller.NavigateDeferred(
        specforge::SampleNavigationRequest::Previous(),
        false,
        std::nullopt,
        &report);
    Require(
        result.current_index == 3 && report.sequence_cache_hit &&
            report.sequence_build_count == 0,
        "sort change should replace the cached topology before deferred navigation");
    controller.CancelDeferredNavigation();

    specforge::SourceCollectionManifest changed_manifest;
    changed_manifest.sample_names = {"zero", "one", "two", "three", "four"};
    activate("source-v2", "context-v2", std::move(changed_manifest));
    Require(
        controller.sample_name_matches().empty(),
        "source/context change must not retain query matches from the old manifest");
    (void)controller.SetSampleNameQuery("thr");
    Require(
        controller.sample_name_matches() == std::vector<std::size_t>({3}),
        "query matches should rebuild from the replacement context");

    specforge::SourceCollectionManifest second_manifest;
    second_manifest.sample_names = {"a", "b", "c", "d", "e"};
    controller.ActivateSource(
        "source-two",
        MakeSnapshot("C:/synthetic/sequence-invalidation-two.npy", "sequence-invalidation-two", 5, 0),
        specforge::SourceCollectionIdentity{
            .id = "sequence-invalidation-identity-two",
            .source_name = "sequence-invalidation-two",
            .source_fingerprint = "source-two",
            .context_fingerprint = "context-two",
            .spectrum_count = 5,
        },
        std::move(second_manifest));
    Require(
        controller.ActivateKnownSource("source").has_value(),
        "source switch fixture should restore the first known source");
    report = {};
    result = controller.NavigateDeferred(
        specforge::SampleNavigationRequest::Previous(),
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
        specforge::SampleNavigationRequest::Previous(),
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
    specforge::SampleNavigationController controller(
        std::filesystem::path{});
    specforge::SourceCollectionManifest manifest;
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
        specforge::SourceCollectionIdentity{
            .id = "prefetch-sequence-identity",
            .source_name = "prefetch-sequence",
            .source_fingerprint = "prefetch-source",
            .context_fingerprint = "prefetch-context",
            .spectrum_count = 6,
        },
        std::move(manifest));
    (void)controller.SetSampleFilter(
        {false, true, true, false, true, true});
    specforge::SampleNavigationSortChoice sort;
    sort.active = true;
    sort.values = {
        specforge::MakeSampleNavigationSortValue(50.0),
        specforge::MakeSampleNavigationSortValue(40.0),
        specforge::MakeSampleNavigationSortValue(20.0),
        specforge::MakeSampleNavigationSortValue(60.0),
        specforge::MakeSampleNavigationSortValue(10.0),
        specforge::MakeSampleNavigationSortValue(30.0),
    };
    (void)controller.SetSampleSorting(std::move(sort));

    const std::vector<std::size_t> next_rows =
        controller.AdjacentRows(
            specforge::SampleNavigationDirection::Next,
            {.ahead = 2, .behind = 1});
    const std::vector<std::size_t> previous_rows =
        controller.AdjacentRows(
            specforge::SampleNavigationDirection::Previous,
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

}  // namespace

int main()
{
    TestNavigationStateCacheRoundTrip();
    TestNavigationStateCacheIgnoresCorruptJson();
    TestNavigationStateCacheIgnoresUnsupportedSchema();
    TestControllerOwnsNavigationState();
    TestControllerReloadsCompanionContextOnReactivate();
    TestControllerAddsManualAnnotationToActiveContext();
    TestControllerRestoresAndRemovesProvidedAnnotations();
    TestAnnotationPathLookupUsesOnlyInMemorySourceIdentity();
    TestControllerPersistsLastIndexBySourceIdentity();
    TestControllerDebouncesNavigationStatePersistence();
    TestControllerCoalescesNavigationStateAndRetriesFailure();
    TestCoordinatorMaintainsFlushesAndRestoresNavigationState();
    TestControllerLoadsLongFolderIdentityState();
    TestRemoveSourceUsesExternalSourceKey();
    TestFilterConstrainsSequentialNavigation();
    TestEmptyFilterClearsCurrentSequenceRow();
    TestSortOnlyRowLocateIsUnavailableButNotBlockedByFilter();
    TestDeferredNavigationReusesSequenceStateAcrossCursorTransitions();
    TestSequenceStateInvalidatesWithNavigationInputsAndContext();
    TestAdjacentRowsFollowFilteredSortedRawSequence();
    return 0;
}
