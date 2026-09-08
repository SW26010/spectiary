#include "legacy_labeling_test_support.h"
#include "app/local_user_state.h"
#include "app/local_user_state_json.h"
#include "app/runtime_paths.h"
#include "domain/sample_labeling.h"
#include "platform/atomic_file.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/sample_workflow_state_cache_io.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <process.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
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
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void TouchFile(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    Require(stream.good(), "could not create fixture file");
}

std::string AnnotationSourceId(const std::filesystem::path& path)
{
    return "annotation:" + PathToUtf8(path);
}

std::string UniqueTestToken()
{
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(::_getpid()) + "-" + std::to_string(ticks);
}

class PortableTestRoots {
public:
    PortableTestRoots()
        : runtime_paths(specforge::DefaultRuntimePaths())
    {
        Require(
            runtime_paths.storage_profile ==
                specforge::StorageProfile::Portable,
            "Portable-only test executable should select Portable storage");

        const std::string token = UniqueTestToken();
        temporary_root =
            std::filesystem::temp_directory_path() /
            ("specforge-portable-runtime-state-" + token);
        package_fixture_root =
            runtime_paths.package_root /
            ("portable-runtime-state-test-" + token);
        default_state_path =
            runtime_paths.local_user_state_root /
            ("portable-default-write-" + token + ".txt");

        std::error_code cleanup_error;
        std::filesystem::remove_all(temporary_root, cleanup_error);
        std::filesystem::remove_all(package_fixture_root, cleanup_error);
        std::filesystem::remove(default_state_path, cleanup_error);
        std::filesystem::create_directories(temporary_root);
    }

    ~PortableTestRoots()
    {
        std::error_code cleanup_error;
        std::filesystem::remove(default_state_path, cleanup_error);
        std::filesystem::remove_all(package_fixture_root, cleanup_error);
        std::filesystem::remove_all(temporary_root, cleanup_error);
    }

    specforge::RuntimePaths runtime_paths;
    std::filesystem::path temporary_root;
    std::filesystem::path package_fixture_root;
    std::filesystem::path default_state_path;
};

void TestPortableDefaultStateWriteCreatesDataFile(
    const PortableTestRoots& roots)
{
    const std::filesystem::path path =
        specforge::DefaultLocalUserStatePath(
            roots.default_state_path.filename());
    Require(
        path == roots.default_state_path,
        "portable default state path should use the selected Data root");

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

    Require(
        path.parent_path() == roots.runtime_paths.local_user_state_root,
        "portable default write should target Data");
    Require(
        path.parent_path().filename() == "Data",
        "portable default write parent should be Data");
    Require(
        std::filesystem::exists(path),
        "portable default write should create the file under Data");
    Require(
        ReadTextFile(path) == "portable",
        "portable default write should persist content");
}

