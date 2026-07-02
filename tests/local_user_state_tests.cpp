#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/runtime_paths.h"
#include "platform/atomic_file.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
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

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

void TestDefaultLocalUserStatePathUsesSpecForgeRoot()
{
    const std::filesystem::path path = specforge::DefaultLocalUserStatePath("nested/state.json");
    const specforge::RuntimePaths runtime_paths = specforge::DefaultRuntimePaths();
    Require(path.filename() == "state.json", "default local state path should keep the requested filename");
    Require(path.parent_path().filename() == "nested", "default local state path should keep relative subdirectories");
    Require(
        path == runtime_paths.local_user_state_root / "nested" / "state.json",
        "default local state path should live under the selected release profile root");
    if (runtime_paths.release_profile == specforge::ReleaseProfile::Portable) {
        Require(
            path.parent_path().parent_path().filename() == "Data",
            "portable local state path should live under the package Data root");
    } else {
        Require(
            path.parent_path().parent_path().filename() == "SpecForge",
            "installed local state path should live under the SpecForge local app data root");
    }
}

void TestRuntimePathPoliciesKeepPortableAndInstalledRootsDistinct()
{
    const std::filesystem::path package_root =
        std::filesystem::temp_directory_path() / "specforge_runtime_path_package";
    const std::filesystem::path installed_root =
        std::filesystem::temp_directory_path() / "specforge_runtime_path_installed";

    specforge::RuntimePathInputs inputs;
    inputs.executable_path = package_root / "SpecForge.exe";
    inputs.installed_local_user_state_root = installed_root;

    const specforge::RuntimePaths portable_paths =
        specforge::RuntimePathsForProfile(specforge::ReleaseProfile::Portable, inputs);
    Require(portable_paths.package_root == package_root, "portable package root should be the executable directory");
    Require(portable_paths.local_user_state_root == package_root / "Data", "portable state should live under Data");
    Require(portable_paths.profile_log_directory == package_root / "Data" / "logs", "portable logs should live under Data/logs");
    Require(
        portable_paths.imgui_ini_path == package_root / "Data" / "specforge-imgui-v2.ini",
        "portable ImGui ini should live under Data");

    const specforge::RuntimePaths installed_paths =
        specforge::RuntimePathsForProfile(specforge::ReleaseProfile::Installed, inputs);
    Require(installed_paths.package_root == package_root, "installed package root should still be the executable directory");
    Require(installed_paths.local_user_state_root == installed_root, "installed state should use local app data root");
    Require(installed_paths.profile_log_directory == installed_root / "logs", "installed logs should live under installed state root");
    Require(
        installed_paths.imgui_ini_path == installed_root / "specforge-imgui-v2.ini",
        "installed ImGui ini should live under installed state root");
    Require(
        portable_paths.local_user_state_root != installed_paths.local_user_state_root,
        "portable and installed state roots should stay distinct");
}

void TestPortableDefaultStateWriteCreatesDataFile()
{
    if (specforge::BuildReleaseProfile() != specforge::ReleaseProfile::Portable) {
        return;
    }

    const specforge::RuntimePaths runtime_paths = specforge::DefaultRuntimePaths();
    const std::filesystem::path path = specforge::DefaultLocalUserStatePath("portable-default-write-smoke.txt");
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);

    specforge::AtomicFileWriteOptions options;
    options.target_description = "portable default state smoke file";
    std::string error;
    Require(
        specforge::WriteFileAtomically(
            path,
            options,
            [](std::ostream& stream, std::string&) {
                stream << "portable";
                return true;
            },
            &error),
        error.empty() ? "portable default state write failed" : error);

    Require(path.parent_path() == runtime_paths.local_user_state_root, "portable default write should target Data");
    Require(path.parent_path().filename() == "Data", "portable default write parent should be Data");
    Require(std::filesystem::exists(path), "portable default write should create the file under Data");
    Require(ReadTextFile(path) == "portable", "portable default write should persist content");
    std::filesystem::remove(path, cleanup_error);
}

