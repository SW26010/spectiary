#include "app/specforge_metadata_finalizer.h"

#include "app/local_user_state_json.h"
#include "app/specforge_metadata_validation.h"
#include "domain/stable_sha256.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

constexpr int kSchema5Version = 5;
constexpr std::string_view kArtifactFileName = "SpecForge.exe";
constexpr std::string_view kMetadataFileName = "specforge_metadata.json";
constexpr std::size_t kHashBufferSize = 64U * 1024U;

void SetError(std::string* error_message, std::string message)
{
    if (error_message != nullptr) {
        *error_message = std::move(message);
    }
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

bool PathsEqualForComparison(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
#ifdef _WIN32
    const std::wstring left_native = left.native();
    const std::wstring right_native = right.native();
    if (left_native.size() != right_native.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left_native.size(); ++index) {
        if (std::towlower(left_native[index]) !=
            std::towlower(right_native[index])) {
            return false;
        }
    }
    return true;
#else
    return left == right;
#endif
}

bool IsCanonicalExecutableFilename(const std::filesystem::path& path)
{
#ifdef _WIN32
    const std::wstring actual = path.filename().native();
    constexpr std::wstring_view expected = L"SpecForge.exe";
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (std::towlower(actual[index]) !=
            std::towlower(expected[index])) {
            return false;
        }
    }
    return true;
#else
    return path.filename() == std::filesystem::path("SpecForge.exe");
#endif
}

bool IsCanonicalMetadataFilename(const std::filesystem::path& path)
{
    return path.filename() ==
        std::filesystem::path(std::string(kMetadataFileName));
}

std::filesystem::path MetadataPathForExecutable(
    const std::filesystem::path& executable_path)
{
    return (
        executable_path.parent_path() /
        std::filesystem::path(std::string(kMetadataFileName)))
        .lexically_normal();
}

std::optional<std::filesystem::path> NormalizePathForComparison(
    const std::filesystem::path& path,
    std::error_code& error)
{
    error.clear();
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(path, error);
    if (!error) {
        return canonical;
    }
    error.clear();
    const std::filesystem::path absolute =
        std::filesystem::absolute(path, error);
    if (error) {
        return std::nullopt;
    }
    return absolute.lexically_normal();
}

bool ValidatePaths(
    const SpecForgeMetadataFinalizerOptions& options,
    std::string& error)
{
    if (options.executable_path.empty()) {
        error = "the executable path is empty";
        return false;
    }
    if (options.metadata_path.empty()) {
        error = "the metadata path is empty";
        return false;
    }
    if (!IsCanonicalExecutableFilename(options.executable_path)) {
        error = "the executable filename must be SpecForge.exe";
        return false;
    }
    std::error_code equivalent_error;
    if (std::filesystem::equivalent(
            options.executable_path,
            options.metadata_path,
            equivalent_error)) {
        error =
            "the executable and metadata paths must not refer to the same file";
        return false;
    }

    std::error_code normalization_error;
    const std::optional<std::filesystem::path> executable_path =
        NormalizePathForComparison(
            options.executable_path,
            normalization_error);
    const std::optional<std::filesystem::path> metadata_path =
        NormalizePathForComparison(
            options.metadata_path,
            normalization_error);
    if (!executable_path || !metadata_path) {
        error =
            "could not compare executable and metadata paths safely";
        return false;
    }
    if (PathsEqualForComparison(*executable_path, *metadata_path)) {
        error =
            "the executable and metadata paths must not refer to the same file";
        return false;
    }
    if (!IsCanonicalMetadataFilename(options.metadata_path)) {
        error =
            "the metadata filename must be specforge_metadata.json";
        return false;
    }

    const std::filesystem::path expected_metadata_path =
        MetadataPathForExecutable(options.executable_path);
    const std::optional<std::filesystem::path> normalized_expected_metadata_path =
        NormalizePathForComparison(
            expected_metadata_path,
            normalization_error);
    if (!normalized_expected_metadata_path) {
        error = "could not resolve the executable-adjacent metadata path";
        return false;
    }
    if (!PathsEqualForComparison(
            *metadata_path,
            *normalized_expected_metadata_path)) {
        error =
            "the metadata path must be the executable-adjacent "
            "specforge_metadata.json";
        return false;
    }
    return true;
}

