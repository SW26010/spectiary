#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "platform/atomic_file.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

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

void TestDefaultLocalUserStatePathUsesSpecForgeRoot()
{
    const std::filesystem::path path = specforge::DefaultLocalUserStatePath("nested/state.json");
    Require(path.filename() == "state.json", "default local state path should keep the requested filename");
    Require(path.parent_path().filename() == "nested", "default local state path should keep relative subdirectories");
    Require(
        path.parent_path().parent_path().filename() == "SpecForge",
        "default local state path should live under the SpecForge local state root");
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
    TestAtomicWriteCreatesParentAndReplacesExistingFile();
    TestAtomicWriteCleansTemporaryAndPreservesExistingFileOnWriterFailure();
    TestVersionedJsonCacheShellRoundTripsDocument();
    TestVersionedJsonCacheShellReportsCorruptCacheWarning();
    TestVersionedJsonCacheShellRejectsUnsupportedSchema();
    TestLocalUserStateSaveSchedulerDebouncesAndRetries();
    TestLocalUserStateSaveSchedulerExtendsDebounceWhenFrameIsKnown();
    return 0;
}