void TestUserPathDisplayTextUsesPackageRelativePortablePath()
{
    if (specforge::BuildReleaseProfile() != specforge::ReleaseProfile::Portable) {
        return;
    }

    const specforge::RuntimePaths runtime_paths = specforge::DefaultRuntimePaths();
    const std::filesystem::path package_path =
        runtime_paths.package_root / "package-relative-display-test" / "source.npy";
    const std::string package_display = specforge::UserPathDisplayText(package_path);
    Require(
        package_display.find(PathToUtf8(runtime_paths.package_root)) == std::string::npos,
        "package-contained user paths should display without the package root");
    Require(
        package_display.find("package-relative-display-test") != std::string::npos,
        "package-contained user paths should display their package-relative directory");
    Require(
        package_display.find("source.npy") != std::string::npos,
        "package-contained user paths should display their file name");

    const std::filesystem::path external_path =
        std::filesystem::temp_directory_path() / "specforge_external_display_test" / "source.npy";
    Require(
        specforge::UserPathDisplayText(external_path) == PathToUtf8(external_path),
        "external user paths should keep their absolute display text");
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

void TestVersionedJsonCacheShellRoundTripsDocument()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "specforge_json_cache_tests";
    const std::filesystem::path path = root / "state.json";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    std::string error;
    Require(
        specforge::WriteVersionedJsonCacheFile(
            path,
            "specforge.test.cache",
            2,
            "test cache",
            [](std::ostream& stream, std::string&) {
                stream << ",\n";
                stream << "  \"items\": [\n";
                stream << "    { \"name\": ";
                specforge::WriteJsonString(stream, "alpha");
                stream << ", \"value\": 7 }\n";
                stream << "  ]";
                return true;
            },
            &error),
        error.empty() ? "versioned cache write failed" : error);

    const specforge::VersionedJsonCacheLoadResult loaded =
        specforge::LoadVersionedJsonCacheFile(path, "specforge.test.cache", {2}, "test cache");
    Require(loaded.warning.empty(), loaded.warning);
    Require(loaded.document.has_value(), "versioned cache should load");
    Require(loaded.document->schema_version == 2, "versioned cache should report the parsed schema");
    const specforge::JsonValue* items = specforge::JsonObjectMember(loaded.document->root, "items");
    Require(items != nullptr && items->kind == specforge::JsonValue::Kind::Array, "versioned cache should expose body fields");
    Require(items->array.size() == 1, "versioned cache should preserve array items");
    Require(
        specforge::ReadJsonStringMember(items->array.front(), "name").value_or("") == "alpha",
        "versioned cache should parse item strings");
    Require(
        specforge::ReadJsonIntMember(items->array.front(), "value").value_or(0) == 7,
        "versioned cache should parse item integers");
    std::filesystem::remove_all(root, cleanup_error);
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
    specforge::LocalUserStateSaveScheduler scheduler(30, 120);
    specforge::LocalUserStateSaveStatus status;

    scheduler.MarkDirty();
    status.MarkFailed("could not write state");
    Require(scheduler.dirty(), "failed status save should leave the scheduler dirty");
    Require(status.failed(), "failed status save should expose failure state");
    Require(status.message() == "could not write state", "failed status save should store the error message");

    scheduler.MarkSaveSucceeded(status);
    Require(!scheduler.dirty(), "successful status save should clear pending state");
    Require(!status.failed(), "successful status save should clear failure state");
    Require(status.message().empty(), "successful status save should clear the error message");

    scheduler.MarkSaveFailed(10, status, "retry later");
    Require(status.failed(), "retry failure should expose failure state");
    Require(!scheduler.ShouldAttemptSave(129), "retry failure should respect retry backoff");
    Require(scheduler.ShouldAttemptSave(130), "retry failure should flush after backoff");
}

void TestLocalUserStateSaveSchedulerDebouncesAndRetries()
{
    specforge::LocalUserStateSaveScheduler scheduler(30, 120);
    Require(!scheduler.dirty(), "new save scheduler should start clean");

    scheduler.MarkDirty();
    Require(scheduler.dirty(), "marking dirty should expose pending state");
    Require(!scheduler.ShouldAttemptSave(10), "first save check should schedule the debounce");
    Require(!scheduler.ShouldAttemptSave(39), "save scheduler should wait for debounce frames");
    Require(scheduler.ShouldAttemptSave(40), "save scheduler should allow a save after debounce");

    scheduler.MarkSaveFailed(40);
    Require(scheduler.dirty(), "failed save should keep pending state");
    Require(!scheduler.ShouldAttemptSave(159), "failed save should wait for retry frames");
    Require(scheduler.ShouldAttemptSave(160), "failed save should retry after backoff");

    scheduler.MarkSaveSucceeded();
    Require(!scheduler.dirty(), "successful save should clear pending state");
    Require(!scheduler.ShouldAttemptSave(1000), "clean save scheduler should not attempt saves");
}

void TestLocalUserStateSaveSchedulerExtendsDebounceWhenFrameIsKnown()
{
    specforge::LocalUserStateSaveScheduler scheduler(30, 120);
    scheduler.MarkDirty(5);
    Require(!scheduler.ShouldAttemptSave(34), "known-frame dirty save should not flush before debounce");
    scheduler.MarkDirty(20);
    Require(!scheduler.ShouldAttemptSave(49), "new dirty frame should extend the debounce window");
    Require(scheduler.ShouldAttemptSave(50), "extended debounce should flush at the latest dirty frame");
}

}  // namespace

int main()
{
    TestDefaultLocalUserStatePathUsesSpecForgeRoot();
    TestRuntimePathPoliciesKeepPortableAndInstalledRootsDistinct();
    TestPortableDefaultStateWriteCreatesDataFile();
    TestUserPathDisplayTextUsesPackageRelativePortablePath();
    TestAtomicWriteCreatesParentAndReplacesExistingFile();
    TestAtomicWriteCleansTemporaryAndPreservesExistingFileOnWriterFailure();
    TestVersionedJsonCacheShellRoundTripsDocument();
    TestVersionedJsonCacheShellReportsCorruptCacheWarning();
    TestVersionedJsonCacheShellRejectsUnsupportedSchema();
    TestSortedCacheKeysReturnsStableOrder();
    TestLocalUserStateSaveStatusTracksFailuresAndClearsOnSuccess();
    TestLocalUserStateSaveSchedulerDebouncesAndRetries();
    TestLocalUserStateSaveSchedulerExtendsDebounceWhenFrameIsKnown();
    return 0;
}
