#include "domain/spectrum_snapshot.h"
#include "ui/sample_navigation_controller.h"
#include "ui/sample_navigation_state_cache_io.h"

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
    Require(context->annotations[0].values[0].display_text == "1", "initial annotation should load");

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
    Require(context->annotations[0].values[0].display_text == "42", "reactivate should reload changed annotations");
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

    std::string message;
    Require(
        controller.AddReadOnlyAnnotationToActiveSource(annotation_path, &message),
        "manual annotation should attach to active source");
    const specforge::SourceCollectionManifest* context = controller.active_context();
    Require(context != nullptr, "active context should exist after manual annotation");
    Require(context->annotations.size() == 1, "manual annotation should be appended");
    Require(context->annotations[0].name == "specforge_nav_manual_annotation_score.npy", "annotation name should be file name");
    Require(context->annotations[0].values[0].display_text == "7", "manual annotation should load first value");

    WriteNpy(annotation_path, "<i4", {2}, BytesFor<std::int32_t>({99, 100}));
    Require(
        controller.AddReadOnlyAnnotationToActiveSource(annotation_path, &message),
        "reopened manual annotation should replace same path");
    context = controller.active_context();
    Require(context != nullptr, "active context should still exist after replacement");
    Require(context->annotations.size() == 1, "same annotation path should replace instead of duplicating");
    Require(context->annotations[0].values[0].display_text == "99", "replacement should refresh annotation values");

    Require(
        !controller.AddReadOnlyAnnotationToActiveSource(mismatched_path, &message),
        "mismatched manual annotation should be rejected");
    context = controller.active_context();
    Require(context != nullptr, "active context should still exist after rejected annotation");
    Require(context->annotations.size() == 1, "rejected annotation should not be appended");
    Require(
        !context->messages.empty() &&
            context->messages.back().find("NPY array length does not match") != std::string::npos,
        "rejected annotation should add a visible context message");
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
    }

    {
        specforge::SampleNavigationController controller(cache_path);
        controller.ActivateSource("source-b", MakeSnapshot(path, "file:any-path-b", 3, 0));
        Require(controller.current_index() && *controller.current_index() == 2, "new controller should restore last row");
    }

    const std::string cache_text = ReadTextFile(cache_path);
    Require(cache_text.find(PathToUtf8(path.parent_path())) == std::string::npos, "cache should not key state by absolute directory path");
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
    const std::string identity = specforge::BuildSourceCollectionIdentity(*snapshot).id;
    Require(identity.size() > 1000, "test identity should be long enough to cover regex stack risk");

    {
        std::ofstream stream(cache_path);
        Require(stream.good(), "could not write navigation cache fixture");
        stream << "{\n";
        stream << "  \"format_kind\": \"specforge.sample_navigation_state.cache\",\n";
        stream << "  \"schema_version\": 1,\n";
        stream << "  \"sources\": [\n";
        stream << "    { \"identity\": \"short-source\", \"last_index\": 0 },\n";
        stream << "    { \"identity\": \"" << identity << "\", \"last_index\": 7 }\n";
        stream << "  ]\n";
        stream << "}\n";
    }

    specforge::SampleNavigationController controller(cache_path);
    controller.ActivateSource("folder-source", snapshot);
    Require(controller.current_index() && *controller.current_index() == 7, "long cached identity should restore index");
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

    controller.RemoveSource("source-list-key");
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
    Require(!controller.current_sample_in_filter(), "initial row should be outside the filter");
    Require(!controller.can_move_next(), "out-of-filter row should not move next");

    specforge::SampleNavigationResult result =
        controller.Navigate(specforge::SampleNavigationRequest::Next());
    Require(result.blocked_by_filter, "out-of-filter sequential move should be blocked");
    Require(!result.target_found, "blocked filtered move should not produce a target");
    Require(result.current_index == 0, "blocked filtered move should keep the current row");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateRow(1));
    Require(result.target_found && result.current_sample_in_filter, "direct row locate can enter the filter");
    Require(controller.can_move_next(), "first filtered row should move next");
    Require(!controller.can_move_previous(), "first filtered row should not move previous inside the filter");

    result = controller.Navigate(specforge::SampleNavigationRequest::Next());
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

    controller.SetSampleNameQuery("a");
    const std::vector<std::size_t>& matches = controller.sample_name_matches();
    Require(matches.size() == 2, "sample-name matches should be filtered to included rows");
    Require(matches[0] == 1 && matches[1] == 3, "filtered sample-name matches should preserve source order");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateRow(4));
    Require(result.target_found, "direct row locate can jump outside the filter");
    Require(!result.current_sample_in_filter, "direct row locate should expose out-of-filter state");
    result = controller.Navigate(specforge::SampleNavigationRequest::LabelAdvance());
    Require(result.blocked_by_filter, "label advance should be blocked when the current row is outside the filter");
    Require(!result.target_found, "blocked label advance should not produce a target");
    Require(result.current_index == 4, "blocked label advance should keep the current row");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateSampleName("omega"));
    Require(!result.target_found, "sample-name locate should not jump to an excluded exact match");
    Require(result.current_index == 4, "excluded sample-name locate should keep the current row");

    result = controller.Navigate(specforge::SampleNavigationRequest::LocateSampleName("beta"));
    Require(result.target_found && result.current_sample_in_filter, "sample-name locate should jump to an included match");
    Require(result.current_index == 1, "sample-name locate should return the filtered match");
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
    TestControllerPersistsLastIndexBySourceIdentity();
    TestControllerLoadsLongFolderIdentityState();
    TestRemoveSourceUsesExternalSourceKey();
    TestFilterConstrainsSequentialNavigation();
    return 0;
}
