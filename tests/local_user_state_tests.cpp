#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/local_user_state_paths.h"
#include "app/runtime_paths.h"
#include "platform/atomic_file.h"
#include "platform/atomic_file_internal.h"
#include "ui/panel_visibility_state_cache_io.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>
#include <thread>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono_literals;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void RequireCompleteLocalUserStatePathMapping(
    const specforge::RuntimePaths& paths,
    const std::filesystem::path& root)
{
    using namespace specforge::local_user_state_paths;
    Require(
        paths.profile_log_directory ==
            root / kProfileLogDirectory,
        "profile log directory should use the canonical name");
    Require(
        paths.frame_capture_directory ==
            root / kFrameCaptureDirectory,
        "frame capture directory should use the canonical name");
    Require(
        paths.imgui_ini_path == root / kImGuiIni,
        "ImGui settings should use the canonical name");
    Require(
        paths.ui_language_settings_path ==
            root / kUiLanguageSettings,
        "language settings should use the canonical name");
    Require(
        paths.appearance_settings_path ==
            root / kAppearanceSettings,
        "appearance settings should use the canonical name");
    Require(
        paths.ui_scale_settings_path ==
            root / kUiScaleSettings,
        "UI scale settings should use the canonical name");
    Require(
        paths.input_settings_path ==
            root / kInputSettings,
        "input settings should use the canonical name");
    Require(
        paths.external_source_settings_path ==
            root / kExternalSourceSettings,
        "external source settings should use the canonical name");
    Require(
        paths.profile_settings_path ==
            root / kProfileSettings,
        "profile settings should use the canonical name");
    Require(
        paths.panel_visibility_state_path ==
            root / kPanelVisibilityState,
        "panel visibility should use the canonical name");
    Require(
        paths.legacy_spectrum_view_state_path ==
            root / kLegacySpectrumViewState,
        "spectrum view state should use the canonical name");
    Require(
        paths.source_session_state_path ==
            root / kSourceSessionState,
        "source session state should use the canonical name");
    Require(
        paths.sample_navigation_state_path ==
            root / kSampleNavigationState,
        "navigation state should use the canonical name");
    Require(
        paths.sample_labeling_state_path ==
            root / kSampleLabelingState,
        "labeling state should use the canonical name");
    Require(
        paths.sample_workflow_state_path ==
            root / kSampleWorkflowState,
        "workflow state should use the canonical name");
    Require(
        paths.spectral_line_user_state_path ==
            root / kSpectralLineUserState,
        "spectral-line state should use the canonical name");
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    Require(stream.good(), "could not open text file");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool HasTemporarySibling(const std::filesystem::path& target_path)
{
    const std::filesystem::path parent = target_path.parent_path();
    if (parent.empty() || !std::filesystem::exists(parent)) {
        return false;
    }
    const std::string temporary_prefix = target_path.filename().string() + ".tmp.";
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(parent)) {
        if (entry.path().filename().string().starts_with(temporary_prefix)) {
            return true;
        }
    }
    return false;
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream stream(path);
    Require(stream.good(), "could not open text file for writing");
    stream << text;
    Require(stream.good(), "could not write text file");
}

void TestDefaultLocalUserStatePathUsesSpecForgeRoot()
{
    const std::filesystem::path path = specforge::DefaultLocalUserStatePath("nested/state.json", specforge::DefaultRuntimePaths());
    const specforge::RuntimePaths runtime_paths = specforge::DefaultRuntimePaths();
    Require(path.filename() == "state.json", "default local state path should keep the requested filename");
    Require(path.parent_path().filename() == "nested", "default local state path should keep relative subdirectories");
    Require(
        path == runtime_paths.local_user_state_root / "nested" / "state.json",
        "default local state path should live under the selected storage root");
    if (runtime_paths.storage_profile == specforge::StorageProfile::Portable) {
        Require(
            path.parent_path().parent_path().filename() == "Data",
            "portable local state path should live under the package Data root");
    } else {
        Require(
            path.parent_path().parent_path().filename() == "SpecForge",
            "LocalAppData state path should live under the SpecForge root");
    }
}

