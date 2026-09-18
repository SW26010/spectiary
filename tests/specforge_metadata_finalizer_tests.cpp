#include "app/specforge_metadata.h"
#include "app/specforge_metadata_finalizer.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void WriteTextFile(
    const std::filesystem::path& path,
    std::string_view contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not create finalizer fixture");
    stream << contents;
    Require(stream.good(), "could not write finalizer fixture");
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "could not read finalizer fixture");
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

bool HasTemporarySibling(const std::filesystem::path& root)
{
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root, error)) {
        if (error) {
            return true;
        }
        if (entry.path().filename().string().find(".tmp.") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

specforge::BuildIdentity WorkingTreeIdentity()
{
    return {
        .product_name = "Spectiary",
        .specforge_version = "0.4.1",
        .configuration = "Debug",
        .target_architecture = "amd64",
        .source_mode = "working_tree",
        .source_revision = "",
    };
}

specforge::BuildMetadata ConfiguredBuildMetadata()
{
    return {
        .compiler_id = "MSVC",
        .compiler_version = "19.44",
        .cmake_version = "4.1.0",
        .generator = "Ninja",
        .windows_sdk_version = std::nullopt,
        .dear_imgui_version = "1.92.5",
        .implot_version = "0.17",
        .cfitsio_version = "4.6.4",
        .yaml_cpp_version = "0.9.0",
        .zlib_version = "1.3.1",
    };
}

std::filesystem::path TestRoot(std::string_view name)
{
    return std::filesystem::temp_directory_path() /
        ("specforge-metadata-finalizer-" + std::string(name));
}

specforge::SpecForgeMetadataFinalizerOptions OptionsFor(
    const std::filesystem::path& root)
{
    const std::filesystem::path executable_path = root / "Spectiary.exe";
    return {
        .executable_path = executable_path,
        .metadata_path = root / "spectiary_metadata.json",
        .build_identity = WorkingTreeIdentity(),
        .configured_build_metadata = ConfiguredBuildMetadata(),
    };
}

std::chrono::system_clock::time_point FixedUtcTime()
{
    const auto day = std::chrono::sys_days{
        std::chrono::year{2026} /
        std::chrono::month{8} /
        std::chrono::day{5}};
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            day.time_since_epoch() +
            std::chrono::hours{9} +
            std::chrono::minutes{21} +
            std::chrono::seconds{32}));
}