void TestUserPathDisplayTextUsesPackageRelativePortablePath(
    const PortableTestRoots& roots)
{
    const std::filesystem::path relative_path =
        roots.package_fixture_root
            .lexically_relative(roots.runtime_paths.package_root) /
        "display" /
        "source.npy";
    const std::filesystem::path package_path =
        roots.runtime_paths.package_root / relative_path;
    const std::string package_display =
        specforge::UserPathDisplayText(package_path);
    Require(
        package_display.find(
            PathToUtf8(roots.runtime_paths.package_root)) ==
            std::string::npos,
        "package-contained user paths should display without the package root");
    Require(
        package_display.find("portable-runtime-state-test-") !=
            std::string::npos,
        "package-contained user paths should display their relative directory");
    Require(
        package_display.find("source.npy") != std::string::npos,
        "package-contained user paths should display their file name");

    const specforge::JsonValue package_reference =
        specforge::PersistedPathReferenceJson(package_path);
    Require(
        specforge::ReadJsonStringMember(
            package_reference,
            "path_kind").value_or("") == "package_relative",
        "nonexistent package-contained paths should persist as package-relative");
    Require(
        specforge::ReadJsonStringMember(
            package_reference,
            "path").value_or("") == PathToUtf8(relative_path),
        "package-relative persistence should keep the lexical suffix");

    Require(
        specforge::UserPathDisplayText(
            roots.runtime_paths.package_root) == ".",
        "the package root should display as the current package-relative directory");
    const specforge::JsonValue package_root_reference =
        specforge::PersistedPathReferenceJson(
            roots.runtime_paths.package_root);
    Require(
        specforge::ReadJsonStringMember(
            package_root_reference,
            "path_kind").value_or("") == "package_relative",
        "the package root should persist as package-relative");
    Require(
        specforge::ReadJsonStringMember(
            package_root_reference,
            "path").value_or("") == ".",
        "the package root should persist with a dot path");
    const std::optional<std::filesystem::path>
        round_tripped_package_root =
            specforge::ReadPersistedPathReference(
                package_root_reference);
    Require(
        round_tripped_package_root.has_value() &&
            *round_tripped_package_root ==
                roots.runtime_paths.package_root,
        "the persisted package root should read back to the current package root");

    const std::filesystem::path external_path =
        roots.temporary_root / "external-display" / "source.npy";
    Require(
        specforge::UserPathDisplayText(external_path) ==
            PathToUtf8(external_path),
        "nonexistent external user paths should keep their absolute display text");
    Require(
        specforge::ReadJsonStringMember(
            specforge::PersistedPathReferenceJson(external_path),
            "path_kind").value_or("") == "absolute",
        "nonexistent external paths should persist as absolute");

    std::wstring differently_cased_root_text =
        roots.runtime_paths.package_root.native();
    for (wchar_t& character : differently_cased_root_text) {
        if (character >= L'a' && character <= L'z') {
            character =
                static_cast<wchar_t>(character - L'a' + L'A');
        } else if (character >= L'A' && character <= L'Z') {
            character =
                static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
    const std::filesystem::path differently_cased_package_path =
        std::filesystem::path(differently_cased_root_text) /
        relative_path;
    Require(
        specforge::UserPathDisplayText(
            differently_cased_package_path) ==
            PathToUtf8(relative_path),
        "package root matching should use case-insensitive Windows path semantics");

    const std::filesystem::path package_root_other =
        roots.runtime_paths.package_root.parent_path() /
        std::filesystem::path(
            roots.runtime_paths.package_root.filename().native() +
            L"-other") /
        "source.npy";
    Require(
        specforge::UserPathDisplayText(package_root_other) ==
            PathToUtf8(package_root_other),
        "a sibling whose name starts with the package root name should remain external");

    const std::filesystem::path escaping_path =
        roots.runtime_paths.package_root /
        "nested" /
        ".." /
        ".." /
        "escaped.npy";
    Require(
        specforge::UserPathDisplayText(escaping_path) ==
            PathToUtf8(escaping_path),
        "a lexical path that escapes the package root should remain external");
}

void TestSampleLabelingStateCacheStoresPackageRelativeOutputPath(
    const PortableTestRoots& roots)
{
    const std::filesystem::path output_path =
        roots.package_fixture_root /
        "labeling" /
        "outputs" /
        "labels.npy";
    const std::filesystem::path cache_path =
        roots.temporary_root / "sample-labeling-state.json";
    std::filesystem::create_directories(output_path.parent_path());

    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "22222222-2222-4222-8222-222222222222",
            "Quality",
            3);
    Require(
        specforge::UpsertSampleLabel(
            task.label_set,
            specforge::SampleLabelDefinition{5, "bad", 'b'}),
        "package-relative fixture should accept label");
    Require(
        specforge::AssignSampleLabel(task, 1, 5).accepted,
        "package-relative fixture should accept label value");
    specforge::test_support::SelectLegacyFixtureOutputPath(task, output_path);
    const specforge::SampleLabelOutputPublicationResult persisted =
        specforge::test_support::PublishLegacyFixture(task);
    Require(
        persisted.published,
        persisted.message.empty()
            ? "package-relative output should save"
            : persisted.message);

    specforge::SampleLabelingSourceState state;
    state.sample_count = 3;
    state.active_task_id = task.task_id;
    state.tasks.push_back(task);
    specforge::SampleLabelingStateCache cache;
    cache.sources.emplace("source-identity", std::move(state));
    Require(
        specforge::SaveSampleLabelingStateCache(cache_path, cache),
        "package-relative labeling cache should save");

    const std::string cache_text = ReadTextFile(cache_path);
    Require(
        cache_text.find("\"path_kind\": \"package_relative\"") !=
            std::string::npos,
        "package-contained output path should be written as a package-relative reference");
    Require(
        cache_text.find(
            PathToUtf8(roots.runtime_paths.package_root)) ==
            std::string::npos,
        "package-relative output path should not store the package root");

    const specforge::SampleLabelingStateCacheLoadResult loaded =
        specforge::LoadSampleLabelingStateCache(cache_path);
    Require(loaded.warning.empty(), loaded.warning);
    const auto source = loaded.cache.sources.find("source-identity");
    Require(
        source != loaded.cache.sources.end(),
        "package-relative labeling source should restore");
    Require(
        source->second.tasks.size() == 1,
        "package-relative labeling task should restore");
    const specforge::SampleLabelingTask& restored_task =
        source->second.tasks.front();
    Require(
        restored_task.persistence.output_path &&
            *restored_task.persistence.output_path == output_path &&
            restored_task.persistence.output_format ==
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar,
        "package-relative output and its legacy owner should restore together");
    Require(
        restored_task.values.size() == 3 &&
            restored_task.values[1] == 5,
        "package-relative output should load values from NPY");
}

void TestSampleLabelingStateCacheWriterRejectsNonUuidTaskIds(
    const PortableTestRoots& roots)
{
    const std::filesystem::path save_path =
        roots.temporary_root / "invalid-task-id-save.json";
    specforge::SampleLabelingTask invalid_task =
        specforge::CreateSampleLabelingTask(
            "quality",
            "Quality",
            2U);
    specforge::SampleLabelingSourceState invalid_state;
    invalid_state.sample_count = 2U;
    invalid_state.active_task_id = invalid_task.task_id;
    invalid_state.tasks.push_back(std::move(invalid_task));
    specforge::SampleLabelingStateCache invalid_cache;
    invalid_cache.sources.emplace(
        "invalid-id-source",
        std::move(invalid_state));
    std::string save_error;
    Require(
        !specforge::SaveSampleLabelingStateCache(
            save_path,
            invalid_cache,
            &save_error) &&
            save_error.find("UUID v4") != std::string::npos &&
            !std::filesystem::exists(save_path),
        "schema-4 cache Save should reject a task id the reader cannot restore");

    const std::filesystem::path patch_path =
        roots.temporary_root / "invalid-task-id-patch.json";
    specforge::SampleLabelingTask valid_task =
        specforge::CreateSampleLabelingTask(
            "11111111-1111-4111-8111-111111111111",
            "Quality",
            2U);
    specforge::SampleLabelingSourceState valid_state;
    valid_state.sample_count = 2U;
    valid_state.active_task_id = valid_task.task_id;
    valid_state.tasks.push_back(valid_task);
    specforge::SampleLabelingStateCache valid_cache;
    valid_cache.sources.emplace(
        "invalid-patch-source",
        std::move(valid_state));
    Require(
        specforge::SaveSampleLabelingStateCache(
            patch_path,
            valid_cache),
        "valid schema-4 cache seed should save");

    valid_task.task_id = "quality";
    specforge::SampleLabelingStateCachePatch patch;
    specforge::SampleLabelingSourceStatePatch& source_patch =
        patch.sources["invalid-patch-source"];
    source_patch.task_tombstones.push_back(
        "11111111-1111-4111-8111-111111111111");
    source_patch.task_upserts.push_back(std::move(valid_task));
    std::string patch_error;
    Require(
        !specforge::CommitSampleLabelingStateCachePatch(
            patch_path,
            patch,
            &patch_error) &&
            patch_error.find("UUID v4") != std::string::npos,
        "schema-4 cache Patch should reject a task id the reader cannot restore");
    const specforge::SampleLabelingStateCacheLoadResult restored =
        specforge::LoadSampleLabelingStateCache(patch_path);
    Require(
        restored.warning.empty() &&
            restored.cache.sources.at("invalid-patch-source")
                    .tasks.front()
                    .task_id ==
                "11111111-1111-4111-8111-111111111111",
        "a rejected non-UUID patch should leave the durable cache unchanged");
}

void TestSourceSessionStateCacheStoresPackageRelativePaths(
    const PortableTestRoots& roots)
{
    const std::filesystem::path fixture_root =
        roots.package_fixture_root / "source-session";
    const std::filesystem::path source_path =
        fixture_root / "sources" / "source.npy";
    const std::filesystem::path annotation_path =
        fixture_root / "annotations" / "labels.npy";
    const std::filesystem::path cache_path =
        roots.temporary_root / "source-session.json";
    std::filesystem::create_directories(source_path.parent_path());
    std::filesystem::create_directories(
        annotation_path.parent_path());
    TouchFile(source_path);
    TouchFile(annotation_path);

    specforge::SourceCollectionSessionStateCache cache;
    specforge::SourceCollectionSavedSource source{
        source_path,
        7,
    };
    source.annotation_paths.push_back(annotation_path);
    cache.sources = {source};
    cache.active_source_index = 0;
    Require(
        specforge::SaveSourceCollectionSessionStateCache(
            cache_path,
            cache),
        "package-relative source session cache should save");

    const std::string cache_text = ReadTextFile(cache_path);
    Require(
        cache_text.find("\"path_kind\": \"package_relative\"") !=
            std::string::npos,
        "package-contained paths should be written as package-relative references");
    Require(
        cache_text.find("C:") == std::string::npos,
        "package-relative source session cache should not store a Windows absolute path");

    const specforge::SourceCollectionSessionStateCache loaded =
        specforge::LoadSourceCollectionSessionStateCache(cache_path)
            .cache;
    Require(
        loaded.sources.size() == 1,
        "package-relative source should load");
    Require(
        loaded.sources[0].path == source_path,
        "package-relative source should resolve under package root");
    Require(
        loaded.sources[0].annotation_paths.size() == 1,
        "package-relative annotation should load");
    Require(
        loaded.sources[0].annotation_paths[0] == annotation_path,
        "package-relative annotation should resolve under package root");
}

void TestSourceSessionStateCacheRebasesLegacyMovedPortablePath(
    const PortableTestRoots& roots)
{
    const std::filesystem::path relative_fixture_root =
        roots.package_fixture_root.lexically_relative(
            roots.runtime_paths.package_root);
    const std::filesystem::path source_path =
        roots.package_fixture_root /
        "legacy-source-session" /
        "sources" /
        "source.npy";
    const std::filesystem::path cache_path =
        roots.temporary_root / "legacy-source-session.json";
    const std::filesystem::path old_package_root =
        roots.temporary_root /
        "old-portable-parent" /
        roots.runtime_paths.package_root.filename();
    const std::filesystem::path old_source_path =
        old_package_root /
        relative_fixture_root /
        "legacy-source-session" /
        "sources" /
        "source.npy";

    std::filesystem::create_directories(source_path.parent_path());
    TouchFile(source_path);

    std::ofstream stream(cache_path, std::ios::trunc);
    Require(
        stream.good(),
        "could not open legacy source-session cache fixture");
    stream << "{\n"
           << "  \"format_kind\": \"specforge.source_collection_session.cache\",\n"
           << "  \"schema_version\": 1,\n"
           << "  \"active_source_index\": 0,\n"
           << "  \"sources\": [\n"
           << "    { \"path\": ";
    specforge::WriteJsonString(
        stream,
        PathToUtf8(old_source_path));
    stream << ", \"last_index\": 5 }\n"
           << "  ]\n"
           << "}\n";
    Require(
        stream.good(),
        "could not write legacy source-session cache fixture");
    stream.close();

    const specforge::SourceCollectionSessionStateCache loaded =
        specforge::LoadSourceCollectionSessionStateCache(cache_path)
            .cache;
    Require(
        loaded.sources.size() == 1,
        "legacy moved portable source should load");
    Require(
        loaded.sources[0].path == source_path,
        "legacy moved portable source should rebase under the current package root");
    Require(
        loaded.sources[0].last_spectrum_index == 5,
        "legacy moved portable source should keep the row index");
}

void TestSampleWorkflowStateCacheStoresPackageRelativeAnnotationSourceIds(
    const PortableTestRoots& roots)
{
    const std::filesystem::path annotation_path =
        roots.package_fixture_root /
        "workflow-state" /
        "annotations" /
        "quality.npy";
    const std::filesystem::path cache_path =
        roots.temporary_root / "sample-workflow-state.json";
    const std::string annotation_source_id =
        AnnotationSourceId(annotation_path);
    std::filesystem::create_directories(
        annotation_path.parent_path());
    TouchFile(annotation_path);

    specforge::SampleWorkflowSourceState state;
    state.selected_filter_source_ids.push_back(
        annotation_source_id);
    specforge::SampleFilterCondition condition;
    condition.source_id = annotation_source_id;
    condition.allowed_value_keys.insert("good");
    state.filter_conditions.push_back(std::move(condition));
    state.selected_sample_sort_source_ids.push_back(
        annotation_source_id);
    state.selected_sample_sort_source_id = annotation_source_id;
    state.selected_sample_sort_direction =
        specforge::SampleNavigationSortDirection::Descending;
    state.annotation_display_names.push_back(
        specforge::SampleAnnotationDisplayNameOverride{
            annotation_source_id,
            "Quality",
        });

    specforge::SampleWorkflowStateCache cache;
    cache.sources_by_identity.emplace(
        "source-identity",
        std::move(state));
    Require(
        specforge::SaveSampleWorkflowStateCache(cache_path, cache),
        "package-relative workflow cache should save");

    const std::string cache_text = ReadTextFile(cache_path);
    Require(
        cache_text.find("\"source_kind\": \"annotation_path\"") !=
            std::string::npos,
        "annotation source ids should be stored as structured path references");
    Require(
        cache_text.find("\"path_kind\": \"package_relative\"") !=
            std::string::npos,
        "package-contained annotation source ids should use package-relative paths");
    Require(
        cache_text.find(
            PathToUtf8(roots.runtime_paths.package_root)) ==
            std::string::npos,
        "package-relative workflow cache should not store the package root");

    const specforge::SampleWorkflowStateCache loaded =
        specforge::LoadSampleWorkflowStateCache(cache_path).cache;
    const auto source =
        loaded.sources_by_identity.find("source-identity");
    Require(
        source != loaded.sources_by_identity.end(),
        "workflow source state should load");
    Require(
        source->second.selected_filter_source_ids ==
            std::vector<std::string>{annotation_source_id},
        "selected filter source id should restore");
    Require(
        source->second.filter_conditions.size() == 1,
        "filter condition should restore");
    Require(
        source->second.filter_conditions[0].source_id ==
            annotation_source_id,
        "filter condition source id should restore");
    Require(
        source->second.selected_sample_sort_source_ids ==
            std::vector<std::string>{annotation_source_id},
        "selected sort source id should restore");
    Require(
        source->second.selected_sample_sort_source_id &&
            *source->second.selected_sample_sort_source_id ==
                annotation_source_id,
        "active sort source id should restore");
    Require(
        source->second.annotation_display_names.size() == 1 &&
            source->second.annotation_display_names[0].source_id ==
                annotation_source_id,
        "annotation display name source id should restore");
}

}  // namespace

int main()
{
    try {
        const PortableTestRoots roots;
        TestPortableDefaultStateWriteCreatesDataFile(roots);
        TestUserPathDisplayTextUsesPackageRelativePortablePath(roots);
        TestSampleLabelingStateCacheStoresPackageRelativeOutputPath(
            roots);
        TestSampleLabelingStateCacheWriterRejectsNonUuidTaskIds(
            roots);
        TestSourceSessionStateCacheStoresPackageRelativePaths(roots);
        TestSourceSessionStateCacheRebasesLegacyMovedPortablePath(
            roots);
        TestSampleWorkflowStateCacheStoresPackageRelativeAnnotationSourceIds(
            roots);
    } catch (const std::exception& error) {
        std::cerr << "portable runtime state tests failed: "
                  << error.what() << '\n';
        return 1;
    }

    std::cout << "portable runtime state tests passed\n";
    return 0;
}