void TestRuntimePathPoliciesKeepPortableAndInstalledRootsDistinct()
{
    const std::filesystem::path package_root =
        std::filesystem::temp_directory_path() / "specforge_runtime_path_package";
    const std::filesystem::path installed_root =
        std::filesystem::temp_directory_path() / "specforge_runtime_path_installed";

    specforge::RuntimePathInputs inputs;
    inputs.executable_path = package_root / "arbitrarily-renamed-program.exe";
    inputs.local_app_data_user_state_root = installed_root;

    const specforge::RuntimePaths portable_paths =
        specforge::RuntimePathsForDeployment(
            {
                .distribution = specforge::Distribution::Portable,
                .storage_profile = specforge::StorageProfile::Portable,
            },
            inputs);
    Require(portable_paths.package_root == package_root, "portable package root should be the executable directory");
    Require(
        portable_paths.public_spectral_line_catalog_path ==
            package_root / "config" / "spectral_lines.public.tsv",
        "packaged spectral-line catalog should resolve from the package root");
    Require(portable_paths.local_user_state_root == package_root / "Data", "portable state should live under Data");
    RequireCompleteLocalUserStatePathMapping(
        portable_paths,
        package_root / "Data");

    const specforge::RuntimePaths installed_paths =
        specforge::RuntimePathsForDeployment(
            {
                .distribution = specforge::Distribution::Installer,
                .storage_profile =
                    specforge::StorageProfile::LocalAppData,
            },
            inputs);
    Require(installed_paths.package_root == package_root, "installed package root should still be the executable directory");
    Require(
        installed_paths.public_spectral_line_catalog_path ==
            package_root / "config" / "spectral_lines.public.tsv",
        "installed profile should use the same package resource root");
    Require(installed_paths.local_user_state_root == installed_root, "installed state should use local app data root");
    RequireCompleteLocalUserStatePathMapping(
        installed_paths,
        installed_root);
    Require(
        portable_paths.local_user_state_root != installed_paths.local_user_state_root,
        "portable and installed state roots should stay distinct");

    const specforge::RuntimePaths winget_paths =
        specforge::RuntimePathsForDeployment(
            {
                .distribution = specforge::Distribution::WinGet,
                .storage_profile =
                    specforge::StorageProfile::LocalAppData,
            },
            inputs);
    Require(
        winget_paths.distribution == specforge::Distribution::WinGet,
        "WinGet identity should be retained for About");
    Require(
        winget_paths.local_user_state_root ==
            installed_paths.local_user_state_root,
        "WinGet distribution should not override LocalAppData storage");

    for (const auto& invalid_path : {std::filesystem::path("relative.exe"), package_root / ""}) {
        inputs.executable_path = invalid_path;
        bool rejected = false;
        try {
            (void)specforge::RuntimePathsForDeployment({}, inputs);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        Require(rejected, "invalid executable locations must not invent a package root");
    }
}

void TestAtomicWriteCreatesParentAndReplacesExistingFile()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "specforge_atomic_file_tests";
    const std::filesystem::path path = root / "nested" / "state.txt";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    specforge::AtomicFileWriteOptions options;
    options.target_description = "test state file";
    std::string error;
    Require(
        specforge::WriteFileAtomically(
            path,
            options,
            [](std::ostream& stream, std::string&) {
                stream << "old";
                return true;
            },
            &error),
        error.empty() ? "initial atomic write failed" : error);
    Require(ReadTextFile(path) == "old", "initial atomic write should create the target file");

    Require(
        specforge::WriteFileAtomically(
            path,
            options,
            [](std::ostream& stream, std::string&) {
                stream << "new";
                return true;
            },
            &error),
        error.empty() ? "replacement atomic write failed" : error);

    Require(ReadTextFile(path) == "new", "atomic write should replace the existing target");
    Require(!HasTemporarySibling(path), "successful atomic write should not leave a temporary sibling");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestAtomicWriteCleansTemporaryAndPreservesExistingFileOnWriterFailure()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "specforge_atomic_file_failure_tests";
    const std::filesystem::path path = root / "state.txt";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    specforge::AtomicFileWriteOptions options;
    options.target_description = "test state file";
    std::string error;
    Require(
        specforge::WriteFileAtomically(
            path,
            options,
            [](std::ostream& stream, std::string&) {
                stream << "stable";
                return true;
            },
            &error),
        error.empty() ? "initial atomic write failed" : error);

    Require(
        !specforge::WriteFileAtomically(
            path,
            options,
            [](std::ostream& stream, std::string& writer_error) {
                stream << "partial";
                writer_error = "planned writer failure";
                return false;
            },
            &error),
        "writer failure should fail the atomic write");

    Require(error == "planned writer failure", "writer error should be surfaced");
    Require(ReadTextFile(path) == "stable", "failed atomic write should preserve the existing target");
    Require(!HasTemporarySibling(path), "failed atomic write should clean the temporary sibling");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestAtomicReplaceRetriesTransientSharingViolation()
{
    const std::filesystem::path temporary = "temporary";
    const std::filesystem::path target = "target";
    std::size_t operation_count = 0;
    std::vector<std::chrono::milliseconds> waits;
    specforge::AtomicFileReplaceRetryPolicy retry_policy;
    retry_policy.maximum_attempts = 5;
    retry_policy.initial_retry_delay = 5ms;
    retry_policy.maximum_retry_delay = 40ms;

    std::string error;
    Require(
        specforge::ReplaceFileAtomicallyWithOperation(
            temporary,
            target,
            retry_policy,
            [&](const std::filesystem::path& observed_temporary,
                const std::filesystem::path& observed_target) {
                Require(
                    observed_temporary == temporary &&
                        observed_target == target,
                    "atomic replacement should preserve operation paths");
                ++operation_count;
                if (operation_count == 1) {
                    return std::error_code(
                        ERROR_SHARING_VIOLATION,
                        std::system_category());
                }
                return std::error_code{};
            },
            [&](std::chrono::milliseconds delay) {
                waits.push_back(delay);
            },
            &error,
            "sharing retry fixture"),
        error.empty()
            ? "atomic replacement should retry a transient sharing violation"
            : error);
    Require(
        operation_count == 2,
        "transient sharing violation should use one retry");
    Require(
        waits == std::vector<std::chrono::milliseconds>{5ms},
        "first atomic replacement retry should use configured delay");
}

void TestAtomicReplaceDoesNotRetryByDefault()
{
    std::size_t operation_count = 0;
    std::size_t wait_count = 0;
    std::string error;
    Require(
        !specforge::ReplaceFileAtomicallyWithOperation(
            "temporary",
            "target",
            {},
            [&](const std::filesystem::path&,
                const std::filesystem::path&) {
                ++operation_count;
                return std::error_code(
                    ERROR_SHARING_VIOLATION,
                    std::system_category());
            },
            [&](std::chrono::milliseconds) {
                ++wait_count;
            },
            &error,
            "single-attempt fixture"),
        "default atomic replacement should preserve single-attempt behavior");
    Require(
        operation_count == 1 && wait_count == 0,
        "default atomic replacement should not retry or wait");
}

void TestJsonSyntaxAndSchemaBoundaries()
{
    std::string error;
    for (const std::string_view malformed : {
             "", "[1,]", "{\"a\":}", "true false", "01", "-01",
             "\"\\uD800\"", "\"\\uDC00\"", "\"\\uD800\\u0041\"",
             "\"\\uZZZZ\"", "\"\\x20\"", "\"\n\"",
             "{\"a\":1,\"a\":2}", "{\"a\":1,\"\\u0061\":2}",
             "{\"a\":[{\"b\":0,\"b\":1}]}"}) {
        Require(!specforge::ParseJson(malformed, error) && !error.empty(),
                "malformed JSON and duplicate decoded keys must fail");
    }
    Require(!specforge::ParseJson(std::string("\"\xC0\xAF\""), error),
            "invalid UTF-8 must fail");
    const auto unicode = specforge::ParseJson(R"({"text":"\u4e2d\uD83D\uDE00\u0000"})", error);
    Require(unicode && error.empty() &&
            specforge::ReadJsonStringMember(*unicode, "text") == std::string("\xE4\xB8\xAD\xF0\x9F\x98\x80\0", 8),
            "Unicode escapes and embedded NUL must decode");
    Require(specforge::ParseJson(R"([{"a":1},{"a":2,"child":{"a":3}}])", error).has_value(),
            "keys in distinct objects must not conflict");

    for (const std::string_view number : {"1.5", "1e2", "1.0", "true", "\"1\"",
             "18446744073709551616", "-9223372036854775809"}) {
        const auto value = specforge::ParseJson("{\"n\":" + std::string(number) + "}", error);
        Require(value.has_value(), "ordinary JSON syntax must parse independently of integer schemas");
        Require(!specforge::ReadJsonIntMember(*value, "n") && !specforge::ReadJsonSizeMember(*value, "n"),
                "non-integer storage and overflow must not convert to schema integers");
    }
    const auto bounds = specforge::ParseJson(
        R"({"min":-2147483648,"max":2147483647,"over":2147483648,"under":-2147483649,"huge":18446744073709551615,"negative":-1})", error);
    Require(bounds && specforge::ReadJsonIntMember(*bounds, "min") == (std::numeric_limits<int>::min)() &&
            specforge::ReadJsonIntMember(*bounds, "max") == (std::numeric_limits<int>::max)() &&
            !specforge::ReadJsonIntMember(*bounds, "over") && !specforge::ReadJsonIntMember(*bounds, "under") &&
            !specforge::ReadJsonIntMember(*bounds, "huge") && !specforge::ReadJsonSizeMember(*bounds, "negative") &&
            !specforge::JsonIsInt64(bounds->at("huge")), "integer boundary checks must fail closed");
    const auto signed_bounds = specforge::ParseJson("[-9223372036854775808,9223372036854775807]", error);
    Require(signed_bounds && specforge::JsonIsInt64(signed_bounds->at(0)) &&
            specforge::JsonIsInt64(signed_bounds->at(1)), "signed int64 endpoints must remain valid");
    Require(specforge::ParseJson(unicode->dump(), error) == unicode,
            "library serialization must preserve semantic values");
}

void TestJsonResourceBoundsAndCancellation()
{
    std::string error;
    const std::string at_depth = std::string(specforge::kMaxJsonNestingDepth, '[') + "0" +
        std::string(specforge::kMaxJsonNestingDepth, ']');
    Require(specforge::ParseJson(at_depth, error).has_value(), "maximum supported depth must parse");
    Require(!specforge::ParseJson("[" + at_depth + "]", error), "excess depth must fail");
    const std::string too_large(specforge::kMaxJsonInputBytes + 1, ' ');
    Require(!specforge::ParseJson(too_large, error), "oversized input must fail before parsing");
    std::istringstream oversized_stream(too_large);
    std::string contents;
    Require(!specforge::ReadTextStreamCancelable(oversized_stream, contents) && contents.empty(),
            "stream reads must enforce the byte limit");
    std::string nodes = "[";
    for (std::size_t i = 0; i < specforge::kMaxJsonNodes; ++i) nodes += "0,";
    nodes.back() = ']';
    Require(!specforge::ParseJson(nodes, error) && error.find("node count") != std::string::npos,
            "shallow node abuse must be bounded");
    struct Canceled {};
    for (const std::string& input : {
             "\"" + std::string(20000, 'a') + "\"",
             std::string(20000, ' ') + "null",
             "1" + std::string(20000, '0')}) {
        int checkpoints = 0;
        bool canceled = false;
        try {
            (void)specforge::ParseJson(input, error, [&] {
                if (++checkpoints == 3) throw Canceled{};
            });
        } catch (const Canceled&) { canceled = true; }
        Require(canceled && checkpoints == 3, "long tokens must allow cancellation during parsing");
    }
    int reads = 0;
    bool canceled = false;
    std::istringstream stream(std::string(200000, ' '));
    try {
        (void)specforge::ReadTextStreamCancelable(stream, contents, [&] {
            if (++reads == 2) throw Canceled{};
        });
    } catch (const Canceled&) { canceled = true; }
    Require(canceled && contents.size() <= 65536, "reads must allow cancellation between bounded chunks");
}

void TestVersionedJsonCacheShellRoundTripsDocument()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "specforge_json_cache_tests";
    const std::filesystem::path path = root / "state.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    std::string error;
    const nlohmann::json body =
        nlohmann::json::object({
            {"items",
             nlohmann::json::array({
                 nlohmann::json::object({
                     {"value",
                      nlohmann::json(7)},
                     {"name",
                      nlohmann::json(
                          "alpha")},
                 }),
             })},
        });
    Require(
        specforge::WriteVersionedJsonCacheDocument(
            path,
            "specforge.test.cache",
            2,
            "test cache",
            body,
            &error),
        error.empty() ? "versioned cache write failed" : error);
    const std::string expected = ReadTextFile(path);
    auto expected_document = body;
    expected_document["format_kind"] = "specforge.test.cache";
    expected_document["schema_version"] = 2;
    Require(nlohmann::json::parse(expected) == expected_document,
            "structured output should round-trip semantically");
    Require(expected.find("\"format_kind\"") < expected.find("\"items\"") &&
            expected.find("\"items\"") < expected.find("\"schema_version\""),
            "object members should be sorted");

    Require(
        specforge::WriteVersionedJsonCacheDocument(
            path,
            "specforge.test.cache",
            2,
            "test cache",
            body,
            &error) &&
            ReadTextFile(path) == expected,
        "repeated structured writes should be byte-stable");

    const specforge::VersionedJsonCacheLoadResult loaded =
        specforge::LoadVersionedJsonCacheFile(path, "specforge.test.cache", {2}, "test cache");
    Require(loaded.warning.empty(), loaded.warning);
    Require(loaded.document.has_value(), "versioned cache should load");
    Require(loaded.document->schema_version == 2, "versioned cache should report the parsed schema");
    const nlohmann::json* items = specforge::JsonObjectMember(loaded.document->root, "items");
    Require(items != nullptr && items->type() == nlohmann::json::value_t::array, "versioned cache should expose body fields");
    Require((*items).size() == 1, "versioned cache should preserve array items");
    Require(
        specforge::ReadJsonStringMember((*items).front(), "name").value_or("") == "alpha",
        "versioned cache should parse item strings");
    Require(
        specforge::ReadJsonIntMember((*items).front(), "value").value_or(0) == 7,
        "versioned cache should parse item integers");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestStructuredJsonCacheRejectsInvalidBody()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge_json_document_invalid_tests";
    const std::filesystem::path path = root / "state.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    WriteTextFile(path, "stable");
    std::string error;
    Require(
        !specforge::WriteVersionedJsonCacheDocument(
            path,
            "specforge.test.cache",
            1,
            "test cache",
            nlohmann::json("invalid"),
            &error),
        "structured cache should reject a non-object body");
    Require(
        !error.empty() &&
            ReadTextFile(path) == "stable",
        "invalid body should preserve the existing target");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestJsonWriteLimitsPreserveDurableState()
{
    const auto path = std::filesystem::temp_directory_path() /
        "specforge_json_write_limits.json";
    WriteTextFile(path, "previous durable state");
    std::string error;
    nlohmann::json nested = 0;
    for (std::size_t i = 0; i < specforge::kMaxJsonNestingDepth; ++i) {
        nested = nlohmann::json::array({std::move(nested)});
    }
    Require(!specforge::WriteVersionedJsonCacheDocument(path, "test", 1, "test cache",
                nlohmann::json::object({{"nested", std::move(nested)}}), &error),
            "a write beyond the read depth must fail");
    Require(ReadTextFile(path) == "previous durable state", "failed write must preserve the old generation");
    Require(!specforge::WriteVersionedJsonCacheFile(path, "test", 1, "test cache",
                [](std::ostream& stream, std::string&) {
                    stream << ",\"duplicate\":1,\"duplicate\":2";
                    return true;
                }, &error), "streaming cache writers must also produce readable JSON");
    Require(ReadTextFile(path) == "previous durable state", "malformed streamed output must not replace state");
    Require(!specforge::WriteVersionedJsonCacheDocument(path, "test", 1, "test cache",
                nlohmann::json::object({{"text", std::string("\xFF")}}), &error),
            "invalid UTF-8 serialization must fail safely");
    Require(ReadTextFile(path) == "previous durable state", "serialization errors must preserve state");
    std::filesystem::remove(path);
}

void TestVersionedJsonCacheShellReportsCorruptCacheWarning()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "specforge_json_cache_corrupt_tests";
    const std::filesystem::path path = root / "state.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    WriteTextFile(path, "{ invalid json");

    const specforge::VersionedJsonCacheLoadResult loaded =
        specforge::LoadVersionedJsonCacheFile(path, "specforge.test.cache", {1}, "test cache");

    Require(!loaded.document.has_value(), "corrupt versioned cache should not load a document");
    Require(!loaded.warning.empty(), "corrupt versioned cache should report a warning");
    Require(
        loaded.issue_kind ==
                specforge::VersionedJsonCacheLoadIssueKind::
                    InvalidDocument &&
            !loaded.diagnostic_detail.empty(),
        "corrupt versioned cache should expose an invalid-document issue and parser detail");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestVersionedJsonCacheShellRejectsUnsupportedSchema()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "specforge_json_cache_schema_tests";
    const std::filesystem::path path = root / "state.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    WriteTextFile(
        path,
        "{\n"
        "  \"format_kind\": \"specforge.test.cache\",\n"
        "  \"schema_version\": 99,\n"
        "  \"items\": []\n"
        "}\n");

    const specforge::VersionedJsonCacheLoadResult loaded =
        specforge::LoadVersionedJsonCacheFile(path, "specforge.test.cache", {1, 2}, "test cache");

    Require(!loaded.document.has_value(), "unsupported versioned cache should not load a document");
    Require(!loaded.warning.empty(), "unsupported versioned cache should report a warning");
    Require(
        loaded.issue_kind ==
                specforge::VersionedJsonCacheLoadIssueKind::
                    UnsupportedFormatOrSchema &&
            loaded.diagnostic_detail.find(
                "schema_version=99") !=
                std::string::npos,
        "unsupported versioned cache should expose its semantic issue and schema detail");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestVersionedJsonCacheDiagnosticsUseUtf8Paths()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge_json_cache_utf8_path_tests";
    const std::filesystem::path path =
        root / std::filesystem::path(
                   std::u8string(u8"用户缓存"));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(path);

    const specforge::VersionedJsonCacheLoadResult loaded =
        specforge::LoadVersionedJsonCacheFile(
            path,
            "specforge.test.cache",
            {1},
            "test cache");

    Require(
        loaded.issue_kind ==
                specforge::VersionedJsonCacheLoadIssueKind::
                    ReadFailed &&
            loaded.diagnostic_detail.find(
                "\xE7\x94\xA8\xE6\x88\xB7\xE7\xBC\x93\xE5\xAD\x98") !=
                std::string::npos,
        "versioned cache diagnostics should preserve non-ASCII paths as UTF-8");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestSortedCacheKeysReturnsStableOrder()
{
    const std::unordered_map<std::string, int> values = {
        {"zeta", 1},
        {"alpha", 2},
        {"middle", 3},
    };

    const std::vector<std::string> keys = specforge::SortedCacheKeys(values);
    Require(keys == std::vector<std::string>({"alpha", "middle", "zeta"}), "cache keys should be sorted");
}

void TestLocalUserStateSaveStatusTracksFailuresAndClearsOnSuccess()
{
    using Scheduler = specforge::LocalUserStateSaveScheduler;
    const Scheduler::TimePoint start{};
    Scheduler scheduler(30ms, 120ms);
    specforge::LocalUserStateSaveStatus status;

    scheduler.MarkDirtyAt(start);
    status.MarkFailed("could not write state");
    Require(scheduler.dirty(), "failed status save should leave the scheduler dirty");
    Require(status.failed(), "failed status save should expose failure state");
    Require(status.message() == "could not write state", "failed status save should store the error message");

    scheduler.MarkSaveSucceeded(status);
    Require(!scheduler.dirty(), "successful status save should clear pending state");
    Require(!status.failed(), "successful status save should clear failure state");
    Require(status.recovered(), "successful retry should expose recovery");
    Require(
        status.message() == "could not write state",
        "recovery should retain the previous failure for diagnostics");
    status.ClearRecovered();
    Require(!status.recovered(), "a later state mutation should clear recovery");
    Require(status.message().empty(), "clearing recovery should clear the previous failure");

    scheduler.MarkSaveFailedAt(start + 10ms, status, "retry later");
    Require(status.failed(), "retry failure should expose failure state");
    Require(!scheduler.ShouldAttemptSave(start + 129ms), "retry failure should respect retry backoff");
    Require(scheduler.ShouldAttemptSave(start + 130ms), "retry failure should flush after backoff");
}

void TestLocalUserStateHealthUsesSharedPriorityAndMessages()
{
    specforge::LocalUserStateHealthView health;
    specforge::AppendLocalUserStateHealth(
        health,
        specforge::LocalUserStateArea::
            SourceSession,
        {
            .retrying = true,
            .save_message = "SYSTEM_DETAIL_TOKEN",
            .save_diagnostic_detail =
                "SYSTEM_DETAIL_TOKEN",
        });
    Require(
        health.kind ==
            specforge::LocalUserStateHealthKind::Retrying,
        "retrying should produce retrying health");
    Require(
        health.messages.size() == 1 &&
            health.messages[0].area ==
                specforge::LocalUserStateArea::
                    SourceSession &&
            health.messages[0].kind ==
                specforge::
                    LocalUserStateHealthMessageKind::
                        SaveRetrying &&
            health.messages[0].diagnostic_detail ==
                "SYSTEM_DETAIL_TOKEN",
        "health aggregation must retain only diagnostic detail instead of preformatting English area and retry text");

    specforge::AppendLocalUserStateHealth(
        health,
        specforge::LocalUserStateArea::
            SpectralLines,
        {.recovered = true});
    Require(
        health.kind ==
            specforge::LocalUserStateHealthKind::Retrying,
        "recovery must not hide an outstanding warning");

    specforge::AppendLocalUserStateHealth(
        health,
        specforge::LocalUserStateArea::
            PanelVisibility,
        {
            .retrying = true,
            .save_message =
                "Could not save panel visibility.",
        });
    Require(
        health.kind ==
                specforge::LocalUserStateHealthKind::Retrying &&
            health.messages.back().diagnostic_detail ==
                "Could not save panel visibility.",
        "legacy persistence producers should retain their concrete failure as transitional diagnostic detail");

    specforge::LocalUserStateHealthView explicit_detail_health;
    specforge::AppendLocalUserStateHealth(
        explicit_detail_health,
        specforge::LocalUserStateArea::
            SampleNavigation,
        {
            .load_warning =
                "Legacy application wrapper.",
            .load_diagnostic_detail =
                "CreateFile: access denied",
        });
    Require(
        explicit_detail_health.messages.size() == 1 &&
            explicit_detail_health.messages[0]
                    .diagnostic_detail ==
                "CreateFile: access denied",
        "structured raw diagnostic detail must take precedence over the legacy application message");
}

void TestLocalUserStateSaveSchedulerDebouncesAndRetries()
{
    using Scheduler = specforge::LocalUserStateSaveScheduler;
    const Scheduler::TimePoint start{};
    Scheduler scheduler(30ms, 120ms);
    Require(!scheduler.dirty(), "new save scheduler should start clean");
    Require(!scheduler.next_attempt_time(), "a clean scheduler should have no maintenance deadline");

    scheduler.MarkDirtyAt(start + 10ms);
    Require(scheduler.dirty(), "marking dirty should expose pending state");
    Require(
        scheduler.next_attempt_time() == start + 40ms,
        "marking dirty should publish the exact debounce deadline");
    Require(!scheduler.ShouldAttemptSave(start + 39ms), "save scheduler should wait for the debounce");
    Require(scheduler.ShouldAttemptSave(start + 40ms), "save scheduler should allow a save at the deadline");

    scheduler.MarkSaveFailedAt(start + 40ms);
    Require(scheduler.dirty(), "failed save should keep pending state");
    Require(
        scheduler.next_attempt_time() == start + 160ms,
        "failed save should replace the debounce with the retry deadline");
    Require(!scheduler.ShouldAttemptSave(start + 159ms), "failed save should wait for retry backoff");
    Require(scheduler.ShouldAttemptSave(start + 160ms), "failed save should retry at the deadline");

    scheduler.MarkSaveSucceeded();
    Require(!scheduler.dirty(), "successful save should clear pending state");
    Require(!scheduler.next_attempt_time(), "successful save should clear the maintenance deadline");
    Require(!scheduler.ShouldAttemptSave(start + 1s), "clean save scheduler should not attempt saves");
}

void TestLocalUserStateSaveSchedulerExtendsDebounceWhenMarkedAgain()
{
    using Scheduler = specforge::LocalUserStateSaveScheduler;
    const Scheduler::TimePoint start{};
    Scheduler scheduler(30ms, 120ms);
    scheduler.MarkDirtyAt(start + 5ms);
    Require(!scheduler.ShouldAttemptSave(start + 34ms), "dirty state should not save before its debounce");
    scheduler.MarkDirtyAt(start + 20ms);
    Require(!scheduler.ShouldAttemptSave(start + 49ms), "new dirty state should extend the debounce");
    Require(scheduler.ShouldAttemptSave(start + 50ms), "extended debounce should use the latest change time");
}

void TestLocalUserStateSaveSchedulerDoesNotShortenRetryBackoff()
{
    using Scheduler = specforge::LocalUserStateSaveScheduler;
    const Scheduler::TimePoint start{};
    Scheduler scheduler(30ms, 120ms);

    scheduler.MarkDirtyAt(start);
    scheduler.MarkSaveFailedAt(start + 30ms);
    scheduler.MarkDirtyAt(start + 40ms);

    Require(
        scheduler.next_attempt_time() == start + 150ms,
        "new dirty state should not shorten an existing retry backoff");
    Require(!scheduler.ShouldAttemptSave(start + 149ms), "retry backoff should remain in force");
    Require(scheduler.ShouldAttemptSave(start + 150ms), "retry should remain due at its original deadline");
}

void TestLocalUserStateSaveSchedulerStartsRetryAfterFailureIsReported()
{
    using Scheduler = specforge::LocalUserStateSaveScheduler;
    Scheduler scheduler(0ms, 50ms);
    scheduler.MarkDirty();

    std::this_thread::sleep_for(5ms);
    const auto io_completed = Scheduler::Clock::now();
    scheduler.MarkSaveFailed();

    const auto retry_deadline = scheduler.next_attempt_time();
    Require(retry_deadline.has_value(), "failed save should publish a retry deadline");
    Require(
        *retry_deadline >= io_completed + 50ms,
        "retry backoff should start when the failed I/O reports completion");
}

void TestLocalUserStatePersistenceLifecycleTracksRecoveryAndMutation()
{
    using Lifecycle = specforge::LocalUserStatePersistenceLifecycle;
    const Lifecycle::TimePoint start{};
    Lifecycle lifecycle(30ms, 120ms);
    lifecycle.SetLoadWarning(
        "saved state could not be read",
        "CreateFile: access denied");

    lifecycle.MarkDirtyAt(start + 10ms);
    Require(
        lifecycle.NextMaintenanceDeadline() == start + 40ms,
        "lifecycle should expose the debounced maintenance deadline");

    int attempts = 0;
    Require(
        lifecycle.RunMaintenance(
            start + 39ms,
            [&attempts] {
                ++attempts;
                return Lifecycle::SaveResult{
                    .saved = true};
            }) == Lifecycle::FlushOutcome::NotNeeded &&
            attempts == 0,
        "maintenance before the debounce deadline should not invoke the owner save");

    Require(
        lifecycle.RunMaintenance(
            start + 40ms,
            [&attempts] {
                ++attempts;
                return Lifecycle::SaveResult{
                    .saved = false,
                    .error = "disk full"};
            }) == Lifecycle::FlushOutcome::Failed &&
            attempts == 1,
        "a due maintenance save should report the owner failure");
    const specforge::LocalUserStatePersistenceStatus failed =
        lifecycle.PersistenceStatus();
    const auto retry_deadline = lifecycle.NextMaintenanceDeadline();
    Require(
        lifecycle.dirty() &&
            retry_deadline.has_value() &&
            failed.retrying &&
            failed.load_warning == "saved state could not be read" &&
            failed.load_diagnostic_detail == "CreateFile: access denied" &&
            failed.save_message == "disk full" &&
            failed.save_diagnostic_detail == "disk full",
        "a failed save should retain warning, retry, and diagnostic state");

    Require(
        lifecycle.RunMaintenance(
            *retry_deadline - 1ms,
            [&attempts] {
                ++attempts;
                return Lifecycle::SaveResult{
                    .saved = true};
            }) == Lifecycle::FlushOutcome::NotNeeded &&
            attempts == 1,
        "retry maintenance should respect its backoff");

    Require(
        lifecycle.RunMaintenance(
            *retry_deadline,
            [&attempts] {
                ++attempts;
                return Lifecycle::SaveResult{
                    .saved = true};
            }) == Lifecycle::FlushOutcome::Saved &&
            attempts == 2,
        "a retry at its deadline should report a successful save");
    const specforge::LocalUserStatePersistenceStatus recovered =
        lifecycle.PersistenceStatus();
    Require(
        !lifecycle.dirty() &&
            !lifecycle.NextMaintenanceDeadline() &&
            !recovered.retrying && recovered.recovered &&
            recovered.load_warning.empty() &&
            recovered.load_diagnostic_detail.empty() &&
            recovered.save_message == "disk full" &&
            recovered.save_diagnostic_detail == "disk full",
        "a successful retry should clear pending warnings and expose recovery");

    lifecycle.MarkDirtyAt(start + 200ms);
    const specforge::LocalUserStatePersistenceStatus mutated =
        lifecycle.PersistenceStatus();
    Require(
        lifecycle.dirty() && !mutated.recovered &&
            mutated.save_message.empty() &&
            lifecycle.NextMaintenanceDeadline() == start + 230ms,
        "a later mutation should clear recovered state and re-debounce saving");
}

void TestLocalUserStatePersistenceLifecycleStartsRetryAfterSlowFailureReturns()
{
    using Lifecycle = specforge::LocalUserStatePersistenceLifecycle;
    const Lifecycle::TimePoint start{};
    Lifecycle lifecycle(0ms, 50ms);
    lifecycle.MarkDirtyAt(start);

    Lifecycle::TimePoint callback_completed;
    Require(
        lifecycle.RunMaintenance(
            start,
            [&callback_completed] {
                std::this_thread::sleep_for(5ms);
                callback_completed = Lifecycle::Clock::now();
                return Lifecycle::SaveResult{
                    .saved = false,
                    .error = "slow owner I/O failed"};
            }) == Lifecycle::FlushOutcome::Failed,
        "a slow failed save should report failure after the callback returns");

    const auto retry_deadline = lifecycle.NextMaintenanceDeadline();
    Require(
        retry_deadline.has_value() &&
            *retry_deadline >= callback_completed + 50ms,
        "retry backoff should begin after slow save completion");
}

void TestLocalUserStatePersistenceLifecycleCleanFlushIsNotNeeded()
{
    using Lifecycle = specforge::LocalUserStatePersistenceLifecycle;
    Lifecycle lifecycle(30ms, 120ms);
    int attempts = 0;

    Require(
        lifecycle.Flush(
            [&attempts] {
                ++attempts;
                return Lifecycle::SaveResult{
                    .saved = true};
            }) == Lifecycle::FlushOutcome::NotNeeded &&
            attempts == 0 && !lifecycle.dirty() &&
            !lifecycle.NextMaintenanceDeadline(),
        "a clean flush should report NotNeeded without invoking the save owner");
}

void TestLocalUserStatePersistenceLifecycleFlushesIndependentOwners()
{
    using Lifecycle = specforge::LocalUserStatePersistenceLifecycle;
    const Lifecycle::TimePoint start{};
    Lifecycle failed_owner(1s, 120ms);
    Lifecycle successful_owner(1s, 120ms);
    failed_owner.MarkDirtyAt(start);
    successful_owner.MarkDirtyAt(start);

    int failed_attempts = 0;
    int successful_attempts = 0;
    Require(
        failed_owner.Flush(
            [&failed_attempts] {
                ++failed_attempts;
                return Lifecycle::SaveResult{
                    .saved = false,
                    .error = "owner A unavailable"};
            }) == Lifecycle::FlushOutcome::Failed,
        "one owner flush should report its own failure");
    Require(
        successful_owner.Flush(
            [&successful_attempts] {
                ++successful_attempts;
                return Lifecycle::SaveResult{
                    .saved = true};
            }) == Lifecycle::FlushOutcome::Saved,
        "another owner flush should still run and report success");
    Require(
        failed_attempts == 1 && successful_attempts == 1 &&
            failed_owner.PersistenceStatus().retrying &&
            failed_owner.dirty() &&
            !successful_owner.PersistenceStatus().retrying &&
            !successful_owner.dirty(),
        "a failed owner must not suppress an independent owner's flush outcome");

    Require(
        failed_owner.Flush(
            [&failed_attempts] {
                ++failed_attempts;
                return Lifecycle::SaveResult{
                    .saved = true};
            }) == Lifecycle::FlushOutcome::Saved &&
            failed_attempts == 2 &&
            failed_owner.PersistenceStatus().recovered,
        "an owner should recover independently on a later flush");
}

void TestPanelVisibilityStateCacheRoundTripsHiddenPanels()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "specforge_panel_visibility_tests";
    const std::filesystem::path path = root / "panel-visibility.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    specforge::PanelVisibilityState state;
    state.files = false;
    state.filters = false;
    state.information = false;
    state.spectral_lines = false;

    Require(specforge::SavePanelVisibilityStateCache(path, state), "panel visibility cache should save");
    const std::string stable_output = ReadTextFile(path);
    Require(
        specforge::SavePanelVisibilityStateCache(path, state) &&
            ReadTextFile(path) == stable_output,
        "panel visibility output should be byte-stable");
    const specforge::PanelVisibilityState loaded =
        specforge::LoadPanelVisibilityStateCache(path).state;
    Require(!loaded.files, "files panel hidden state should persist");
    Require(loaded.navigation, "navigation panel visible state should persist");
    Require(loaded.annotations, "annotations panel visible state should persist");
    Require(loaded.labeling, "labeling panel visible state should persist");
    Require(!loaded.filters, "filters panel hidden state should persist");
    Require(loaded.sorting, "sorting panel visible state should persist");
    Require(loaded.smoothing, "smoothing panel visible state should persist");
    Require(!loaded.information, "information panel hidden state should persist");
    Require(!loaded.spectral_lines, "spectral lines panel hidden state should persist");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestPanelVisibilityStateCacheIgnoresCorruptJson()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge_panel_visibility_corrupt_tests";
    const std::filesystem::path path =
        root / "panel-visibility.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    WriteTextFile(path, "{ invalid json");
    const specforge::PanelVisibilityStateCacheLoadResult loaded =
        specforge::LoadPanelVisibilityStateCache(path);
    Require(
        loaded.state == specforge::PanelVisibilityState{},
        "corrupt panel visibility cache should use defaults");
    Require(
        !loaded.warning.empty(),
        "corrupt panel visibility cache should report a warning");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestPanelVisibilityStateCacheDefaultsMissingFieldsToVisible()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "specforge_panel_visibility_partial_tests";
    const std::filesystem::path path = root / "panel-visibility.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    WriteTextFile(
        path,
        "{\n"
        "  \"format_kind\": \"specforge.panel_visibility.cache\",\n"
        "  \"schema_version\": 1,\n"
        "  \"files\": false\n"
        "}\n");

    const specforge::PanelVisibilityState loaded =
        specforge::LoadPanelVisibilityStateCache(path).state;
    Require(!loaded.files, "loaded panel visibility should apply present fields");
    Require(loaded.navigation, "missing navigation visibility should default to visible");
    Require(loaded.annotations, "missing annotations visibility should default to visible");
    Require(loaded.labeling, "missing labeling visibility should default to visible");
    Require(loaded.filters, "missing filters visibility should default to visible");
    Require(loaded.sorting, "missing sorting visibility should default to visible");
    Require(loaded.smoothing, "missing smoothing visibility should default to visible");
    Require(loaded.information, "missing information visibility should default to visible");
    Require(loaded.spectral_lines, "missing spectral lines visibility should default to visible");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestPanelVisibilityStateCacheWarnsAboutInvalidFieldTypes()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge_panel_visibility_invalid_field_tests";
    const std::filesystem::path path =
        root / "panel-visibility.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    WriteTextFile(
        path,
        "{\n"
        "  \"format_kind\": \"specforge.panel_visibility.cache\",\n"
        "  \"schema_version\": 1,\n"
        "  \"files\": false,\n"
        "  \"navigation\": \"visible\"\n"
        "}\n");

    const specforge::PanelVisibilityStateCacheLoadResult loaded =
        specforge::LoadPanelVisibilityStateCache(path);
    Require(
        !loaded.state.files && loaded.state.navigation,
        "valid fields should load while invalid fields use defaults");
    Require(
        !loaded.warning.empty(),
        "present but non-boolean panel visibility fields should report a load warning");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestSharedLifecycleFlushesPanelVisibilityCodec()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "specforge_panel_visibility_persistence_tests";
    const std::filesystem::path path = root / "panel-visibility.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    using Lifecycle = specforge::LocalUserStatePersistenceLifecycle;
    Lifecycle persistence(30ms, 120ms);
    const specforge::PanelVisibilityState previous =
        specforge::LoadPanelVisibilityStateCache(path).state;
    specforge::PanelVisibilityState current = previous;
    current.files = false;
    current.information = false;

    persistence.MarkDirty();
    Require(
        persistence.Flush([&] {
            return Lifecycle::SaveResult{
                .saved = specforge::SavePanelVisibilityStateCache(
                    path,
                    current),
                .error = "Could not save panel visibility.",
            };
        }) != Lifecycle::FlushOutcome::Failed,
        "dirty panel visibility should flush");

    const specforge::PanelVisibilityState restored =
        specforge::LoadPanelVisibilityStateCache(path).state;
    Require(!restored.files, "flushed UI-hidden files panel should restore hidden");
    Require(restored.navigation, "unchanged navigation panel should restore visible");
    Require(!restored.information, "flushed UI-hidden information panel should restore hidden");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestSharedLifecycleRunsPanelVisibilityCodecAtDeadline()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "specforge_panel_visibility_deadline_tests";
    const std::filesystem::path path = root / "panel-visibility.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    using Lifecycle = specforge::LocalUserStatePersistenceLifecycle;
    Lifecycle persistence(30ms, 120ms);
    const specforge::PanelVisibilityState previous =
        specforge::LoadPanelVisibilityStateCache(path).state;
    specforge::PanelVisibilityState current = previous;
    current.smoothing = false;
    persistence.MarkDirty();

    const auto deadline = persistence.NextMaintenanceDeadline();
    Require(deadline.has_value(), "dirty panel visibility should expose a maintenance deadline");
    (void)persistence.RunMaintenance(
        *deadline - 1ms,
        [&] {
            return Lifecycle::SaveResult{
                .saved = specforge::SavePanelVisibilityStateCache(
                    path,
                    current),
                .error = "Could not save panel visibility.",
            };
        });
    Require(!std::filesystem::exists(path), "panel visibility should not save before its deadline");
    (void)persistence.RunMaintenance(
        *deadline,
        [&] {
            return Lifecycle::SaveResult{
                .saved = specforge::SavePanelVisibilityStateCache(
                    path,
                    current),
                .error = "Could not save panel visibility.",
            };
        });
    Require(std::filesystem::exists(path), "panel visibility should save exactly at its deadline");
    Require(!persistence.NextMaintenanceDeadline(), "successful maintenance should clear the deadline");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestSharedLifecycleReportsPanelVisibilityCodecRecovery()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge_panel_visibility_recovery_tests";
    const std::filesystem::path blocker =
        root / "not-a-directory";
    const std::filesystem::path path =
        blocker / "panel-visibility.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    WriteTextFile(blocker, "block cache directory creation");

    using Lifecycle = specforge::LocalUserStatePersistenceLifecycle;
    Lifecycle persistence(30ms, 120ms);
    specforge::PanelVisibilityState current =
        specforge::LoadPanelVisibilityStateCache(path).state;
    current.files = false;
    persistence.MarkDirty();
    Require(
        persistence.Flush([&] {
            return Lifecycle::SaveResult{
                .saved = specforge::SavePanelVisibilityStateCache(
                    path,
                    current),
                .error = "Could not save panel visibility.",
            };
        }) == Lifecycle::FlushOutcome::Failed,
        "blocked panel visibility path should fail to flush");
    Require(
        persistence.PersistenceStatus().retrying &&
            !persistence.PersistenceStatus()
                 .save_message.empty(),
        "failed panel visibility flush should expose retrying status");

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    Require(
        persistence.Flush([&] {
            return Lifecycle::SaveResult{
                .saved = specforge::SavePanelVisibilityStateCache(
                    path,
                    current),
                .error = "Could not save panel visibility.",
            };
        }) == Lifecycle::FlushOutcome::Saved,
        "panel visibility flush should retry after the path is repaired");
    Require(
        persistence.PersistenceStatus().recovered,
        "successful panel visibility retry should expose recovery");

    specforge::PanelVisibilityState next = current;
    next.files = true;
    persistence.MarkDirty();
    Require(
        !persistence.PersistenceStatus().recovered,
        "a later panel visibility mutation should clear recovery");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestCancelableTextStreamReadStopsBetweenChunks()
{
    constexpr std::size_t kReadChunkBytes = 1024U * 1024U;
    std::istringstream stream(std::string(3U * kReadChunkBytes, 'x'));
    std::string contents;
    std::size_t cancellation_checks = 0;
    class CancellationMarker final : public std::runtime_error {
    public:
        CancellationMarker()
            : std::runtime_error("text stream cancellation marker")
        {
        }
    };

    bool canceled = false;
    try {
        (void)specforge::ReadTextStreamCancelable(
            stream,
            contents,
            [&cancellation_checks]() {
                if (++cancellation_checks == 2) {
                    throw CancellationMarker();
                }
            });
    } catch (const CancellationMarker&) {
        canceled = true;
    }

    Require(canceled, "text stream read should propagate cooperative cancellation");
    Require(cancellation_checks == 2, "text stream read should check cancellation before every chunk");
    Require(
        !contents.empty() && contents.size() <= kReadChunkBytes,
        "text stream cancellation should stop after at most one bounded chunk");
}

}  // namespace