bool ComputeExecutableSha256(
    const std::filesystem::path& executable_path,
    std::string& digest,
    std::string& error)
{
    std::ifstream stream(executable_path, std::ios::binary);
    if (!stream.good()) {
        error =
            "could not open final executable for hashing: " +
            PathToUtf8(executable_path);
        return false;
    }

    StableSha256 sha256;
    std::array<char, kHashBufferSize> buffer = {};
    for (;;) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read_count = stream.gcount();
        if (read_count > 0) {
            sha256.Append(std::string_view(
                buffer.data(),
                static_cast<std::size_t>(read_count)));
        }
        if (stream.eof()) {
            break;
        }
        if (stream.fail()) {
            error =
                "could not read final executable completely: " +
                PathToUtf8(executable_path);
            return false;
        }
    }

    digest = sha256.FinishHex();
    return true;
}

bool FormatUtcTimestamp(
    std::chrono::system_clock::time_point time_point,
    std::string& timestamp,
    std::string& error)
{
    const std::time_t time =
        std::chrono::system_clock::to_time_t(time_point);
    std::tm utc = {};
#ifdef _WIN32
    if (gmtime_s(&utc, &time) != 0) {
#else
    if (gmtime_r(&time, &utc) == nullptr) {
#endif
        error = "could not convert finalization time to UTC";
        return false;
    }

    const int year = utc.tm_year + 1900;
    if (year < 0 || year > 9999 || utc.tm_mon < 0 || utc.tm_mon > 11 ||
        utc.tm_mday < 1 || utc.tm_mday > 31 || utc.tm_hour < 0 ||
        utc.tm_hour > 23 || utc.tm_min < 0 || utc.tm_min > 59 ||
        utc.tm_sec < 0 || utc.tm_sec > 59) {
        error = "finalization time is outside the supported UTC range";
        return false;
    }

    char formatted[21] = {};
    const int written = std::snprintf(
        formatted,
        sizeof(formatted),
        "%04d-%02d-%02dT%02d:%02d:%02dZ",
        year,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec);
    if (written != 20) {
        error = "could not format finalization time as UTC ISO 8601";
        return false;
    }
    timestamp.assign(formatted, static_cast<std::size_t>(written));
    return true;
}

void WriteOptionalString(
    std::ostream& stream,
    const std::optional<std::string>& value)
{
    if (value) {
        WriteJsonString(stream, *value);
    } else {
        stream << "null";
    }
}

bool WriteSchema5Metadata(
    std::ostream& stream,
    const BuildIdentity& identity,
    const BuildMetadata& build,
    std::string&)
{
    stream << "{\n"
           << "  \"schema_version\": " << kSchema5Version << ",\n"
           << "  \"product\": {\n"
           << "    \"name\": ";
    WriteJsonString(stream, identity.product_name);
    stream << ",\n"
           << "    \"version\": ";
    WriteJsonString(stream, identity.specforge_version);
    stream << "\n  },\n"
           << "  \"build\": {\n"
           << "    \"source_mode\": ";
    WriteJsonString(stream, identity.source_mode);
    stream << ",\n"
           << "    \"source_revision\": ";
    if (identity.source_mode == "working_tree") {
        stream << "null";
    } else {
        WriteJsonString(stream, identity.source_revision);
    }
    stream << ",\n"
           << "    \"configuration\": ";
    WriteJsonString(stream, identity.configuration);
    stream << ",\n"
           << "    \"compiler_id\": ";
    WriteJsonString(stream, build.compiler_id);
    stream << ",\n"
           << "    \"compiler_version\": ";
    WriteJsonString(stream, build.compiler_version);
    stream << ",\n"
           << "    \"cmake_version\": ";
    WriteJsonString(stream, build.cmake_version);
    stream << ",\n"
           << "    \"generator\": ";
    WriteJsonString(stream, build.generator);
    stream << ",\n"
           << "    \"target_architecture\": ";
    WriteJsonString(stream, identity.target_architecture);
    stream << ",\n"
           << "    \"windows_sdk_version\": ";
    WriteOptionalString(stream, build.windows_sdk_version);
    stream << ",\n"
           << "    \"dear_imgui\": ";
    WriteJsonString(stream, build.dear_imgui_version);
    stream << ",\n"
           << "    \"implot\": ";
    WriteJsonString(stream, build.implot_version);
    stream << ",\n"
           << "    \"zlib\": ";
    WriteJsonString(stream, build.zlib_version);
    stream << ",\n"
           << "    \"completed_at_utc\": ";
    WriteJsonString(stream, *build.completed_at_utc);
    stream << "\n  },\n"
           << "  \"artifact\": {\n"
           << "    \"file\": ";
    WriteJsonString(stream, kArtifactFileName);
    stream << ",\n"
           << "    \"sha256\": ";
    WriteJsonString(stream, build.artifact->sha256);
    stream << "\n  }\n}\n";
    return true;
}

void RemoveStaleMetadataTarget(
    const std::filesystem::path& metadata_path,
    std::string& failure_message)
{
    std::error_code remove_error;
    std::filesystem::remove(metadata_path, remove_error);
    if (remove_error) {
        failure_message +=
            "; could not remove stale SpecForge metadata target " +
            PathToUtf8(metadata_path) + ": " +
            remove_error.message();
    }
}

}  // namespace

