#include "domain/sample_labeling_asdf_store.h"
#include "app/application_settings.h"
#include "app/runtime_paths.h"
#include "domain/sample_annotation_io.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_loader.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/shell_ui.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

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

namespace spectiary {
struct ShellUiTestAccess {
    static SpectrumViewSessionView SpectrumView(const ShellUi& shell)
    {
        return shell.spectrum_view_session_.View();
    }

    static void ChangeSpectrumPreferencesAndViewport(ShellUi& shell, PlotSeriesColor color)
    {
        shell.spectrum_view_session_.Submit(SpectrumViewSessionCommand::SetPlotSeriesColor(
            SpectrumPlotSeries::RawSpectrum, std::move(color)));
        shell.spectrum_view_session_.Submit(SpectrumViewSessionCommand::RequestFitView());
    }
};
}  // namespace spectiary

namespace {
spectiary::RuntimePaths FixtureRuntimePaths(const std::filesystem::path& state)
{
    spectiary::RuntimePaths paths;
    if (state.filename() == "sample-labeling-state.json" && state.parent_path().filename() == "state") {
        paths.sample_labeling_state_path = state;
        paths.sample_labeling_drafts_path = state.parent_path().parent_path() / "unsaved" / "sample-labeling-drafts.json";
    }
    return paths;
}

constexpr char kLabelingTaskId[] =
    "77777777-7777-4777-8777-777777777777";
constexpr char kPersistentLabelingTaskId[] =
    "88888888-8888-4888-8888-888888888888";

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void Require(const spectiary::SampleLabelingStateCacheSaveResult& result,
             std::string_view message)
{
    Require(result.Succeeded(), message);
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
           ("spectiary-automation-state-isolation-" +
            std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(suffix));
}

void TestPortableSpectrumOwnerIsolation(const std::filesystem::path& fixture_root)
{
    using namespace spectiary;
    const auto package = fixture_root / "portable-package";
    const auto isolated_root = fixture_root / "portable-automation";
    std::filesystem::create_directories(package);
    // Deployment selection is independent of optional build provenance.
    std::ofstream(package / project_identity::kMetadataFilename) <<
        R"({"schema_version":6,"deployment":{"distribution":"portable","storage_profile":"portable"}})";
    RuntimePathInputs inputs{.executable_path = package / "renamed.exe"};
    const auto ordinary = PrepareSpectiaryStartup(inputs);
    Require(ordinary.runtime_paths().storage_profile == StorageProfile::Portable,
        "isolation fixture must exercise actual Portable startup");
    const auto config = ordinary.runtime_paths().spectrum_plot_preferences_path;
    const auto state = ordinary.runtime_paths().spectrum_viewport_state_path;
    const SpectrumPlotPreferences preferences{{
        .raw_spectrum = PlotSeriesColor::ExplicitColor({0.1f, 0.2f, 0.3f, 1}),
    }};
    Require(SaveSpectrumPlotPreferences(config, preferences), "seed ordinary Portable preferences");
    Require(SaveSpectrumViewportState(state, {true, "ordinary-source", {1, 2, 3, 4}}),
        "seed ordinary Portable viewport");
    const auto config_bytes = ReadFile(config);
    const auto state_bytes = ReadFile(state);
    const auto fixed_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    std::filesystem::last_write_time(config, fixed_time);
    std::filesystem::last_write_time(state, fixed_time);

    inputs.application_data_root_override = isolated_root;
    const auto isolated = PrepareSpectiaryStartup(inputs);
    const auto& paths = isolated.runtime_paths();
    Require(paths.package_root == package && paths.application_data_root == isolated_root &&
            paths.spectrum_plot_preferences_path == isolated_root / "config" / "spectrum-plot-preferences.json" &&
            paths.spectrum_viewport_state_path == isolated_root / "state" / "spectrum-viewport-state.json",
        "Portable automation must isolate both final spectrum paths");
    const auto isolated_color = PlotSeriesColor::ExplicitColor({0.7f, 0.6f, 0.5f, 1});
    {
        ShellUi shell(isolated);
        const auto view = ShellUiTestAccess::SpectrumView(shell);
        Require(view.plot_colors == SpectrumPlotColors{} &&
                view.viewport_range_mode == SpectrumViewportRangeMode::Automatic,
            "Portable automation must not import ordinary spectrum preferences or viewport");
        ShellUiTestAccess::ChangeSpectrumPreferencesAndViewport(shell, isolated_color);
        Require(shell.FlushLocalState().all_saved(), "isolated spectrum mutations must flush normally");
    }
    Require(LoadSpectrumPlotPreferences(paths.spectrum_plot_preferences_path).state.plot_colors.raw_spectrum == isolated_color &&
            LoadSpectrumViewportState(paths.spectrum_viewport_state_path).document_present,
        "Portable spectrum writes must establish only isolated owner files");
    {
        ShellUi shell(isolated);
        Require(ShellUiTestAccess::SpectrumView(shell).plot_colors.raw_spectrum == isolated_color,
            "Portable automation restart must load its own preferences");
        Require(shell.FlushLocalState().all_saved(), "isolated restart should flush normally");
    }
    Require(ReadFile(config) == config_bytes && ReadFile(state) == state_bytes &&
            std::filesystem::last_write_time(config) == fixed_time &&
            std::filesystem::last_write_time(state) == fixed_time,
        "Portable automation startup, write, shutdown and restart must leave ordinary owner files untouched");
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
        spectiary::SampleLabelingStateCoordinationDirectories(
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

    const spectiary::SpectrumSnapshotHandle snapshot =
        spectiary::LoadSpectrumSnapshotFromPath(
            *source_path,
            0);
    if (!snapshot ||
        snapshot->capabilities.has_domain_error ||
        snapshot->collection.spectrum_count < 2U) {
        return 3;
    }
    const spectiary::SourceCollectionContext context =
        spectiary::LoadSourceCollectionContext(
            *snapshot);
    spectiary::SampleLabelingTask task =
        spectiary::CreateSampleLabelingTask(
            kLabelingTaskId,
            "Quality",
            context.identity.spectrum_count);
    if (!spectiary::UpsertSampleLabel(
            task.label_set,
            {5, "accepted", 'a'}) ||
        !spectiary::UpsertSampleLabel(
            task.label_set,
            {7, "rejected", 'r'})) {
        return 4;
    }
    task.session.auto_advance = true;
    if (output_path) {
        task.persistence.output_path = *output_path;
        task.persistence.output_format =
            spectiary::SampleLabelingOutputArtifactFormat::CanonicalAsdf;
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
            const auto descriptor = spectiary::BuildSampleLabelingCanonicalSourceDescriptor(
                *snapshot, context);
            const auto write_outcome = spectiary::WriteSampleLabelingAsdfDocumentAtomically(
                *output_path, spectiary::BuildSampleLabelingDocument(descriptor, task.Content().value()));
            if (!write_outcome.succeeded()) {
                return 5;
            }
            task.persistence.save_state.kind =
                spectiary::SampleLabelSaveStateKind::AutosavedToOutput;
        }
    }

    // The launcher accepts one pinned historical schema-4 import fixture.
    // Production writes always use the separate state/checkpoint owners.
    nlohmann::json labels = nlohmann::json::array();
    for (const auto& label : task.label_set.labels)
        labels.push_back({{"code", label.code}, {"name", label.name},
            {"shortcut", std::string(1, label.shortcut)}});
    const auto path_utf8 = output_path ? output_path->u8string() : std::u8string{};
    nlohmann::json record{{"task_id", task.task_id}, {"task_name", task.task_name},
        {"labels", labels}, {"values", task.values.Complete()}, {"auto_advance", true},
        {"canonical_metadata", {{"created_at", "2026-01-01T00:00:00.000Z"},
            {"modified_at", "2026-01-01T00:00:00.000Z"}, {"origin", {{"kind", "manual"}}}}},
        {"output", {{"path", output_path ? nlohmann::json{{"path_kind", "absolute"}, {"path", std::string(path_utf8.begin(), path_utf8.end())}} : nlohmann::json(nullptr)}, {"format", output_path ? "canonical_asdf" : "none"}}}};
    nlohmann::json fixture{{"format_kind", "specforge.sample_labeling_tasks.cache"},
        {"schema_version", 4}, {"sources", nlohmann::json::array({{
            {"identity", context.identity.id}, {"sample_count", context.identity.spectrum_count},
            {"source_name", context.identity.source_name}, {"source_fingerprint", context.identity.source_fingerprint},
            {"context_fingerprint", context.identity.context_fingerprint},
            {"active_task_id", task.task_id}, {"tasks", nlohmann::json::array({record})}}})}};
    std::ofstream fixture_output(*seed_path, std::ios::binary | std::ios::trunc);
    fixture_output << fixture.dump(2);
    fixture_output.close();
    if (!fixture_output) return 5;
    if (output_path) return 0; // Negative seed tests deliberately contain an output locator.
    return spectiary::LoadLegacySampleLabelingDraftSeed(*seed_path).warning.empty() ? 0 : 6;
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

    const spectiary::SpectrumSnapshotHandle snapshot =
        spectiary::LoadSpectrumSnapshotFromPath(
            *source_path,
            0);
    if (!snapshot ||
        snapshot->capabilities.has_domain_error ||
        snapshot->collection.spectrum_count == 0U) {
        return 3;
    }
    const spectiary::SourceCollectionContext context =
        spectiary::LoadSourceCollectionContext(*snapshot);
    spectiary::SampleLabelingController controller(*cache_path, FixtureRuntimePaths(*cache_path));
    controller.ActivateSource(context.identity);
    const std::string selected_task_id =
        task_id
        ? task_id->string()
        : std::string(kLabelingTaskId);
    const auto first_view = controller.View();
    if (first_view.active_task == nullptr ||
        first_view.active_task->task_id != selected_task_id) {
        return 4;
    }

    const spectiary::SampleLabelingOperationResult deactivated =
        controller.DeactivateActiveTask();
    if (!deactivated.accepted || !deactivated.state_saved) {
        return 5;
    }
    const spectiary::SampleLabelingOperationResult reactivated =
        controller.ActivateTask(selected_task_id);
    if (!reactivated.accepted || !reactivated.state_saved) {
        return 6;
    }
    const spectiary::SampleLabelingOperationResult deleted =
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

    const spectiary::SpectrumSnapshotHandle snapshot =
        spectiary::LoadSpectrumSnapshotFromPath(
            *source_path,
            0);
    if (!snapshot ||
        snapshot->capabilities.has_domain_error) {
        return 3;
    }
    const spectiary::SourceCollectionContext context =
        spectiary::LoadSourceCollectionContext(*snapshot);
    if (expected_values->size() !=
        context.identity.spectrum_count) {
        return 4;
    }

    const auto loaded = spectiary::ReadSampleLabelingAsdfDocument(*output_path);
    if (!loaded.succeeded()) {
        return 5;
    }
    if (!std::ranges::equal(loaded.document->annotation.values, *expected_values)) {
        std::cerr << "actual ASDF values:";
        for (const auto value : loaded.document->annotation.values) std::cerr << ' ' << value;
        std::cerr << '\n';
        return 6;
    }
    const std::string expected_task_id = task_id_text
        ? std::filesystem::path(*task_id_text).string() : std::string(kLabelingTaskId);
    if (loaded.document->labeling.id != expected_task_id) {
        return 7;
    }
    const auto& source = loaded.document->source;
    if (source.base_identity != context.identity.id ||
        source.name != context.identity.source_name ||
        source.fingerprint != context.identity.source_fingerprint ||
        source.sample_count != context.identity.spectrum_count) {
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

    const spectiary::SpectrumSnapshotHandle
        snapshot =
            spectiary::
                LoadSpectrumSnapshotFromPath(
                    *source_path,
                    0);
    if (!snapshot ||
        snapshot->capabilities.has_domain_error) {
        return 3;
    }
    const spectiary::SourceCollectionContext
        context =
            spectiary::
                LoadSourceCollectionContext(
                    *snapshot);
    const auto loaded =
        spectiary::LoadSampleLabelingStateCache(FixtureRuntimePaths(*cache_path),
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
    if (task == source->second.tasks.end()) {
        return 6;
    }
    std::optional<spectiary::SampleLabelingTask> projected;
    const spectiary::SampleLabelingTask* effective = &*task;
    if (task->persistence.output_format == spectiary::SampleLabelingOutputArtifactFormat::CanonicalAsdf) {
        const auto document = spectiary::ReadSampleLabelingAsdfDocument(*task->persistence.output_path);
        if (!document.succeeded()) return 6;
        projected = spectiary::ProjectSampleLabelingDocumentTask(*document.document, *task);
        if (!projected) return 6;
        effective = &*projected;
    }
    if (*spectrum_index >= effective->values.SampleCount() ||
        effective->values.Complete()[*spectrum_index] != static_cast<int>(*expected_code)) {
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
    spectiary::SampleLabelingTask
        persistent_task =
            spectiary::
                CreateSampleLabelingTask(
                    kPersistentLabelingTaskId,
                    "Persistent",
                    2U);
    persistent_task.persistence.output_path =
        forbidden_output;
    persistent_task.persistence.output_format =
        spectiary::SampleLabelingOutputArtifactFormat::LegacyNpyWithSidecar;
    spectiary::SampleLabelingSourceState
        persistent_source;
    persistent_source.sample_count = 2U;
    persistent_source.tasks.push_back(
        std::move(persistent_task));
    persistent_source.active_task_id =
        kPersistentLabelingTaskId;
    spectiary::SampleLabelingStateCache
        persistent_cache;
    persistent_cache.sources.emplace(
        "seed-source",
        std::move(persistent_source));
    Require(
        spectiary::SaveSampleLabelingStateCache(spectiary::RuntimePaths{},
            persistent_seed_cache,
            persistent_cache),
        "persistent-output seed fixture should use the production serializer");
    const auto restricted_seed =
        spectiary::LoadSampleLabelingStateCache(spectiary::RuntimePaths{},
            persistent_seed_cache,
            {},
            spectiary::
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
    spectiary::ApplicationSettings ordinary_settings({
        .language_settings_path = ordinary_language,
        .appearance_settings_path =
            ordinary_root / "appearance-settings.json",
        .ui_scale_settings_path = ordinary_root / "ui-scale.json",
        .input_settings_path = ordinary_root / "input-settings.json",
        .external_source_settings_path =
            ordinary_root / "external-source-settings.json",
        .profile_settings_path = ordinary_root / "profile-settings.json",
        .panel_visibility_path = ordinary_root / "panel-visibility.json",
        .default_profile_output_directory = ordinary_root / "profiles",
    });
    const spectiary::ApplicationSettingsResult language_result =
        ordinary_settings.Apply(
            spectiary::ApplicationSettingsIntent::SetLanguage(
                spectiary::UiLanguage::SimplifiedChinese),
            {});
    Require(
        language_result.applied(),
        language_result.detail);
    const std::string ordinary_before =
        ReadFile(ordinary_language);
    const auto ordinary_write_time =
        std::filesystem::last_write_time(
            ordinary_language);

    spectiary::RuntimePathInputs inputs;
    inputs.executable_path =
        spectiary::CurrentExecutablePath();
    inputs.local_app_data_user_state_root =
        ordinary_root;
    inputs.application_data_root_override =
        automation_root;
    const spectiary::SpectiaryStartup startup =
        spectiary::PrepareSpectiaryStartup(
            std::move(inputs));
    Require(
        startup.runtime_paths()
                .application_data_root ==
            automation_root &&
            startup.runtime_paths()
                    .ui_language_settings_path ==
                automation_root / "config" /
                    "ui-language.json" &&
            startup.runtime_paths()
                    .appearance_settings_path ==
                automation_root / "config" /
                    "appearance-settings.json" &&
            startup.runtime_paths()
                    .source_session_state_path ==
                automation_root / "state" /
                    "source-session.json" &&
            startup.runtime_paths()
                    .legacy_spectrum_view_state_path.empty() &&
            startup.runtime_paths().spectrum_plot_preferences_path ==
                automation_root / "config" / "spectrum-plot-preferences.json" &&
            startup.runtime_paths().spectrum_viewport_state_path ==
                automation_root / "state" / "spectrum-viewport-state.json",
        "automation override should remap the complete local-state path family");

    {
        spectiary::ShellUi shell(startup);
        Require(
            shell.ui_language() ==
                spectiary::UiLanguage::English,
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

    TestPortableSpectrumOwnerIsolation(fixture_root);

    std::filesystem::remove_all(
        fixture_root,
        error);
    std::cout
        << "automation state isolation tests passed\n";
    return 0;
}