void TestFinalizerWritesSchema6AndHashesFinalExecutable()
{
    const std::filesystem::path root = TestRoot("success");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path executable_path = root / "Spectiary.exe";
    const std::filesystem::path metadata_path =
        root / "spectiary_metadata.json";
    WriteTextFile(executable_path, "abc");

    specforge::SpecForgeMetadataFinalizerOptions options =
        OptionsFor(root);
    options.utc_now = [] { return FixedUtcTime(); };
    std::string error;
    const bool finalized =
        specforge::FinalizeSpecForgeMetadata(options, &error);
    Require(
        finalized,
        "finalizer should write schema 6 metadata: " + error);
    Require(
        std::filesystem::exists(metadata_path),
        "successful finalization should replace the metadata target");

    const specforge::SpecForgeMetadataReadResult result =
        specforge::ReadSpecForgeMetadata(
            metadata_path,
            WorkingTreeIdentity());
    Require(
        !result.startup_error &&
            result.build_metadata.status ==
                specforge::BuildMetadataStatus::Available &&
            result.build_metadata.metadata,
        "finalizer output should be readable as available schema 6 metadata");
    const specforge::BuildMetadata& metadata =
        *result.build_metadata.metadata;
    Require(
        metadata.cfitsio_version == "4.6.4" &&
            metadata.finalized_artifact &&
            metadata.finalized_artifact->completed_at_utc ==
                "2026-08-05T09:21:32Z" &&
            metadata.finalized_artifact->artifact.file == "Spectiary.exe" &&
            metadata.finalized_artifact->artifact.sha256 ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "finalizer should record the fixed UTC time and SHA-256 of abc");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestFailedFinalizationRemovesStaleTargetAndRecovers()
{
    const std::filesystem::path root = TestRoot("pre-replace-failure");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path executable_path = root / "Spectiary.exe";
    const std::filesystem::path metadata_path =
        root / "spectiary_metadata.json";
    WriteTextFile(executable_path, "old executable bytes");

    specforge::SpecForgeMetadataFinalizerOptions options =
        OptionsFor(root);
    options.utc_now = [] { return FixedUtcTime(); };
    Require(
        specforge::FinalizeSpecForgeMetadata(options),
        "fixture finalization should create the old schema 6 sidecar");
    const std::string original_metadata = ReadTextFile(metadata_path);

    WriteTextFile(executable_path, "new executable bytes");
    std::filesystem::path temporary_path;
    bool temporary_existed_at_checkpoint = false;
    options.before_replace =
        [&](const std::filesystem::path& observed_temporary_path,
            const std::filesystem::path& observed_target_path) {
            temporary_path = observed_temporary_path;
            temporary_existed_at_checkpoint =
                std::filesystem::exists(observed_temporary_path) &&
                observed_target_path == metadata_path;
            throw std::runtime_error("injected finalizer failure");
        };

    std::string error;
    const bool finalized =
        specforge::FinalizeSpecForgeMetadata(options, &error);
    Require(!finalized, "pre-replace failure should fail finalization");
    Require(
        temporary_existed_at_checkpoint,
        "pre-replace failure seam should observe a closed temporary file");
    Require(
        !temporary_path.empty() &&
            !std::filesystem::exists(temporary_path),
        "pre-replace failure should remove the temporary metadata file");
    Require(
        !std::filesystem::exists(metadata_path),
        "pre-replace failure should remove the stale metadata target");
    Require(
        error.find("pre-replace checkpoint failed") != std::string::npos,
        "pre-replace failure should report the atomic write diagnostic");

    options.before_replace = {};
    options.utc_now = [] {
        return FixedUtcTime() + std::chrono::seconds{1};
    };
    Require(
        specforge::FinalizeSpecForgeMetadata(options),
        "a later finalization should recover after the stale target is removed");
    Require(
        std::filesystem::exists(metadata_path) &&
            ReadTextFile(metadata_path) != original_metadata,
        "recovery should publish fresh metadata for the new executable");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestReplacementFailureRemovesStaleTargetAndRecovers()
{
    const std::filesystem::path root = TestRoot("replacement-failure");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path executable_path = root / "Spectiary.exe";
    const std::filesystem::path metadata_path =
        root / "spectiary_metadata.json";
    WriteTextFile(executable_path, "old executable bytes");

    specforge::SpecForgeMetadataFinalizerOptions options =
        OptionsFor(root);
    options.utc_now = [] { return FixedUtcTime(); };
    Require(
        specforge::FinalizeSpecForgeMetadata(options),
        "replacement fixture should create the old schema 6 sidecar");

    Require(
        std::filesystem::remove(metadata_path, cleanup_error) &&
            !cleanup_error,
        "replacement fixture should remove the old sidecar");
    Require(
        std::filesystem::create_directory(metadata_path, cleanup_error) &&
            !cleanup_error,
        "replacement fixture should create a blocking metadata directory");
    WriteTextFile(executable_path, "new executable bytes");

    std::string error;
    const bool finalized =
        specforge::FinalizeSpecForgeMetadata(options, &error);
    Require(!finalized, "an atomic replacement failure should fail finalization");
    Require(
        error.find("could not replace") != std::string::npos,
        "replacement failure should report the atomic replacement diagnostic");
    Require(
        !std::filesystem::exists(metadata_path),
        "replacement failure should remove the stale metadata target");
    Require(
        !HasTemporarySibling(root),
        "replacement failure should remove the temporary metadata file");

    options.utc_now = [] {
        return FixedUtcTime() + std::chrono::seconds{1};
    };
    Require(
        specforge::FinalizeSpecForgeMetadata(options),
        "a later finalization should recover after replacement failure");
    Require(
        std::filesystem::exists(metadata_path),
        "replacement recovery should publish a metadata file");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestHashFailureDoesNotCreateMetadataOrTemporaryFile()
{
    const std::filesystem::path root = TestRoot("hash-failure");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path metadata_path =
        root / "spectiary_metadata.json";
    WriteTextFile(metadata_path, "old metadata\n");

    specforge::SpecForgeMetadataFinalizerOptions options =
        OptionsFor(root);
    options.utc_now = [] { return FixedUtcTime(); };
    std::string error;
    const bool finalized =
        specforge::FinalizeSpecForgeMetadata(options, &error);
    Require(!finalized, "missing executable should fail finalization");
    Require(
        error.find("could not open file for hashing") != std::string::npos,
        "missing executable should report the hash input failure");
    Require(
        !std::filesystem::exists(metadata_path),
        "hash failure should remove the stale metadata target");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestRejectsNonCanonicalExecutableFilenameBeforeSideEffects()
{
    const std::filesystem::path root = TestRoot("wrong-executable-name");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path executable_path = root / "Other.exe";
    const std::filesystem::path metadata_path =
        root / "spectiary_metadata.json";
    const std::string original_metadata = "old metadata\n";
    WriteTextFile(executable_path, "other executable bytes");
    WriteTextFile(metadata_path, original_metadata);
    const std::string original_executable = ReadTextFile(executable_path);

    specforge::SpecForgeMetadataFinalizerOptions options = OptionsFor(root);
    options.executable_path = executable_path;
    bool clock_called = false;
    bool checkpoint_called = false;
    options.utc_now = [&] {
        clock_called = true;
        return FixedUtcTime();
    };
    options.before_replace =
        [&](const std::filesystem::path&, const std::filesystem::path&) {
            checkpoint_called = true;
        };

    std::string error;
    const bool finalized =
        specforge::FinalizeSpecForgeMetadata(options, &error);
    Require(
        !finalized,
        "a non-canonical executable filename should fail finalization");
    Require(
        error.find("Spectiary.exe") != std::string::npos,
        "wrong executable filename should identify the canonical artifact name");
    Require(
        !clock_called && !checkpoint_called,
        "wrong executable filename should fail before clock or atomic-write hooks");
    Require(
        ReadTextFile(executable_path) == original_executable,
        "wrong executable filename failure should preserve the executable");
    Require(
        ReadTextFile(metadata_path) == original_metadata,
        "wrong executable filename failure should preserve metadata");
    Require(
        !HasTemporarySibling(root),
        "wrong executable filename failure should not leave a temporary file");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestRejectsArbitraryMetadataPathsBeforeSideEffects()
{
    const std::filesystem::path root = TestRoot("arbitrary-metadata-path");
    std::error_code cleanup_error;
    const std::string original_metadata = "unrelated file contents\n";

    for (const std::filesystem::path& metadata_path : {
             root / "unrelated.txt",
             root / "elsewhere" / "spectiary_metadata.json",
         }) {
        std::filesystem::remove_all(root, cleanup_error);
        WriteTextFile(metadata_path, original_metadata);

        specforge::SpecForgeMetadataFinalizerOptions options =
            OptionsFor(root);
        options.executable_path = root / "app" / "Spectiary.exe";
        options.metadata_path = metadata_path;
        bool clock_called = false;
        options.utc_now = [&] {
            clock_called = true;
            return FixedUtcTime();
        };

        std::string error;
        const bool finalized =
            specforge::FinalizeSpecForgeMetadata(options, &error);
        Require(
            !finalized,
            "an arbitrary metadata path should fail validation");
        Require(
            error.find("spectiary_metadata.json") != std::string::npos,
            "arbitrary metadata path failure should identify the canonical sidecar");
        Require(
            !clock_called,
            "arbitrary metadata path should fail before finalization starts");
        Require(
            ReadTextFile(metadata_path) == original_metadata,
            "arbitrary metadata path validation must preserve the caller's file");
        Require(
            !HasTemporarySibling(metadata_path.parent_path()),
            "arbitrary metadata path validation should not create a temporary file");
    }

    std::filesystem::remove_all(root, cleanup_error);
}

void TestRejectsEquivalentExecutableAndMetadataPathsBeforeSideEffects()
{
    const std::filesystem::path root = TestRoot("same-path");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path executable_path = root / "Spectiary.exe";
    const std::filesystem::path original_metadata_path =
        root / "spectiary_metadata.json";

    for (const std::filesystem::path& metadata_path : {
             executable_path,
             root / "." / "Spectiary.exe",
         }) {
        std::filesystem::remove_all(root, cleanup_error);
        WriteTextFile(executable_path, "final executable bytes");
        WriteTextFile(original_metadata_path, "old metadata\n");
        const std::string original_executable = ReadTextFile(executable_path);
        const std::string original_metadata = ReadTextFile(
            original_metadata_path);

        specforge::SpecForgeMetadataFinalizerOptions options =
            OptionsFor(root);
        options.metadata_path = metadata_path;
        bool clock_called = false;
        bool checkpoint_called = false;
        options.utc_now = [&] {
            clock_called = true;
            return FixedUtcTime();
        };
        options.before_replace =
            [&](const std::filesystem::path&, const std::filesystem::path&) {
                checkpoint_called = true;
            };

        std::string error;
        const bool finalized =
            specforge::FinalizeSpecForgeMetadata(options, &error);
        Require(
            !finalized,
            "equivalent executable and metadata paths should fail finalization");
        Require(
            error.find("same file") != std::string::npos,
            "same-path failure should identify the aliased files");
        Require(
            !clock_called && !checkpoint_called,
            "same-path failure should happen before clock or atomic-write hooks");
        Require(
            ReadTextFile(executable_path) == original_executable,
            "same-path failure should preserve the executable bytes");
        Require(
            ReadTextFile(original_metadata_path) == original_metadata,
            "same-path failure should preserve the original metadata file");
        Require(
            !HasTemporarySibling(root),
            "same-path failure should not leave a temporary metadata file");
    }

    std::filesystem::remove_all(root, cleanup_error);
}

void TestRejectsSchema6InvalidBuildValuesWithoutPublishingMetadata()
{
    const std::filesystem::path root = TestRoot("schema6-validation-failure");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path metadata_path =
        root / "spectiary_metadata.json";
    const std::string original_metadata = "old schema 6 metadata\n";

    const std::vector<std::function<void(specforge::BuildMetadata&)>> invalid_cases = {
        [](specforge::BuildMetadata& build) {
            build.compiler_version = "19.44-preview";
        },
        [](specforge::BuildMetadata& build) {
            build.cmake_version = "4.x";
        },
        [](specforge::BuildMetadata& build) {
            build.generator = "Ninja\nVisual Studio";
        },
        [](specforge::BuildMetadata& build) {
            build.windows_sdk_version = "10.0.26100.preview";
        },
        [](specforge::BuildMetadata& build) {
            build.cfitsio_version.clear();
        },
        [](specforge::BuildMetadata& build) {
            build.cfitsio_version = "4.6.x";
        },
    };

    for (const auto& make_invalid : invalid_cases) {
        std::filesystem::remove_all(root, cleanup_error);
        WriteTextFile(root / "Spectiary.exe", "abc");
        WriteTextFile(metadata_path, original_metadata);

        specforge::SpecForgeMetadataFinalizerOptions options =
            OptionsFor(root);
        options.utc_now = [] { return FixedUtcTime(); };
        make_invalid(options.configured_build_metadata);

        std::string error;
        const bool finalized =
            specforge::FinalizeSpecForgeMetadata(options, &error);
        Require(
            !finalized,
            "schema 6-invalid build values should fail finalization");
        Require(
            !error.empty(),
            "schema 6 validation failure should report a diagnostic");
        Require(
            !std::filesystem::exists(metadata_path),
            "schema 6 validation failure should remove stale metadata");
        Require(
            !HasTemporarySibling(root),
            "schema 6 validation failure should not leave a temporary file");
    }

    std::filesystem::remove_all(root, cleanup_error);
}

}  // namespace

int main()
{
    TestFinalizerWritesSchema6AndHashesFinalExecutable();
    TestFailedFinalizationRemovesStaleTargetAndRecovers();
    TestReplacementFailureRemovesStaleTargetAndRecovers();
    TestHashFailureDoesNotCreateMetadataOrTemporaryFile();
    TestRejectsNonCanonicalExecutableFilenameBeforeSideEffects();
    TestRejectsArbitraryMetadataPathsBeforeSideEffects();
    TestRejectsEquivalentExecutableAndMetadataPathsBeforeSideEffects();
    TestRejectsSchema6InvalidBuildValuesWithoutPublishingMetadata();
    std::cout << "SpecForge metadata finalizer tests passed\n";
    return 0;
}
