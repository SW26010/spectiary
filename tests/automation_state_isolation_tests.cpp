#include "app/application_settings.h"
#include "app/runtime_paths.h"
#include "domain/sample_annotation_io.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_loader.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/shell_ui.h"

#include <Windows.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::string ReadFile(
    const std::filesystem::path& path)
{
    std::ifstream input(
        path,
        std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::filesystem::path UniqueRoot()
{
    const auto suffix =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
    return std::filesystem::temp_directory_path() /
           ("specforge-automation-state-isolation-" +
            std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(suffix));
}

std::optional<std::filesystem::path> ArgumentPath(
    int argc,
    wchar_t** argv,
    std::wstring_view name)
{
    for (int index = 1;
         index + 1 < argc;
         ++index) {
        if (std::wstring_view(argv[index]) ==
            name) {
            return std::filesystem::path(
                argv[index + 1]);
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> ArgumentText(
    int argc,
    wchar_t** argv,
    std::wstring_view name)
{
    for (int index = 1;
         index + 1 < argc;
         ++index) {
        if (std::wstring_view(argv[index]) ==
            name) {
            return std::wstring(argv[index + 1]);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<int>> ArgumentIntList(
    int argc,
    wchar_t** argv,
    std::wstring_view name)
{
    const auto text = ArgumentText(argc, argv, name);
    if (!text || text->empty()) {
        return std::nullopt;
    }

    std::vector<int> values;
    std::size_t start = 0;
    while (start <= text->size()) {
        const std::size_t separator =
            text->find(L',', start);
        const std::wstring token = text->substr(
            start,
            separator == std::wstring::npos
                ? std::wstring::npos
                : separator - start);
        if (token.empty()) {
            return std::nullopt;
        }
        wchar_t* end = nullptr;
        errno = 0;
        const long long parsed = std::wcstoll(
            token.c_str(),
            &end,
            10);
        if (errno == ERANGE ||
            end != token.c_str() + token.size() ||
            parsed < (std::numeric_limits<int>::min)() ||
            parsed > (std::numeric_limits<int>::max)()) {
            return std::nullopt;
        }
        values.push_back(static_cast<int>(parsed));
        if (separator == std::wstring::npos) {
            break;
        }
        start = separator + 1;
    }
    return values;
}

bool HasArgument(
    int argc,
    wchar_t** argv,
    std::wstring_view name)
{
    for (int index = 1; index < argc; ++index) {
        if (std::wstring_view(argv[index]) == name) {
            return true;
        }
    }
    return false;
}

std::optional<std::size_t> ArgumentSize(
    int argc,
    wchar_t** argv,
    std::wstring_view name)
{
    for (int index = 1;
         index + 1 < argc;
         ++index) {
        if (std::wstring_view(argv[index]) !=
            name) {
            continue;
        }
        const std::wstring_view value(
            argv[index + 1]);
        if (value.empty() ||
            value.front() == L'+' ||
            value.front() == L'-') {
            return std::nullopt;
        }
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long parsed =
            std::wcstoull(
                value.data(),
                &end,
                10);
        if (errno == ERANGE ||
            end != value.data() +
                       value.size() ||
            parsed >
                (std::numeric_limits<
                    std::size_t>::max)()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(
            parsed);
    }
    return std::nullopt;
}

int WriteLabelingCoordinationDirectoriesFixture(
    int argc,
    wchar_t** argv)
{
    const auto cache_path = ArgumentPath(
        argc,
        argv,
        L"--write-labeling-coordination-directories");
    const auto manifest_path = ArgumentPath(
        argc,
        argv,
        L"--manifest-path");
    if (!cache_path || !manifest_path) {
        return 2;
    }

    const std::vector<std::filesystem::path> directories =
        specforge::SampleLabelingStateCoordinationDirectories(
            *cache_path);
    if (directories.empty()) {
        return 3;
    }
    std::ofstream output(
        *manifest_path,
        std::ios::binary | std::ios::trunc);
    if (!output) {
        return 4;
    }
    for (const std::filesystem::path& directory : directories) {
        const std::u8string utf8 = directory.u8string();
        output.write(
            reinterpret_cast<const char*>(utf8.data()),
            static_cast<std::streamsize>(utf8.size()));
        output.put('\n');
    }
    return output.good() ? 0 : 5;
}

int WriteLabelingSeedFixture(
    int argc,
    wchar_t** argv)
{
    const auto seed_path =
        ArgumentPath(
            argc,
            argv,
            L"--write-labeling-seed");
    const auto source_path =
        ArgumentPath(
            argc,
            argv,
            L"--source");
    const auto output_path =
        ArgumentPath(
            argc,
            argv,
            L"--output-path");
    if (!seed_path || !source_path) {
        return 2;
    }

    const specforge::SpectrumSnapshotHandle snapshot =
        specforge::LoadSpectrumSnapshotFromPath(
            *source_path,
            0);
    if (!snapshot ||
        snapshot->capabilities.has_domain_error ||
        snapshot->collection.spectrum_count < 2U) {
        return 3;
    }
    const specforge::SourceCollectionContext context =
        specforge::LoadSourceCollectionContext(
            *snapshot);
    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask(
            "quality",
            "Quality",
            context.identity.spectrum_count);
    if (!specforge::UpsertSampleLabel(
            task.label_set,
            {5, "accepted", 'a'}) ||
        !specforge::UpsertSampleLabel(
            task.label_set,
            {7, "rejected", 'r'})) {
        return 4;
    }
    task.auto_advance = true;
    if (output_path) {
        task.output_path = *output_path;
        if (HasArgument(
                argc,
                argv,
                L"--materialize-labeling-output")) {
            std::error_code directory_error;
            std::filesystem::create_directories(
                output_path->parent_path(),
                directory_error);
            if (directory_error) {
                return 5;
            }
            const specforge::SampleLabelResultMetadataSource source_metadata{
                .source_name = context.identity.source_name,
                .source_fingerprint = context.identity.source_fingerprint,
                .context_fingerprint = context.identity.context_fingerprint,
                .spectrum_count = context.identity.spectrum_count,
            };
            const specforge::SampleLabelResultWriteOutcome write_outcome =
                specforge::SampleAnnotationIoAdapter{}.SaveLabelResult(
                    *output_path,
                    task,
                    &source_metadata);
            if (!write_outcome.array_saved ||
                !write_outcome.metadata_saved) {
                return 5;
            }
            task.save_state.kind =
                specforge::SampleLabelSaveStateKind::AutosavedToOutput;
        }
    }

    specforge::SampleLabelingSourceState source;
    source.sample_count =
        context.identity.spectrum_count;
    source.source_name =
        context.identity.source_name;
    source.source_fingerprint =
        context.identity.source_fingerprint;
    source.context_fingerprint =
        context.identity.context_fingerprint;
    source.tasks.push_back(std::move(task));
    source.active_task_id = "quality";

    specforge::SampleLabelingStateCache cache;
    cache.sources.emplace(
        context.identity.id,
        std::move(source));
    if (!specforge::SaveSampleLabelingStateCache(
            *seed_path,
            cache)) {
        return 5;
    }
    const auto loaded =
        specforge::LoadSampleLabelingStateCache(
            *seed_path);
    return loaded.warning.empty() &&
                   loaded.cache.sources.size() == 1U
               ? 0
               : 6;
}

int ExerciseLabelingDeleteFixture(
    int argc,
    wchar_t** argv)
{
    const auto cache_path = ArgumentPath(
        argc,
        argv,
        L"--exercise-labeling-delete");
    const auto source_path = ArgumentPath(
        argc,
        argv,
        L"--source");
    const auto task_id = ArgumentPath(
        argc,
        argv,
        L"--task-id");
    if (!cache_path || !source_path) {
        return 2;
    }

    const specforge::SpectrumSnapshotHandle snapshot =
        specforge::LoadSpectrumSnapshotFromPath(
            *source_path,
            0);
    if (!snapshot ||
        snapshot->capabilities.has_domain_error ||
        snapshot->collection.spectrum_count == 0U) {
        return 3;
    }
    const specforge::SourceCollectionContext context =
        specforge::LoadSourceCollectionContext(*snapshot);
    specforge::SampleLabelingController controller(*cache_path);
    controller.ActivateSource(context.identity);
    const std::string selected_task_id =
        task_id
        ? task_id->string()
        : std::string("quality");
    const auto first_view = controller.View();
    if (first_view.active_task == nullptr ||
        first_view.active_task->task_id != selected_task_id) {
        return 4;
    }

    const specforge::SampleLabelingOperationResult deactivated =
        controller.DeactivateActiveTask();
    if (!deactivated.accepted || !deactivated.state_saved) {
        return 5;
    }
    const specforge::SampleLabelingOperationResult reactivated =
        controller.ActivateTask(selected_task_id);
    if (!reactivated.accepted || !reactivated.state_saved) {
        return 6;
    }
    const specforge::SampleLabelingOperationResult deleted =
        controller.DeleteActiveTask();
    if (!deleted.accepted || !deleted.state_saved ||
        controller.View().active_task != nullptr) {
        return 7;
    }
    return controller.FlushStateCache() ? 0 : 8;
}

int VerifyLabelOutputFixture(
    int argc,
    wchar_t** argv)
{
    const auto output_path = ArgumentPath(
        argc,
        argv,
        L"--verify-label-output");
    const auto source_path = ArgumentPath(
        argc,
        argv,
        L"--source");
    const auto expected_values = ArgumentIntList(
        argc,
        argv,
        L"--expected-values");
    const auto task_id_text = ArgumentText(
        argc,
        argv,
        L"--task-id");
    if (!output_path || !source_path ||
        !expected_values || expected_values->empty()) {
        return 2;
    }

    const specforge::SpectrumSnapshotHandle snapshot =
        specforge::LoadSpectrumSnapshotFromPath(
            *source_path,
            0);
    if (!snapshot ||
        snapshot->capabilities.has_domain_error) {
        return 3;
    }
    const specforge::SourceCollectionContext context =
        specforge::LoadSourceCollectionContext(*snapshot);
    if (expected_values->size() !=
        context.identity.spectrum_count) {
        return 4;
    }

    std::string load_error;
    const std::optional<specforge::LoadedSampleLabelResult> loaded =
        specforge::SampleAnnotationIoAdapter{}.LoadLabelResult(
            *output_path,
            context.identity.spectrum_count,
            {},
            &load_error);
    if (!loaded) {
        std::cerr << "FAILED: could not load label output: "
                  << load_error << '\n';
        return 5;
    }
    if (loaded->values != *expected_values ||
        !loaded->metadata_sidecar_exists ||
        !loaded->metadata) {
        return 6;
    }

    const std::string expected_task_id =
        task_id_text
        ? std::filesystem::path(*task_id_text).string()
        : std::string("quality");
    const specforge::SampleLabelResultMetadata& metadata =
        *loaded->metadata;
    if (metadata.task_id != expected_task_id ||
        metadata.value_count !=
            context.identity.spectrum_count ||
        metadata.expected_dtype != "int32" ||
        !metadata.source) {
        return 7;
    }
    const auto& source_metadata = *metadata.source;
    if (source_metadata.source_name !=
            context.identity.source_name ||
        source_metadata.source_fingerprint !=
            context.identity.source_fingerprint ||
        source_metadata.context_fingerprint !=
            context.identity.context_fingerprint ||
        source_metadata.spectrum_count !=
            context.identity.spectrum_count) {
        return 8;
    }
    return 0;
}

int VerifyLabelingStateFixture(
    int argc,
    wchar_t** argv)
{
    const auto cache_path =
        ArgumentPath(
            argc,
            argv,
            L"--verify-labeling-state");
    const auto source_path =
        ArgumentPath(
            argc,
            argv,
            L"--source");
    const auto spectrum_index =
        ArgumentSize(
            argc,
            argv,
            L"--spectrum-index");
    const auto expected_code =
        ArgumentSize(
            argc,
            argv,
            L"--expected-code");
    if (!cache_path || !source_path ||
        !spectrum_index || !expected_code ||
        *expected_code >
            static_cast<std::size_t>(
                (std::numeric_limits<
                    int>::max)())) {
        return 2;
    }

    const specforge::SpectrumSnapshotHandle
        snapshot =
            specforge::
                LoadSpectrumSnapshotFromPath(
                    *source_path,
                    0);
    if (!snapshot ||
        snapshot->capabilities.has_domain_error) {
        return 3;
    }
    const specforge::SourceCollectionContext
        context =
            specforge::
                LoadSourceCollectionContext(
                    *snapshot);
    const auto loaded =
        specforge::LoadSampleLabelingStateCache(
            *cache_path);
    if (!loaded.warning.empty()) {
        return 4;
    }
    const auto source =
        loaded.cache.sources.find(
            context.identity.id);
    if (source ==
            loaded.cache.sources.end() ||
        !source->second.active_task_id) {
        return 5;
    }
    const auto task = std::find_if(
        source->second.tasks.begin(),
        source->second.tasks.end(),
        [&](const auto& candidate) {
            return candidate.task_id ==
                   *source->second
                        .active_task_id;
        });
    if (task == source->second.tasks.end() ||
        *spectrum_index >=
            task->values.size() ||
        task->values[*spectrum_index] !=
            static_cast<int>(*expected_code)) {
        return 6;
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (ArgumentPath(
            argc,
            argv,
            L"--write-labeling-coordination-directories")) {
        return WriteLabelingCoordinationDirectoriesFixture(
            argc,
            argv);
    }
    if (ArgumentPath(
            argc,
            argv,
            L"--exercise-labeling-delete")) {
        return ExerciseLabelingDeleteFixture(
            argc,
            argv);
    }
    if (ArgumentPath(
            argc,
            argv,
            L"--write-labeling-seed")) {
        return WriteLabelingSeedFixture(
            argc,
            argv);
    }
    if (ArgumentPath(
            argc,
            argv,
            L"--verify-label-output")) {
        return VerifyLabelOutputFixture(
            argc,
            argv);
    }
    if (ArgumentPath(
            argc,
            argv,
            L"--verify-labeling-state")) {
        return VerifyLabelingStateFixture(
            argc,
            argv);
    }
    const std::filesystem::path fixture_root =
        UniqueRoot();
    const std::filesystem::path ordinary_root =
        fixture_root / "ordinary";
    const std::filesystem::path automation_root =
        fixture_root / "automation";
    std::error_code error;
    std::filesystem::create_directories(
        ordinary_root,
        error);
    Require(!error, "ordinary fixture root should be created");
    std::filesystem::create_directories(
        automation_root,
        error);
    Require(!error, "automation fixture root should be created");

    const std::filesystem::path
        persistent_seed_cache =
            fixture_root /
            "persistent-output-seed.json";
    const std::filesystem::path
        forbidden_output =
            ordinary_root /
            "must-not-be-read.npy";
    specforge::SampleLabelingTask
        persistent_task =
            specforge::
                CreateSampleLabelingTask(
                    "persistent",
                    "Persistent",
                    2U);
    persistent_task.output_path =
        forbidden_output;
    specforge::SampleLabelingSourceState
        persistent_source;
    persistent_source.sample_count = 2U;
    persistent_source.tasks.push_back(
        std::move(persistent_task));
    persistent_source.active_task_id =
        "persistent";
    specforge::SampleLabelingStateCache
        persistent_cache;
    persistent_cache.sources.emplace(
        "seed-source",
        std::move(persistent_source));
    Require(
        specforge::SaveSampleLabelingStateCache(
            persistent_seed_cache,
            persistent_cache),
        "persistent-output seed fixture should use the production serializer");
    const auto restricted_seed =
        specforge::LoadSampleLabelingStateCache(
            persistent_seed_cache,
            {},
            specforge::
                SampleLabelingStateCacheLoadPolicy::
                    InternalDraftsOnly);
    Require(
        restricted_seed.cache.sources.empty() &&
            restricted_seed.warning ==
                "Persistent labeling output paths are not permitted in an automation state seed." &&
            !std::filesystem::exists(
                forbidden_output),
        "automation production loading must reject a non-null output_path before hydrating or writing its external target");

    const std::filesystem::path ordinary_language =
        ordinary_root / "ui-language.json";
    specforge::ApplicationSettings ordinary_settings({
        .language_settings_path = ordinary_language,
        .ui_scale_settings_path = ordinary_root / "ui-scale.json",
        .input_settings_path = ordinary_root / "input-settings.json",
        .external_source_settings_path =
            ordinary_root / "external-source-settings.json",
        .profile_settings_path = ordinary_root / "profile-settings.json",
        .panel_visibility_path = ordinary_root / "panel-visibility.json",
        .default_profile_output_directory = ordinary_root / "profiles",
    });
    const specforge::ApplicationSettingsResult language_result =
        ordinary_settings.Apply(
            specforge::ApplicationSettingsIntent::SetLanguage(
                specforge::UiLanguage::SimplifiedChinese),
            {});
    Require(
        language_result.applied(),
        language_result.detail);
    const std::string ordinary_before =
        ReadFile(ordinary_language);
    const auto ordinary_write_time =
        std::filesystem::last_write_time(
            ordinary_language);

    specforge::RuntimePathInputs inputs;
    inputs.executable_path =
        specforge::CurrentExecutablePath();
    inputs.local_app_data_user_state_root =
        ordinary_root;
    inputs.local_user_state_root_override =
        automation_root;
    const specforge::SpecForgeStartup startup =
        specforge::PrepareSpecForgeStartup(
            std::move(inputs));
    Require(
        startup.runtime_paths()
                .local_user_state_root ==
            automation_root &&
            startup.runtime_paths()
                    .ui_language_settings_path ==
                automation_root /
                    "ui-language.json" &&
            startup.runtime_paths()
                    .source_session_state_path ==
                automation_root /
                    "source-session.json" &&
            startup.runtime_paths()
                    .spectrum_view_state_path ==
                automation_root /
                    "spectrum-view-state.json",
        "automation override should remap the complete local-state path family");

    {
        specforge::ShellUi shell(startup);
        Require(
            shell.ui_language() ==
                specforge::UiLanguage::English,
            "Shell should not import the ordinary root's Chinese language setting");
        Require(
            !shell.current_snapshot(),
            "isolated Shell should not restore an ordinary user session");
        const auto flush = shell.FlushLocalState();
        Require(
            flush.all_saved(),
            "isolated Shell state should flush normally");
    }

    Require(
        ReadFile(ordinary_language) ==
                ordinary_before &&
            std::filesystem::last_write_time(
                ordinary_language) ==
                ordinary_write_time,
        "automation Shell construction and flush must not modify the ordinary state root");
    Require(
        std::filesystem::exists(
            automation_root),
        "automation state should remain scoped to the launcher-provided root");

    std::filesystem::remove_all(
        fixture_root,
        error);
    std::cout
        << "automation state isolation tests passed\n";
    return 0;
}
