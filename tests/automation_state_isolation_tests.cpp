#include "app/runtime_paths.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_loader.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/shell_ui.h"
#include "ui/ui_language_settings.h"

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
            L"--write-labeling-seed")) {
        return WriteLabelingSeedFixture(
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
    std::string save_error;
    Require(
        specforge::SaveUiLanguageSettings(
            ordinary_language,
            specforge::UiLanguage::
                SimplifiedChinese,
            &save_error),
        save_error);
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
                    "source-session.json",
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