int main()
{
    try {
        TestDefaultLocalUserStatePathUsesSpecForgeRoot();
        TestRuntimePathPoliciesKeepPortableAndInstalledRootsDistinct();
        TestAtomicWriteCreatesParentAndReplacesExistingFile();
        TestAtomicWriteCleansTemporaryAndPreservesExistingFileOnWriterFailure();
        TestAtomicReplaceRetriesTransientSharingViolation();
        TestAtomicReplaceDoesNotRetryByDefault();
        TestJsonSyntaxAndSchemaBoundaries();
        TestJsonResourceBoundsAndCancellation();
        TestVersionedJsonCacheShellRoundTripsDocument();
        TestStructuredJsonCacheRejectsInvalidBody();
        TestJsonWriteLimitsPreserveDurableState();
        TestVersionedJsonCacheShellReportsCorruptCacheWarning();
        TestVersionedJsonCacheShellRejectsUnsupportedSchema();
        TestVersionedJsonCacheDiagnosticsUseUtf8Paths();
        TestSortedCacheKeysReturnsStableOrder();
        TestLocalUserStateSaveStatusTracksFailuresAndClearsOnSuccess();
        TestLocalUserStateHealthUsesSharedPriorityAndMessages();
        TestLocalUserStateSaveSchedulerDebouncesAndRetries();
        TestLocalUserStateSaveSchedulerExtendsDebounceWhenMarkedAgain();
        TestLocalUserStateSaveSchedulerDoesNotShortenRetryBackoff();
        TestLocalUserStateSaveSchedulerStartsRetryAfterFailureIsReported();
        TestLocalUserStatePersistenceLifecycleTracksRecoveryAndMutation();
        TestLocalUserStatePersistenceLifecycleStartsRetryAfterSlowFailureReturns();
        TestLocalUserStatePersistenceLifecycleCleanFlushIsNotNeeded();
        TestLocalUserStatePersistenceLifecycleFlushesIndependentOwners();
        TestPanelVisibilityStateCacheRoundTripsHiddenPanels();
        TestPanelVisibilityStateCacheIgnoresCorruptJson();
        TestPanelVisibilityStateCacheDefaultsMissingFieldsToVisible();
        TestPanelVisibilityStateCacheWarnsAboutInvalidFieldTypes();
        TestSharedLifecycleFlushesPanelVisibilityCodec();
        TestSharedLifecycleRunsPanelVisibilityCodecAtDeadline();
        TestSharedLifecycleReportsPanelVisibilityCodecRecovery();
        TestCancelableTextStreamReadStopsBetweenChunks();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