bool FinalizeSpecForgeMetadata(
    const SpecForgeMetadataFinalizerOptions& options,
    std::string* error_message)
{
    if (error_message != nullptr) {
        error_message->clear();
    }

    std::string error;
    if (!ValidatePaths(options, error)) {
        SetError(error_message, "cannot finalize SpecForge metadata: " + error);
        return false;
    }

    const std::filesystem::path metadata_path =
        MetadataPathForExecutable(options.executable_path);
    const auto fail_after_path_validation =
        [&](std::string failure_message) {
            RemoveStaleMetadataTarget(
                metadata_path,
                failure_message);
            SetError(error_message, std::move(failure_message));
            return false;
        };

    std::string executable_sha256;
    try {
        if (!ComputeExecutableSha256(
                options.executable_path,
                executable_sha256,
                error)) {
            return fail_after_path_validation(
                "cannot finalize SpecForge metadata: " + error);
        }
    } catch (const std::exception& exception) {
        return fail_after_path_validation(
            "cannot finalize SpecForge metadata: executable hashing failed: " +
                std::string(exception.what()));
    } catch (...) {
        return fail_after_path_validation(
            "cannot finalize SpecForge metadata: executable hashing failed");
    }

    std::chrono::system_clock::time_point now;
    try {
        now = options.utc_now
            ? options.utc_now()
            : std::chrono::system_clock::now();
    } catch (const std::exception& exception) {
        return fail_after_path_validation(
            "cannot finalize SpecForge metadata: UTC clock failed: " +
                std::string(exception.what()));
    } catch (...) {
        return fail_after_path_validation(
            "cannot finalize SpecForge metadata: UTC clock failed");
    }

    std::string completed_at_utc;
    if (!FormatUtcTimestamp(now, completed_at_utc, error)) {
        return fail_after_path_validation(
            "cannot finalize SpecForge metadata: " + error);
    }

    BuildMetadata finalized_build = options.configured_build_metadata;
    finalized_build.completed_at_utc = completed_at_utc;
    finalized_build.artifact = BuildArtifactMetadata{
        .file = std::string(kArtifactFileName),
        .sha256 = executable_sha256,
    };
    if (!metadata_validation::ValidateSchema5BuildMetadata(
            options.build_identity,
            finalized_build,
            &error)) {
        return fail_after_path_validation(
            "cannot finalize SpecForge metadata: " + error);
    }

    AtomicFileWriteOptions write_options;
    write_options.target_description = "SpecForge metadata";
    write_options.replace_retry_policy = options.replace_retry_policy;
    write_options.before_replace = options.before_replace;
    try {
        std::string atomic_write_error;
        if (!WriteFileAtomically(
                metadata_path,
                write_options,
                [&](std::ostream& stream, std::string& writer_error) {
                    return WriteSchema5Metadata(
                        stream,
                        options.build_identity,
                        finalized_build,
                        writer_error);
                },
                &atomic_write_error)) {
            return fail_after_path_validation(
                atomic_write_error.empty()
                    ? "cannot finalize SpecForge metadata: atomic write failed"
                    : std::move(atomic_write_error));
        }
    } catch (const std::exception& exception) {
        return fail_after_path_validation(
            "cannot finalize SpecForge metadata: atomic write failed: " +
                std::string(exception.what()));
    } catch (...) {
        return fail_after_path_validation(
            "cannot finalize SpecForge metadata: atomic write failed");
    }
    return true;
}

}  // namespace specforge
