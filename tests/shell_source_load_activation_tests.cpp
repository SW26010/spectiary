#include "ui/shell_ui.h"

#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"
#include "domain/source_path_identity.h"
#include "ui/source_collection_load_queue_internal.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace specforge {

struct ShellUiTestAccess {
    static std::unique_ptr<ShellUi> Create(
        SourceCollectionSession session,
        SourceCollectionLoadQueue source_load_queue)
    {
        return std::unique_ptr<ShellUi>(
            new ShellUi(std::move(session), std::move(source_load_queue)));
    }

    static SourceCollectionSession& Session(ShellUi& shell)
    {
        return shell.session_;
    }

    static SourceCollectionSessionResult Submit(
        ShellUi& shell,
        SourceCollectionSessionIntent intent)
    {
        return shell.SubmitSessionCommand(std::move(intent));
    }

    static SourceCollectionSessionResult SubmitNavigation(
        ShellUi& shell,
        SourceCollectionSessionIntent intent,
        NavigationLatencyInputKind kind,
        std::optional<NavigationLatencyTimePoint> input_at = std::nullopt)
    {
        return shell.SubmitSessionCommand(
            std::move(intent),
            SourceCollectionActivationTransaction::
                NavigationIntent{kind, input_at});
    }

    static void Drain(
        ShellUi& shell,
        bool allow_snapshot_prefetch = true)
    {
        shell.DrainSourceLoads(allow_snapshot_prefetch);
    }

    static void BeginDeferredRestore(ShellUi& shell)
    {
        shell.BeginDeferredSourceRestore();
    }

    static std::size_t PendingLoadCount(const ShellUi& shell)
    {
        return shell.source_activation_.
            PendingLoadCount();
    }

    static std::size_t PendingLoadCount(
        const SourceCollectionActivationTransaction&
            activation)
    {
        return activation.PendingLoadCount();
    }

    static void RegisterCompletionReadyCallback(
        SourceCollectionActivationTransaction& activation,
        SourceCollectionLoadQueue::
            CompletionReadyCallback callback)
    {
        activation.RegisterCompletionReadyCallback(
            std::move(callback));
    }

    static bool PrefetchActive(const ShellUi& shell)
    {
        return shell.source_activation_.PrefetchActive();
    }

    static std::vector<NavigationPrefetchReport>
    TakePrefetchReports(ShellUi& shell)
    {
        return shell.source_activation_.
            TakeNavigationPrefetchReports();
    }

    static std::string_view LoadError(const ShellUi& shell)
    {
        return shell.source_activation_.ErrorMessage();
    }

    static void EnableNavigationTracing(ShellUi& shell, std::uint64_t frame_index)
    {
        shell.source_activation_.BeginFrame(
            true,
            frame_index,
            nullptr);
    }

    static std::vector<NavigationLatencyReport> CompleteFramePresentation(
        ShellUi& shell,
        std::uint64_t frame_index,
        unsigned int viewport_id = 7)
    {
        SubmitSpectrumDraw(shell, frame_index, shell.session_.CurrentSampleSnapshot());
        const NavigationLatencyPresentation presentation{
            viewport_id,
            NavigationLatencyTrace::Now()};
        return shell.source_activation_.
            CompleteNavigationFramePresentations(
                frame_index,
                std::span(&presentation, 1));
    }

    static void SubmitSpectrumDraw(
        ShellUi& shell,
        std::uint64_t frame_index,
        SpectrumSnapshotHandle snapshot,
        unsigned int viewport_id = 7)
    {
        shell.RecordSpectrumDrawSubmission(
            frame_index,
            viewport_id,
            std::move(snapshot));
    }

    static std::vector<NavigationLatencyReport> CompleteFramePresentationWithoutSpectrumDraw(
        ShellUi& shell,
        std::uint64_t frame_index,
        unsigned int viewport_id = 7)
    {
        const NavigationLatencyPresentation presentation{
            viewport_id,
            NavigationLatencyTrace::Now()};
        return shell.source_activation_.
            CompleteNavigationFramePresentations(
                frame_index,
                std::span(&presentation, 1));
    }

    static std::vector<SourceLoadLatencyReport>
    CompleteSourceLoadFramePresentation(
        ShellUi& shell,
        std::uint64_t frame_index,
        unsigned int viewport_id = 7)
    {
        SubmitSpectrumDraw(
            shell,
            frame_index,
            shell.session_.CurrentSampleSnapshot(),
            viewport_id);
        const NavigationLatencyPresentation presentation{
            viewport_id,
            NavigationLatencyTrace::Now()};
        return shell.source_activation_.
            CompleteSourceLoadFramePresentations(
                frame_index,
                std::span(&presentation, 1));
    }

    static std::vector<SourceLoadLatencyReport>
    CompleteSourceLoadFramePresentationWithoutSpectrumDraw(
        ShellUi& shell,
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation>
            presentations = {})
    {
        return shell.source_activation_.
            CompleteSourceLoadFramePresentations(
            frame_index,
            presentations);
    }

    static void RecordNavigationKeyInput(
        ShellUi& shell,
        NavigationLatencyInputKind kind,
        NavigationLatencyTimePoint at)
    {
        shell.RecordNavigationKeyInput(kind, at);
    }

    static std::optional<NavigationLatencyTimePoint> TakeNavigationKeyInput(
        ShellUi& shell,
        NavigationLatencyInputKind kind)
    {
        return shell.TakeNavigationKeyInput(kind);
    }

};

}  // namespace specforge

namespace {

using namespace std::chrono_literals;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Future, typename CancellationCheck>
void WaitForRelease(
    const Future& release,
    const CancellationCheck& canceled,
    std::string_view timeout_message)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (release.wait_for(2ms) != std::future_status::ready) {
        if (canceled()) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::string(timeout_message));
        }
    }
}

std::filesystem::path UniqueTempPath(std::string_view suffix)
{
    static std::atomic_uint64_t next_id = 1;
    return std::filesystem::temp_directory_path() /
           ("specforge_shell_activation_" + std::to_string(next_id.fetch_add(1)) +
            std::string(suffix));
}

specforge::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->source.id = "fixture";
    snapshot->source.display_name = "fixture";
    snapshot->source.path = path;
    snapshot->collection.spectrum_count = 3;
    snapshot->collection.current_index = spectrum_index;
    snapshot->collection.can_move_previous = spectrum_index > 0;
    snapshot->collection.can_move_next = spectrum_index + 1 < 3;
    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_switch_spectrum = true;
    return snapshot;
}

specforge::SpectrumSnapshotHandle MakeSnapshotWithDestructionProbe(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const std::shared_ptr<std::promise<std::thread::id>>& destroyed)
{
    const specforge::SpectrumSnapshotHandle source = MakeSnapshot(path, spectrum_index);
    auto* snapshot = new specforge::SpectrumSnapshot(*source);
    return specforge::SpectrumSnapshotHandle(
        snapshot,
        [destroyed](const specforge::SpectrumSnapshot* value) {
            delete value;
            destroyed->set_value(std::this_thread::get_id());
        });
}

bool SaveAnnotationFixture(
    const std::filesystem::path& path,
    std::vector<int> values,
    std::string* error)
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask(
        "shell-drain-annotation",
        "Shell drain annotation",
        values.size());
    task.values = std::move(values);
    return specforge::SampleAnnotationIoAdapter{}.SaveLabelArray(path, task, error);
}

specforge::SourceCollectionSession MakePreparedDeferredSession(
    const std::filesystem::path& path,
    std::optional<std::string> context_fingerprint_override = std::nullopt)
{
    specforge::SourceCollectionSession session({}, {}, {}, {});
    const specforge::SpectrumSnapshotHandle snapshot = MakeSnapshot(path, 0);
    specforge::SourceCollectionContext context;
    context.identity = specforge::BuildSourceCollectionIdentity(
        *snapshot,
        specforge::CaptureSourceCollectionSingleFileState(path));
    if (context_fingerprint_override) {
        context.identity.context_fingerprint = std::move(*context_fingerprint_override);
    }
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    specforge::PreparedSampleWorkflowState workflow =
        specforge::PrepareSampleWorkflowState(*snapshot, context, 0, {{}, {}});
    Require(
        session.OpenPreparedSource(
                   path,
                   0,
                   snapshot,
                   std::move(context),
                   std::move(workflow))
            .loaded,
        "Shell drain fixture should commit its initial source");
    return session;
}

void OpenPreparedFixtureSource(
    specforge::SourceCollectionSession& session,
    const std::filesystem::path& path,
    std::size_t spectrum_index = 0)
{
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(path, spectrum_index);
    specforge::SourceCollectionContext context;
    context.identity = specforge::BuildSourceCollectionIdentity(
        *snapshot,
        specforge::CaptureSourceCollectionSingleFileState(path));
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    specforge::PreparedSampleWorkflowState workflow =
        specforge::PrepareSampleWorkflowState(
            *snapshot,
            context,
            spectrum_index,
            {{}, {}});
    Require(
        session.OpenPreparedSource(
                   path,
                   spectrum_index,
                   snapshot,
                   std::move(context),
                   std::move(workflow))
            .loaded,
        "fixture source should enter the session");
}

struct SourceSessionCachePaths {
    std::filesystem::path source_session;
    std::filesystem::path navigation;
    std::filesystem::path labeling;
    std::filesystem::path workflow;
};

specforge::SourceCollectionSession MakeCachedSession(
    const SourceSessionCachePaths& cache_paths)
{
    return specforge::SourceCollectionSession(
        cache_paths.source_session,
        cache_paths.navigation,
        cache_paths.labeling,
        cache_paths.workflow);
}

specforge::SourceCollectionPreparationAdapters MakeFixtureLoadDependencies(
    const SourceSessionCachePaths& cache_paths)
{
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&, const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.file_context_builder =
        [](const specforge::SpectrumSnapshot& snapshot,
           const specforge::SourceCollectionSingleFileState& file_state,
           const specforge::SourceCollectionCancellationCheckpoint& checkpoint) {
            checkpoint();
            specforge::SourceCollectionContext context;
            context.identity =
                specforge::BuildSourceCollectionIdentity(snapshot, file_state);
            context.manifest.sample_names = {"alpha", "beta", "gamma"};
            return context;
        };
    dependencies.workflow_cache_paths = {
        cache_paths.labeling,
        cache_paths.workflow,
        cache_paths.navigation,
    };
    return dependencies;
}

std::unique_ptr<specforge::ShellUi> MakeDeferredShell(
    const SourceSessionCachePaths& cache_paths,
    specforge::SourceCollectionPreparationAdapters dependencies)
{
    using Access = specforge::ShellUiTestAccess;
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakeCachedSession(cache_paths),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    Access::BeginDeferredRestore(*shell);
    return shell;
}

bool DrainAllSourceLoads(specforge::ShellUi& shell)
{
    using Access = specforge::ShellUiTestAccess;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        Access::Drain(shell);
        if (Access::PendingLoadCount(shell) == 0) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

bool CurrentSourceMatches(
    specforge::SourceCollectionSession& session,
    const std::filesystem::path& expected_path)
{
    const specforge::SourceCollectionSessionView view = session.View();
    if (!view.current_source_index ||
        *view.current_source_index >= view.sources.size()) {
        return false;
    }
    const specforge::SpectrumSnapshotHandle snapshot =
        session.CurrentSampleSnapshot();
    const std::string expected_key =
        specforge::SourcePathIdentityKey(expected_path);
    return snapshot &&
        specforge::SourcePathIdentityKey(
            view.sources[*view.current_source_index].path) == expected_key &&
        specforge::SourcePathIdentityKey(snapshot->source.path) == expected_key;
}

void TestAutomationGotoAndTargetedLabelNavigationRespectActiveSequence()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path source_path =
        UniqueTempPath("_automation_sequence.npy");
    const std::optional<std::filesystem::path>
        annotation_path =
            specforge::
                SourceCollectionCompanionAnnotationPath(
                    source_path);
    Require(
        annotation_path.has_value(),
        "automation sequence fixture should expose a companion annotation path");
    {
        std::ofstream stream(
            source_path,
            std::ios::binary | std::ios::trunc);
        stream << "fixture";
    }
    std::string annotation_error;
    Require(
        SaveAnnotationFixture(
            *annotation_path,
            {1, 0, 1},
            &annotation_error),
        annotation_error.empty()
            ? "automation sequence annotation should save"
            : annotation_error);

    const auto make_dependencies = []() {
        specforge::
            SourceCollectionPreparationAdapters
                dependencies;
        dependencies.snapshot_loader =
            [](const std::filesystem::path& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index);
            };
        dependencies.workflow_cache_loader =
            [](const auto&,
               const std::function<void()>& checkpoint) {
                checkpoint();
                return specforge::
                    SampleWorkflowPreparationCacheBundle{};
            };
        dependencies.workflow_cache_paths = {{}, {}};
        return dependencies;
    };

    specforge::SourceCollectionSession filtered =
        MakePreparedDeferredSession(source_path);
    std::optional<specforge::SampleAnnotationResult>
        annotation =
            specforge::SampleAnnotationIoAdapter{}.
                Load(
                    *annotation_path,
                    3,
                    &annotation_error);
    Require(
        annotation.has_value(),
        "automation sequence annotation should load");
    const std::string filter_source_id =
        specforge::BuildAnnotationFilterSourceId(
            *annotation);
    Require(
        filtered.Submit(
                    specforge::
                        SourceCollectionSessionIntent::
                            EditSourceCollection(
                                specforge::
                                    SourceCollectionIntent::
                                        AddReadOnlyAnnotationResult(
                                            *annotation_path)))
            .loaded,
        "automation sequence fixture should attach its filter annotation");
    (void)filtered.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleFiltering(
                specforge::SampleFilteringIntent::
                    AddSource(filter_source_id)));
    (void)filtered.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleFiltering(
                specforge::SampleFilteringIntent::
                    SetFilterValueSelected(
                        filter_source_id,
                        "1",
                        true)));
    std::unique_ptr<specforge::ShellUi> filtered_shell =
        Access::Create(
            std::move(filtered),
            specforge::
                MakeSourceCollectionLoadQueueForTesting(
                    make_dependencies()));
    const auto included_index =
        filtered_shell->GotoSpectrumForAutomation(
            2,
            std::nullopt);
    const auto included_name =
        filtered_shell->GotoSpectrumForAutomation(
            std::nullopt,
            "gamma");
    const auto excluded_index =
        filtered_shell->GotoSpectrumForAutomation(
            1,
            std::nullopt);
    const auto excluded_name =
        filtered_shell->GotoSpectrumForAutomation(
            std::nullopt,
            "beta");

    specforge::SourceCollectionSession sorted =
        MakePreparedDeferredSession(source_path);
    (void)sorted.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleSorting(
                specforge::SampleSortingIntent::
                    SetSortSource("sample-name")));
    std::unique_ptr<specforge::ShellUi> sorted_shell =
        Access::Create(
            std::move(sorted),
            specforge::
                MakeSourceCollectionLoadQueueForTesting(
                    make_dependencies()));
    const auto sorted_index =
        sorted_shell->GotoSpectrumForAutomation(
            2,
            std::nullopt);
    const auto sorted_name =
        sorted_shell->GotoSpectrumForAutomation(
            std::nullopt,
            "gamma");

    filtered_shell.reset();
    sorted_shell.reset();
    std::filesystem::remove(source_path);
    std::filesystem::remove(*annotation_path);
    Require(
        included_index.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            included_index.target.index == 2 &&
            included_name.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            included_name.target.index == 2,
        "source-row index/name targets retained by a filter should remain valid for goto and targeted-label navigation");
    Require(
        excluded_index.error ==
                specforge::
                    ShellAutomationNavigationError::
                        FilteredOut &&
            excluded_name.error ==
                specforge::
                    ShellAutomationNavigationError::
                        FilteredOut,
        "source-row index/name targets excluded by the active sequence should remain deterministically filtered out");
    Require(
        sorted_index.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            sorted_index.target.index == 2 &&
            sorted_name.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            sorted_name.target.index == 2,
        "source-row index/name targets should remain valid under non-source-order sorting");
}

void TestExplicitOpenTracesAcceptedPathThroughFirstPresent()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_source_load_present.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "source load trace fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionSession session({}, {}, {}, {});
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path& source, std::size_t index, const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&, const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        std::move(session),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 300;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    shell->OpenSource(path);
    const auto activation_deadline = std::chrono::steady_clock::now() + 2s;
    bool activated = false;
    while (std::chrono::steady_clock::now() < activation_deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot =
            Access::Session(*shell).CurrentSampleSnapshot();
        activated = snapshot && snapshot->source.path == path &&
                    Access::PendingLoadCount(*shell) == 0;
        if (activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const bool waits_for_present = activated;
    Access::SubmitSpectrumDraw(
        *shell,
        presentation_frame,
        Access::Session(*shell).CurrentSampleSnapshot(),
        7);
    const specforge::NavigationLatencyPresentation wrong_viewport_presentation{
        8,
        specforge::NavigationLatencyTrace::Now()};
    const std::vector<specforge::SourceLoadLatencyReport>
        wrong_viewport_reports =
            Access::
                CompleteSourceLoadFramePresentationWithoutSpectrumDraw(
                    *shell,
                    presentation_frame,
                    std::span(
                        &wrong_viewport_presentation,
                        1));
    const bool waits_for_matching_viewport =
        wrong_viewport_reports.empty();
    const std::vector<specforge::SourceLoadLatencyReport> reports =
        Access::CompleteSourceLoadFramePresentation(
            *shell,
            presentation_frame + 1,
            7);

    shell->OpenSource(path);
    const auto replacement_deadline = std::chrono::steady_clock::now() + 2s;
    bool replacement_activated = false;
    while (std::chrono::steady_clock::now() < replacement_deadline) {
        Access::Drain(*shell);
        replacement_activated =
            Access::PendingLoadCount(*shell) == 0;
        if (replacement_activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    Access::SubmitSpectrumDraw(
        *shell,
        presentation_frame + 2,
        MakeSnapshot(path, 0),
        7);
    const specforge::NavigationLatencyPresentation replacement_presentation{
        7,
        specforge::NavigationLatencyTrace::Now()};
    const std::vector<specforge::SourceLoadLatencyReport>
        wrong_snapshot_reports =
            Access::
                CompleteSourceLoadFramePresentationWithoutSpectrumDraw(
                    *shell,
                    presentation_frame + 2,
                    std::span(
                        &replacement_presentation,
                        1));
    shell.reset();
    std::filesystem::remove(path);

    Require(activated, "the explicit source should activate through the async queue");
    Require(
        waits_for_present,
        "a loaded source trace should remain pending until its snapshot is presented");
    Require(
        waits_for_matching_viewport,
        "a successful Present from another viewport must not complete the source load trace");
    Require(
        reports.size() == 1 &&
            reports.front().outcome ==
                specforge::SourceLoadLatencyOutcome::Presented &&
            reports.front().attempts.size() == 1 &&
            reports.front().presentation_viewport_id == 7,
        "the exact loaded snapshot Present should complete one source load trace");
    Require(
        reports.front().accepted_ns <=
                reports.front().attempts.front().load_enqueued_ns &&
            reports.front().attempts.front().completion_drained_ns <=
                reports.front().snapshot_activated_ns &&
            reports.front().snapshot_activated_ns <=
                reports.front().ui_updated_ns &&
            reports.front().ui_updated_ns <= reports.front().first_present_ns,
        "the source load report should retain a monotonic accepted-to-Present chain");
    Require(
        replacement_activated && wrong_snapshot_reports.size() == 1 &&
            wrong_snapshot_reports.front().outcome ==
                specforge::SourceLoadLatencyOutcome::Superseded &&
            wrong_snapshot_reports.front().first_present_ns == 0,
        "drawing a different snapshot must supersede rather than present the source load trace");
}

void TestFailedExplicitOpenProducesTerminalSourceLoadReport()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_source_load_failure.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "failed source load trace fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionSession session({}, {}, {}, {});
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path&, std::size_t, const auto&)
            -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("synthetic decode failure");
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        std::move(session),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t frame_index = 301;
    Access::EnableNavigationTracing(*shell, frame_index);

    shell->OpenSource(path);
    const auto failure_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < failure_deadline &&
           Access::PendingLoadCount(*shell) != 0) {
        Access::Drain(*shell);
        std::this_thread::sleep_for(2ms);
    }
    Access::Drain(*shell);
    const std::vector<specforge::SourceLoadLatencyReport> reports =
        Access::
            CompleteSourceLoadFramePresentationWithoutSpectrumDraw(
                *shell,
                frame_index);
    const bool load_error_visible = !Access::LoadError(*shell).empty();
    shell.reset();
    std::filesystem::remove(path);

    Require(
        reports.size() == 1 &&
            reports.front().outcome ==
                specforge::SourceLoadLatencyOutcome::Failed &&
            reports.front().attempts.size() == 1 &&
            reports.front().first_present_ns == 0,
        "a failed explicit open should emit one terminal report without a Present");
    Require(
        reports.front().attempts.front().completion_drained_ns > 0 &&
            load_error_visible,
        "the failed report should include completion drain and preserve the UI error");
}

void TestRealDrainCommitsOnlyTheLatestRapidNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_real_drain.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "real drain fixture should be created");
        stream << "fixture";
    }

    std::promise<void> row_one_entered_promise;
    std::shared_future<void> row_one_entered = row_one_entered_promise.get_future().share();
    std::promise<void> row_two_entered_promise;
    std::shared_future<void> row_two_entered = row_two_entered_promise.get_future().share();
    std::promise<void> release_decoders_promise;
    std::shared_future<void> release_decoders = release_decoders_promise.get_future().share();
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&row_one_entered_promise,
         &row_two_entered_promise,
         release_decoders](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 1) {
                row_one_entered_promise.set_value();
                WaitForRelease(
                    release_decoders,
                    canceled,
                    "timed out waiting to release navigation row one");
            } else if (index == 2) {
                row_two_entered_promise.set_value();
                WaitForRelease(
                    release_decoders,
                    canceled,
                    "timed out waiting to release navigation row two");
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 77;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    const specforge::SourceCollectionSessionResult first = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const bool first_queued = first.follow_up_spectrum_index == 1;
    const bool row_one_started = row_one_entered.wait_for(2s) == std::future_status::ready;
    const specforge::SourceCollectionSessionResult second = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const bool second_queued = second.follow_up_spectrum_index == 2;
    const bool row_two_started = row_two_entered.wait_for(2s) == std::future_status::ready;
    release_decoders_promise.set_value();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool latest_committed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot =
            Access::Session(*shell).CurrentSampleSnapshot();
        latest_committed = snapshot && snapshot->collection.current_index == 2 &&
            Access::PendingLoadCount(*shell) == 0;
        if (latest_committed) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const bool no_load_error = Access::LoadError(*shell).empty();
    std::vector<specforge::NavigationLatencyReport> navigation_reports =
        Access::CompleteFramePresentation(*shell, presentation_frame, 99);
    const bool detached_mismatch_retained_presented_trace =
        std::none_of(
            navigation_reports.begin(),
            navigation_reports.end(),
            [](const auto& report) {
                return report.outcome ==
                    specforge::NavigationLatencyOutcome::
                        Presented;
            });
    std::vector<specforge::NavigationLatencyReport> presented_reports =
        Access::CompleteFramePresentation(*shell, presentation_frame, 7);
    navigation_reports.insert(
        navigation_reports.end(),
        presented_reports.begin(),
        presented_reports.end());
    const auto presented_report = std::find_if(
        navigation_reports.begin(),
        navigation_reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Presented;
        });
    const auto superseded_report = std::find_if(
        navigation_reports.begin(),
        navigation_reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Superseded;
        });
    const bool terminal_outcomes_present =
        presented_report != navigation_reports.end() &&
        superseded_report != navigation_reports.end();
    const bool trace_indices_correlated = terminal_outcomes_present &&
        presented_report->from_index == 1 && presented_report->target_index == 2;
    const bool trace_viewport_correlated = terminal_outcomes_present &&
        presented_report->presentation_viewport_id == 7;
    const bool trace_attempt_correlated = terminal_outcomes_present &&
        presented_report->attempts.size() == 1 &&
        presented_report->attempts[0].source_task_id != 0 &&
        presented_report->attempts[0].worker_started_ns > 0 &&
        presented_report->attempts[0].snapshot_load_finished_ns >=
            presented_report->attempts[0].snapshot_load_started_ns &&
        presented_report->attempts[0].completion_drained_ns >=
            presented_report->attempts[0].completion_published_ns;
    const bool trace_activation_correlated = trace_attempt_correlated &&
        presented_report->snapshot_activated_ns >=
            presented_report->attempts[0].completion_drained_ns &&
        presented_report->first_present_ns >= presented_report->snapshot_activated_ns;
    const auto target_resolution_sum = [](const specforge::NavigationLatencyReport& report) {
        const specforge::NavigationTargetResolutionReport& resolution =
            report.target_resolution;
        return resolution.effective_index_ns +
            resolution.pending_activation_supersede_ns +
            resolution.base_sequence_ns +
            resolution.target_lookup_ns +
            resolution.target_sequence_ns +
            resolution.navigation_state_result_ns;
    };
    const bool target_resolution_correlated = terminal_outcomes_present &&
        superseded_report->target_resolution.row_count == 3 &&
        !superseded_report->target_resolution.pending_present &&
        presented_report->target_resolution.row_count == 3 &&
        presented_report->target_resolution.pending_present &&
        presented_report->target_resolution.sequence_cache_hit &&
        presented_report->target_resolution.sequence_build_count == 0 &&
        target_resolution_sum(*presented_report) ==
            presented_report->target_resolved_ns -
                presented_report->requested_ns;
    shell.reset();
    std::filesystem::remove(path);

    Require(first_queued && row_one_started, "row 1 should enter the real Shell worker chain");
    Require(second_queued && row_two_started, "row 2 should supersede row 1 while both use real workers");
    Require(
        latest_committed,
        "the production drain should admit and atomically publish only the latest row 2 completion");
    Require(no_load_error, "canceling the obsolete row 1 completion must not publish a load error");
    Require(
        detached_mismatch_retained_presented_trace,
        "a Present from a viewport that does not host Spectrum must not complete the trace");
    Require(terminal_outcomes_present, "rapid navigation should report presented and superseded traces");
    Require(trace_indices_correlated, "rapid navigation should retain logical from/target indices");
    Require(trace_viewport_correlated, "the presented trace should name the Spectrum viewport");
    Require(trace_attempt_correlated, "the presented trace should retain its complete worker attempt");
    Require(trace_activation_correlated, "activation and Present timestamps should follow worker drain");
    Require(
        target_resolution_correlated,
        "rapid navigation should retain an exact target-resolution breakdown and pending state");
}

void TestGenericRowLocationDoesNotStartPreviousNextTrace()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_locate_row_trace.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "row-location trace fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    Access::EnableNavigationTracing(*shell, 90);

    const specforge::SourceCollectionSessionResult result = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::LocateRow(2))));
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline &&
           Access::PendingLoadCount(*shell) != 0) {
        Access::Drain(*shell);
        std::this_thread::sleep_for(2ms);
    }
    const std::vector<specforge::NavigationLatencyReport>
        reports = Access::CompleteFramePresentation(
            *shell,
            90,
            7);
    shell.reset();
    std::filesystem::remove(path);

    Require(result.follow_up_spectrum_index == 2, "row location should still queue its real source load");
    Require(
        reports.empty(),
        "LocateRow must not be classified as previous/next navigation latency");
}

void TestAcceptedNavigationUsesLatestMatchingRawKeyInput()
{
    using Access = specforge::ShellUiTestAccess;
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        specforge::SourceCollectionSession({}, {}, {}, {}),
        specforge::MakeSourceCollectionLoadQueueForTesting());
    const auto stale = specforge::NavigationLatencyTimePoint(std::chrono::milliseconds(10));
    const auto accepted = specforge::NavigationLatencyTimePoint(std::chrono::milliseconds(20));
    Access::RecordNavigationKeyInput(
        *shell,
        specforge::NavigationLatencyInputKind::KeyboardNext,
        stale);
    Access::RecordNavigationKeyInput(
        *shell,
        specforge::NavigationLatencyInputKind::KeyboardNext,
        accepted);

    const std::optional<specforge::NavigationLatencyTimePoint> correlated =
        Access::TakeNavigationKeyInput(
            *shell,
            specforge::NavigationLatencyInputKind::KeyboardNext);
    Require(correlated == accepted, "an accepted shortcut must correlate with the latest raw key edge");
}

void TestWorkflowAutoAdvanceStartsExplicitTrace()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_auto_advance_trace.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "auto-advance trace fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    Access::EnableNavigationTracing(*shell, 91);

    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::StartOrResumeTemporaryLabelingTask()));
    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::UpsertActiveLabel(
                specforge::SampleLabelDefinition{7, "accepted", 'a'})));
    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(true)));

    const auto drain_and_present =
        [&shell](std::size_t target_index, std::uint64_t presentation_frame) {
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < deadline) {
                Access::Drain(*shell);
                const specforge::SpectrumSnapshotHandle snapshot =
                    Access::Session(*shell).CurrentSampleSnapshot();
                if (snapshot && snapshot->collection.current_index == target_index &&
                    Access::PendingLoadCount(*shell) == 0) {
                    break;
                }
                std::this_thread::sleep_for(2ms);
            }
            return Access::CompleteFramePresentation(*shell, presentation_frame, 7);
        };
    const auto has_complete_target_resolution =
        [](const specforge::NavigationLatencyReport& report) {
            const specforge::NavigationTargetResolutionReport& resolution =
                report.target_resolution;
            const std::int64_t stage_sum =
                resolution.effective_index_ns +
                resolution.pending_activation_supersede_ns +
                resolution.base_sequence_ns +
                resolution.target_lookup_ns +
                resolution.target_sequence_ns +
                resolution.navigation_state_result_ns;
            return resolution.row_count >= 1 &&
                !resolution.pending_present &&
                resolution.sequence_cache_hit &&
                resolution.sequence_build_count == 0 &&
                resolution.effective_index_ns >= 0 &&
                resolution.pending_activation_supersede_ns >= 0 &&
                resolution.base_sequence_ns >= 0 &&
                resolution.target_lookup_ns >= 0 &&
                resolution.target_sequence_ns >= 0 &&
                resolution.navigation_state_result_ns >= 0 &&
                stage_sum ==
                    report.target_resolved_ns - report.requested_ns;
        };

    const specforge::SourceCollectionSessionResult assign_result = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(7)),
        specforge::NavigationLatencyInputKind::AutoAdvance);
    const std::vector<specforge::NavigationLatencyReport> assign_reports =
        drain_and_present(1, 91);

    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(false)));
    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(7)));
    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(true)));
    Access::EnableNavigationTracing(*shell, 92);
    const specforge::SourceCollectionSessionResult clear_result = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::ClearActiveLabelForCurrentSample()),
        specforge::NavigationLatencyInputKind::AutoAdvance);
    const std::vector<specforge::NavigationLatencyReport> clear_reports =
        drain_and_present(2, 92);

    const bool assign_report_complete =
        assign_reports.size() == 1 &&
        assign_reports.front().outcome == specforge::NavigationLatencyOutcome::Presented &&
        assign_reports.front().input_kind == specforge::NavigationLatencyInputKind::AutoAdvance &&
        assign_reports.front().from_index == 0 &&
        assign_reports.front().target_index == 1 &&
        has_complete_target_resolution(assign_reports.front());
    const bool clear_report_complete =
        clear_reports.size() == 1 &&
        clear_reports.front().outcome == specforge::NavigationLatencyOutcome::Presented &&
        clear_reports.front().input_kind == specforge::NavigationLatencyInputKind::AutoAdvance &&
        clear_reports.front().from_index == 1 &&
        clear_reports.front().target_index == 2 &&
        has_complete_target_resolution(clear_reports.front());
    shell.reset();
    std::filesystem::remove(path);

    Require(assign_result.follow_up_spectrum_index == 1, "label assignment should request auto-advance");
    Require(clear_result.follow_up_spectrum_index == 2, "label clearing should request auto-advance");
    Require(
        assign_report_complete,
        "label assignment auto-advance should emit an analyzer-compatible target-resolution report");
    Require(
        clear_report_complete,
        "label clearing auto-advance should emit an analyzer-compatible target-resolution report");
}

void TestWarmUiAndKeyboardNavigationReuseSequenceStateAtFixedIndices()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_warm_sequence_reuse.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "warm sequence reuse fixture should be created");
        stream << "fixture";
    }

    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [&decoder_calls](
                                       const std::filesystem::path& source,
                                       std::size_t index,
                                       const auto&) {
        ++decoder_calls;
        std::this_thread::sleep_for(12ms);
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    Access::EnableNavigationTracing(*shell, 200);

    struct ProjectionTimings {
        std::vector<std::int64_t> base_ns;
        std::vector<std::int64_t> target_ns;
    };
    std::array<ProjectionTimings, 4> timings;
    std::vector<std::int64_t> cold_decode_ns;
    std::vector<std::int64_t> hit_decode_ns;
    std::uint64_t presentation_frame = 200;
    const auto navigate_and_present =
        [&shell,
         &presentation_frame,
         &cold_decode_ns,
         &hit_decode_ns](
            specforge::NavigationLatencyInputKind input_kind,
            const specforge::SampleNavigationRequest& request,
            std::size_t from_index,
            std::size_t target_index,
            bool expected_cache_hit,
            ProjectionTimings& projection_timings) {
            const specforge::SpectrumSnapshotHandle before =
                Access::Session(*shell).CurrentSampleSnapshot();
            Require(
                before && before->collection.current_index == from_index,
                "warm navigation repetition should start from its fixed source index");
            Access::EnableNavigationTracing(*shell, presentation_frame);
            const bool keyboard_input =
                input_kind == specforge::NavigationLatencyInputKind::KeyboardPrevious ||
                input_kind == specforge::NavigationLatencyInputKind::KeyboardNext;
            const specforge::SourceCollectionSessionResult result = Access::SubmitNavigation(
                *shell,
                specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
                    specforge::SampleNavigationIntent::Move(request)),
                input_kind,
                keyboard_input
                    ? std::optional<specforge::NavigationLatencyTimePoint>{
                          specforge::NavigationLatencyTrace::Now()}
                    : std::nullopt);
            Require(
                result.follow_up_spectrum_index == target_index,
                "warm navigation repetition should enqueue the expected target");

            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < deadline) {
                Access::Drain(*shell, false);
                const specforge::SpectrumSnapshotHandle snapshot =
                    Access::Session(*shell).CurrentSampleSnapshot();
                if (snapshot && snapshot->collection.current_index == target_index &&
                    Access::PendingLoadCount(*shell) == 0) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }

            const std::vector<specforge::NavigationLatencyReport> reports =
                Access::CompleteFramePresentation(*shell, presentation_frame++, 7);
            if (reports.size() != 1) {
                const specforge::SpectrumSnapshotHandle after =
                    Access::Session(*shell).CurrentSampleSnapshot();
                throw std::runtime_error(
                    "each warm navigation should complete one trace; frame=" +
                    std::to_string(presentation_frame - 1) +
                    "; reports=" + std::to_string(reports.size()) +
                    "; current=" +
                    (after ? std::to_string(after->collection.current_index) : "none") +
                    "; pending_loads=" + std::to_string(Access::PendingLoadCount(*shell)) +
                    "; load_error=" + std::string(Access::LoadError(*shell)));
            }
            const specforge::NavigationLatencyReport& report = reports.front();
            const specforge::NavigationTargetResolutionReport& resolution =
                report.target_resolution;
            Require(
                report.outcome == specforge::NavigationLatencyOutcome::Presented &&
                    report.input_kind == input_kind &&
                    report.from_index == from_index &&
                    report.target_index == target_index &&
                    report.cache_hit == expected_cache_hit,
                "warm navigation should preserve input kind, indices, presented outcome, and actual decode reuse");
            Require(
                !resolution.pending_present &&
                    resolution.sequence_cache_hit &&
                    resolution.sequence_build_count == 0,
                "fixed-index warm navigation should reuse sequence state without rebuilding");
            const specforge::SourceCollectionNavigationView navigation =
                Access::Session(*shell).View().navigation;
            Require(
                !navigation.exact_sample_name_match &&
                    navigation.sample_name_matches.empty(),
                "passive UI and keyboard navigation sync must not submit the current sample name as a search query");
            Require(
                resolution.base_sequence_ns >= 0 &&
                    resolution.target_sequence_ns >= 0 &&
                    report.attempts.size() == 1,
                "warm navigation should retain valid projection timings and one load attempt");
            const std::int64_t decode_ns =
                report.attempts[0].snapshot_load_finished_ns -
                report.attempts[0].snapshot_load_started_ns;
            (expected_cache_hit ? hit_decode_ns : cold_decode_ns)
                .push_back(decode_ns);
            projection_timings.base_ns.push_back(resolution.base_sequence_ns);
            projection_timings.target_ns.push_back(resolution.target_sequence_ns);
        };

    for (std::size_t repetition = 0; repetition < 100; ++repetition) {
        navigate_and_present(
            specforge::NavigationLatencyInputKind::UiNext,
            specforge::SampleNavigationRequest::Next(),
            0,
            1,
            repetition > 0,
            timings[0]);
        navigate_and_present(
            specforge::NavigationLatencyInputKind::UiPrevious,
            specforge::SampleNavigationRequest::Previous(),
            1,
            0,
            repetition > 0,
            timings[1]);
    }
    for (std::size_t repetition = 0; repetition < 100; ++repetition) {
        navigate_and_present(
            specforge::NavigationLatencyInputKind::KeyboardNext,
            specforge::SampleNavigationRequest::Next(),
            0,
            1,
            true,
            timings[2]);
        navigate_and_present(
            specforge::NavigationLatencyInputKind::KeyboardPrevious,
            specforge::SampleNavigationRequest::Previous(),
            1,
            0,
            true,
            timings[3]);
    }

    const auto percentile = [](std::vector<std::int64_t> values, std::size_t numerator) {
        std::sort(values.begin(), values.end());
        const std::size_t rank =
            std::max<std::size_t>(1, (values.size() * numerator + 99) / 100);
        return values[rank - 1];
    };
    constexpr std::array<std::string_view, 4> kTimingLabels = {
        "ui_next",
        "ui_previous",
        "keyboard_next",
        "keyboard_previous",
    };
    for (std::size_t index = 0; index < timings.size(); ++index) {
        Require(
            timings[index].base_ns.size() == 100 &&
                timings[index].target_ns.size() == 100,
            "each fixed-index input/direction group should contain 100 warm samples");
        std::printf(
            "%.*s count=100 base_ns_p50=%lld base_ns_p95=%lld "
            "target_ns_p50=%lld target_ns_p95=%lld\n",
            static_cast<int>(kTimingLabels[index].size()),
            kTimingLabels[index].data(),
            static_cast<long long>(percentile(timings[index].base_ns, 50)),
            static_cast<long long>(percentile(timings[index].base_ns, 95)),
            static_cast<long long>(percentile(timings[index].target_ns, 50)),
            static_cast<long long>(percentile(timings[index].target_ns, 95)));
    }
    if (decoder_calls.load() != 2) {
        throw std::runtime_error(
            "400 fixed-index Previous/Next visits should decode only the two initial cold rows; decoder_calls=" +
            std::to_string(decoder_calls.load()));
    }
    Require(
        cold_decode_ns.size() == 2 &&
            hit_decode_ns.size() == 398,
        "cache-hit timing groups should match the two cold and 398 resident visits");
    const std::int64_t cold_decode_min =
        *std::min_element(
            cold_decode_ns.begin(),
            cold_decode_ns.end());
    const std::int64_t hit_decode_p95 =
        percentile(hit_decode_ns, 95);
    Require(
        cold_decode_min >= 8'000'000 &&
            hit_decode_p95 * 2 < cold_decode_min,
        "resident hit decode_ms p95 should remain near zero relative to the deliberately slow decoder");

    shell.reset();
    std::filesystem::remove(path);
}

void TestNewActivationSupersedesAnUnpresentedOlderTrace()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_activation_present_owner.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "activation ownership fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 101;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    const auto submit_next = [&shell]() {
        return Access::SubmitNavigation(
            *shell,
            specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::SampleNavigationRequest::Next())),
            specforge::NavigationLatencyInputKind::UiNext);
    };
    const auto drain_to_index = [&shell](std::size_t target_index) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            Access::Drain(*shell);
            const specforge::SpectrumSnapshotHandle snapshot =
                Access::Session(*shell).CurrentSampleSnapshot();
            if (snapshot && snapshot->collection.current_index == target_index &&
                Access::PendingLoadCount(*shell) == 0) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return false;
    };

    const bool first_requested = submit_next().follow_up_spectrum_index == 1;
    const bool first_activated = drain_to_index(1);
    const bool first_still_waiting_for_present =
        Access::CompleteFramePresentationWithoutSpectrumDraw(
            *shell,
            presentation_frame,
            7)
            .empty();
    const bool second_requested = submit_next().follow_up_spectrum_index == 2;
    const bool second_activated = drain_to_index(2);
    const std::vector<specforge::NavigationLatencyReport> reports =
        Access::CompleteFramePresentation(*shell, presentation_frame, 7);
    const std::size_t presented_count = static_cast<std::size_t>(std::count_if(
        reports.begin(),
        reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Presented;
        }));
    const std::size_t superseded_count = static_cast<std::size_t>(std::count_if(
        reports.begin(),
        reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Superseded;
        }));
    shell.reset();
    std::filesystem::remove(path);

    Require(first_requested && first_activated, "the first navigation should activate row 1");
    Require(first_still_waiting_for_present, "the first trace should wait after an unsuccessful Present");
    Require(second_requested && second_activated, "the second navigation should activate row 2");
    Require(
        presented_count == 1 && superseded_count == 1,
        "row 2 Present must present only row 2 and supersede the unpresented row 1 trace");
}

void TestSameFrameSourceSwitchSupersedesActivatedNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path_a = UniqueTempPath("_present_target_a.csv");
    const std::filesystem::path path_b = UniqueTempPath("_present_target_b.csv");
    {
        std::ofstream stream_a(path_a, std::ios::binary | std::ios::trunc);
        std::ofstream stream_b(path_b, std::ios::binary | std::ios::trunc);
        Require(stream_a.good() && stream_b.good(), "source-switch fixtures should be created");
        stream_a << "fixture-a";
        stream_b << "fixture-b";
    }

    specforge::SourceCollectionSession session = MakePreparedDeferredSession(path_a);
    const specforge::SpectrumSnapshotHandle snapshot_b = MakeSnapshot(path_b, 0);
    specforge::SourceCollectionContext context_b;
    context_b.identity = {"present-target-b", "b", "b-source", "b-context", 3};
    context_b.manifest.sample_names = {"one", "two", "three"};
    specforge::PreparedSampleWorkflowState workflow_b =
        specforge::PrepareSampleWorkflowState(*snapshot_b, context_b, 0, {{}, {}});
    Require(
        session.OpenPreparedSource(
                   path_b,
                   0,
                   snapshot_b,
                   std::move(context_b),
                   std::move(workflow_b))
            .loaded,
        "source B should be cached before the navigation fixture starts");
    (void)session.Submit(specforge::SourceCollectionSessionIntent::EditSourceCollection(
        specforge::SourceCollectionIntent::SwitchActive(0)));
    Require(
        session.CurrentSampleSnapshot() &&
            session.CurrentSampleSnapshot()->source.path == path_a,
        "source A should be active before navigation");

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        std::move(session),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 202;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    const specforge::SourceCollectionSessionResult navigation = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const auto activation_deadline = std::chrono::steady_clock::now() + 2s;
    bool target_activated = false;
    while (std::chrono::steady_clock::now() < activation_deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot = Access::Session(*shell).CurrentSampleSnapshot();
        target_activated = snapshot && snapshot->source.path == path_a &&
            snapshot->collection.current_index == 1 && Access::PendingLoadCount(*shell) == 0;
        if (target_activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SourceCollectionSessionResult switched = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::EditSourceCollection(
            specforge::SourceCollectionIntent::SwitchActive(1)));
    const specforge::SpectrumSnapshotHandle active_snapshot = Access::Session(*shell).CurrentSampleSnapshot();
    Access::SubmitSpectrumDraw(*shell, presentation_frame, active_snapshot, 7);
    const std::vector<specforge::NavigationLatencyReport> reports =
        Access::CompleteFramePresentationWithoutSpectrumDraw(*shell, presentation_frame, 7);
    const std::size_t presented_count = static_cast<std::size_t>(std::count_if(
        reports.begin(),
        reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Presented;
        }));
    const std::size_t superseded_count = static_cast<std::size_t>(std::count_if(
        reports.begin(),
        reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Superseded;
        }));
    shell.reset();
    std::filesystem::remove(path_a);
    std::filesystem::remove(path_b);

    Require(navigation.follow_up_spectrum_index == 1 && target_activated, "source A row 1 should activate");
    Require(
        switched.action.snapshot_changed && active_snapshot == snapshot_b,
        "the cached source B snapshot should synchronously take over in the same frame");
    Require(
        presented_count == 0 && superseded_count == 1,
        "a Present that draws source B must supersede, not present, source A's navigation trace");
}

void TestPresentationWithoutSpectrumDrawDoesNotCompleteNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_hidden_spectrum.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "hidden Spectrum fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 203;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    const specforge::SourceCollectionSessionResult navigation = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const auto activation_deadline = std::chrono::steady_clock::now() + 2s;
    bool target_activated = false;
    while (std::chrono::steady_clock::now() < activation_deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot = Access::Session(*shell).CurrentSampleSnapshot();
        target_activated = snapshot && snapshot->collection.current_index == 1 &&
            Access::PendingLoadCount(*shell) == 0;
        if (target_activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const std::vector<specforge::NavigationLatencyReport> reports =
        Access::CompleteFramePresentationWithoutSpectrumDraw(*shell, presentation_frame, 7);
    const bool trace_retained = reports.empty();
    Access::SubmitSpectrumDraw(
        *shell,
        presentation_frame + 1,
        Access::Session(*shell).CurrentSampleSnapshot(),
        7);
    const std::vector<specforge::NavigationLatencyReport> visible_reports =
        Access::CompleteFramePresentationWithoutSpectrumDraw(
            *shell,
            presentation_frame + 1,
            7);
    const bool presented_after_visible_draw =
        visible_reports.size() == 1 &&
        visible_reports.front().outcome == specforge::NavigationLatencyOutcome::Presented;
    shell.reset();
    std::filesystem::remove(path);

    Require(navigation.follow_up_spectrum_index == 1 && target_activated, "the hidden Spectrum target should activate");
    Require(
        reports.empty() && trace_retained,
        "a successful viewport Present without a submitted Spectrum draw must not complete navigation");
    Require(
        presented_after_visible_draw,
        "the retained navigation should complete after its exact snapshot is visibly submitted later");
}

void TestPublishedStaleCompletionIsRejectedWithoutMutatingNewNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_published_stale_completion.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "published stale completion fixture should be created");
        stream << "fixture";
    }

    auto stale_snapshot_destroyed_promise =
        std::make_shared<std::promise<std::thread::id>>();
    std::future<std::thread::id> stale_snapshot_destroyed =
        stale_snapshot_destroyed_promise->get_future();
    std::promise<void> row_two_entered_promise;
    std::shared_future<void> row_two_entered = row_two_entered_promise.get_future().share();
    std::promise<void> release_row_two_promise;
    std::shared_future<void> release_row_two = release_row_two_promise.get_future().share();

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [stale_snapshot_destroyed_promise,
         &row_two_entered_promise,
         release_row_two](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 1) {
                return MakeSnapshotWithDestructionProbe(
                    source,
                    index,
                    stale_snapshot_destroyed_promise);
            }
            if (index == 2) {
                row_two_entered_promise.set_value();
                WaitForRelease(
                    release_row_two,
                    canceled,
                    "timed out waiting to release stale row two");
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    const specforge::SpectrumSnapshotHandle initial_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    std::promise<void> completion_ready_promise;
    std::shared_future<void> completion_ready =
        completion_ready_promise.get_future().share();
    std::atomic_bool completion_ready_signaled = false;
    shell->RegisterSourceLoadCompletionReadyCallback(
        [&completion_ready_promise,
         &completion_ready_signaled]() {
            if (!completion_ready_signaled.exchange(true)) {
                completion_ready_promise.set_value();
            }
        });

    const specforge::SourceCollectionSessionResult first = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool stale_completion_published =
        completion_ready.wait_for(2s) ==
        std::future_status::ready;

    const specforge::SourceCollectionSessionResult second = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool row_two_started =
        row_two_entered.wait_for(2s) == std::future_status::ready;
    const std::thread::id drain_thread = std::this_thread::get_id();
    Access::Drain(*shell);

    const bool stale_snapshot_retired =
        stale_snapshot_destroyed.wait_for(2s) == std::future_status::ready;
    const std::thread::id retirement_thread = stale_snapshot_retired
        ? stale_snapshot_destroyed.get()
        : std::thread::id{};
    const specforge::SpectrumSnapshotHandle snapshot_after_stale_drain =
        Access::Session(*shell).CurrentSampleSnapshot();
    const bool no_load_error = Access::LoadError(*shell).empty();
    const bool new_ticket_retained = Access::PendingLoadCount(*shell) == 1;
    const bool new_session_pending_retained =
        Access::Session(*shell).CancelPendingSampleNavigation(path, 2);

    release_row_two_promise.set_value();
    shell.reset();
    std::filesystem::remove(path);

    Require(first.follow_up_spectrum_index == 1, "row 1 should create the old Shell ticket");
    Require(
        stale_completion_published,
        "row 1 completion should be published before supersession");
    Require(
        second.follow_up_spectrum_index == 2 && row_two_started,
        "new navigation should invalidate row 1 and start the row 2 worker");
    Require(
        snapshot_after_stale_drain == initial_snapshot &&
            snapshot_after_stale_drain->collection.current_index == 0,
        "the already-published stale completion must not replace the committed snapshot");
    Require(
        new_ticket_retained && new_session_pending_retained,
        "rejecting the stale completion must retain both the new Shell ticket and session pending row");
    Require(no_load_error, "rejecting a stale successful completion must not publish an error");
    Require(
        stale_snapshot_retired && retirement_thread != drain_thread,
        "the rejected stale prepared snapshot should be reclaimed off the drain thread");
}

void TestRealDrainPreservesWorkflowChangesMadeWhileFullPlanWaits()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_stale_plan_drain.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "stale plan drain fixture should be created");
        stream << "fixture";
    }

    std::promise<void> decoder_entered_promise;
    std::shared_future<void> decoder_entered = decoder_entered_promise.get_future().share();
    std::promise<void> release_decoder_promise;
    std::shared_future<void> release_decoder = release_decoder_promise.get_future().share();
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&decoder_entered_promise,
         release_decoder](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            decoder_entered_promise.set_value();
            WaitForRelease(
                release_decoder,
                canceled,
                "timed out waiting to release the stale-plan decoder");
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path, "older-context"),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));

    const specforge::SourceCollectionSessionResult navigation = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool navigation_queued = navigation.follow_up_spectrum_index == 1;
    const bool decoder_started = decoder_entered.wait_for(2s) == std::future_status::ready;
    const specforge::SourceCollectionSessionResult labeling_started = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::StartOrResumeTemporaryLabelingTask()));
    const specforge::SourceCollectionSessionResult label_added = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::UpsertActiveLabel(
                specforge::SampleLabelDefinition{7, "live", 'l'})));
    release_decoder_promise.set_value();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool completion_drained = false;
    while (std::chrono::steady_clock::now() < deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot =
            Access::Session(*shell).CurrentSampleSnapshot();
        completion_drained = snapshot && snapshot->collection.current_index == 1 &&
            Access::PendingLoadCount(*shell) == 0;
        if (completion_drained) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SourceCollectionSessionView view = Access::Session(*shell).View();
    const bool live_workflow_preserved =
        view.labeling.has_active_task &&
        specforge::ContainsSampleLabelCode(view.labeling.label_set, 7);
    const bool no_load_error = Access::LoadError(*shell).empty();
    shell.reset();
    std::filesystem::remove(path);

    Require(navigation_queued && decoder_started, "the full-plan worker should wait on row 1");
    Require(
        labeling_started.action.workflow_changed && label_added.changed,
        "the UI should commit newer workflow intent while the full plan waits");
    Require(completion_drained, "production DrainSourceLoads should commit the full plan snapshot");
    Require(
        live_workflow_preserved,
        "the drained older full plan must not replace workflow intent committed while it waited");
    Require(no_load_error, "live workflow reconciliation should complete without a load error");
}

void TestRealDrainRequeuesReconciledTargetAndRetiresIntermediateSnapshotOffThread()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_reentrant_drain.npy");
    const std::optional<std::filesystem::path> annotation_path =
        specforge::SourceCollectionCompanionAnnotationPath(path);
    Require(annotation_path.has_value(), "NPY drain fixture should expose its companion path");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "reentrant drain source fixture should be created");
        stream << "fixture";
    }
    std::string annotation_error;
    Require(
        SaveAnnotationFixture(*annotation_path, {1, 1, 0}, &annotation_error),
        annotation_error.empty() ? "initial drain annotation should save" : annotation_error);

    specforge::SourceCollectionSession session = MakePreparedDeferredSession(path);
    std::optional<specforge::SampleAnnotationResult> initial_annotation =
        specforge::SampleAnnotationIoAdapter{}.Load(
            *annotation_path,
            3,
            &annotation_error);
    Require(initial_annotation.has_value(), "initial drain annotation should load");
    const std::string filter_source_id =
        specforge::BuildAnnotationFilterSourceId(*initial_annotation);
    Require(
        session.Submit(specforge::SourceCollectionSessionIntent::EditSourceCollection(
                           specforge::SourceCollectionIntent::AddReadOnlyAnnotationResult(
                               *annotation_path)))
            .loaded,
        "reentrant drain fixture should attach its annotation");
    (void)session.Submit(specforge::SourceCollectionSessionIntent::ApplySampleFiltering(
        specforge::SampleFilteringIntent::AddSource(filter_source_id)));
    (void)session.Submit(specforge::SourceCollectionSessionIntent::ApplySampleFiltering(
        specforge::SampleFilteringIntent::SetFilterValueSelected(
            filter_source_id,
            "1",
            true)));

    std::promise<void> first_decode_entered_promise;
    std::shared_future<void> first_decode_entered =
        first_decode_entered_promise.get_future().share();
    std::promise<void> release_first_decode_promise;
    std::shared_future<void> release_first_decode =
        release_first_decode_promise.get_future().share();
    std::promise<void> row_two_entered_promise;
    std::shared_future<void> row_two_entered = row_two_entered_promise.get_future().share();
    std::promise<void> release_row_two_promise;
    std::shared_future<void> release_row_two = release_row_two_promise.get_future().share();
    auto intermediate_destroyed_promise =
        std::make_shared<std::promise<std::thread::id>>();
    std::future<std::thread::id> intermediate_destroyed =
        intermediate_destroyed_promise->get_future();
    std::atomic_bool initial_open_failed = false;
    std::atomic_int row_one_decode_count = 0;
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&first_decode_entered_promise,
         release_first_decode,
         &row_two_entered_promise,
         release_row_two,
         intermediate_destroyed_promise,
         &initial_open_failed,
         &row_one_decode_count](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 0 &&
                !initial_open_failed.exchange(true)) {
                throw std::runtime_error(
                    "previous source load failure");
            }
            if (index == 1) {
                if (row_one_decode_count.fetch_add(1) == 0) {
                    first_decode_entered_promise.set_value();
                    WaitForRelease(
                        release_first_decode,
                        canceled,
                        "timed out waiting to release the first retry decoder");
                    return MakeSnapshot(source, index);
                }
                return MakeSnapshotWithDestructionProbe(
                    source,
                    index,
                    intermediate_destroyed_promise);
            }
            if (index == 2) {
                row_two_entered_promise.set_value();
                WaitForRelease(
                    release_row_two,
                    canceled,
                    "timed out waiting to release retry row two");
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        std::move(session),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));

    shell->OpenSource(path);
    const auto initial_failure_deadline =
        std::chrono::steady_clock::now() + 2s;
    bool initial_failure_visible = false;
    while (std::chrono::steady_clock::now() <
           initial_failure_deadline) {
        Access::Drain(*shell);
        initial_failure_visible =
            Access::LoadError(*shell).find(
                "previous source load failure") !=
                std::string_view::npos &&
            Access::PendingLoadCount(*shell) == 0;
        if (initial_failure_visible) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }

    const specforge::SourceCollectionSessionResult navigation = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool navigation_queued = navigation.follow_up_spectrum_index == 1;
    const bool first_decode_started =
        first_decode_entered.wait_for(2s) == std::future_status::ready;
    annotation_error.clear();
    const bool annotation_changed =
        SaveAnnotationFixture(*annotation_path, {0, 0, 1}, &annotation_error);
    release_first_decode_promise.set_value();

    const auto requeue_deadline = std::chrono::steady_clock::now() + 2s;
    bool row_two_requeued = false;
    while (std::chrono::steady_clock::now() < requeue_deadline) {
        Access::Drain(*shell);
        row_two_requeued =
            row_two_entered.wait_for(0ms) == std::future_status::ready;
        if (row_two_requeued) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SpectrumSnapshotHandle snapshot_while_requeued =
        Access::Session(*shell).CurrentSampleSnapshot();
    const specforge::SourceCollectionSessionView view_while_requeued =
        Access::Session(*shell).View();
    const bool failure_retained_while_requeued =
        Access::LoadError(*shell).find(
            "previous source load failure") !=
        std::string_view::npos;
    const std::thread::id caller_thread = std::this_thread::get_id();
    const bool intermediate_retired =
        intermediate_destroyed.wait_for(2s) == std::future_status::ready;
    const std::thread::id retirement_thread = intermediate_retired
        ? intermediate_destroyed.get()
        : std::thread::id{};
    release_row_two_promise.set_value();

    const auto commit_deadline = std::chrono::steady_clock::now() + 2s;
    bool final_row_committed = false;
    while (std::chrono::steady_clock::now() < commit_deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot =
            Access::Session(*shell).CurrentSampleSnapshot();
        final_row_committed = snapshot && snapshot->collection.current_index == 2 &&
            Access::PendingLoadCount(*shell) == 0;
        if (final_row_committed) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SourceCollectionSessionView final_view = Access::Session(*shell).View();
    const bool no_load_error = Access::LoadError(*shell).empty();
    shell.reset();
    std::filesystem::remove(*annotation_path);
    std::filesystem::remove(path);

    Require(
        initial_failure_visible,
        "the retry fixture should retain its previous source failure");
    Require(navigation_queued && first_decode_started, "row 1 should enter the real drain worker");
    Require(annotation_changed, "the annotation context should change while row 1 is decoded");
    Require(row_two_requeued, "draining row 1 should enqueue the reconciled row 2 follow-up");
    Require(
        failure_retained_while_requeued,
        "an intermediate loaded result must not clear the source failure before its follow-up succeeds");
    Require(
        snapshot_while_requeued && snapshot_while_requeued->collection.current_index == 0,
        "the reentrant row 2 load must keep the complete row 0 snapshot visible");
    Require(
        view_while_requeued.filter.evaluation.included_count == 2 &&
            view_while_requeued.navigation.sequence_count == 2 &&
            view_while_requeued.navigation.current_sample_in_filter,
        "the real drain must keep the old context projections visible while row 2 loads");
    Require(
        intermediate_retired && retirement_thread != caller_thread,
        "the rejected row 1 prepared snapshot should retire off the drain caller thread");
    Require(final_row_committed, "the reentrant row 2 completion should commit through the real drain");
    Require(
        final_view.filter.evaluation.included_count == 1 &&
            final_view.navigation.sequence_count == 1 &&
            final_view.navigation.current_index == 2,
        "the final drain should publish the changed context and row 2 snapshot together");
    Require(no_load_error, "reentrant reconciliation should not publish a load error");
}

void TestDeferredRestoreCompletionPreservesUnrelatedNavigationTicket()
{
    const std::filesystem::path path = UniqueTempPath("_late.csv");
    const std::filesystem::path other_path = UniqueTempPath("_other.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "late completion fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionSession session =
        MakePreparedDeferredSession(path);
    const specforge::SpectrumSnapshotHandle
        initial_snapshot = session.CurrentSampleSnapshot();

    const specforge::SpectrumSnapshotHandle other_snapshot = MakeSnapshot(other_path, 0);
    specforge::SourceCollectionContext other_context;
    other_context.identity = {"shell-other", "other", "other-source", "other-context", 3};
    other_context.manifest.sample_names = {"one", "two", "three"};
    specforge::PreparedSampleWorkflowState other_workflow =
        specforge::PrepareSampleWorkflowState(
            *other_snapshot,
            other_context,
            0,
            {{}, {}});
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    specforge::SourceCollectionActivationTransaction activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    std::promise<void> completion_ready_promise;
    std::shared_future<void> completion_ready =
        completion_ready_promise.get_future().share();
    specforge::ShellUiTestAccess::
        RegisterCompletionReadyCallback(
            activation,
            [&completion_ready_promise]() {
                completion_ready_promise.set_value();
            });

    const specforge::SourceCollectionSessionResult
        navigation = activation.Submit(
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    Require(
        navigation.follow_up_spectrum_index == 1,
        "source A should issue the async row 1 follow-up used by the Shell ticket");

    const bool completion_published =
        completion_ready.wait_for(2s) ==
        std::future_status::ready;
    const specforge::SourceCollectionSessionResult
        switched_to_b = session.OpenPreparedSource(
            other_path,
            0,
            other_snapshot,
            std::move(other_context),
            std::move(other_workflow));
    const bool session_navigation_preserved =
        switched_to_b.loaded &&
        !switched_to_b.canceled_source_follow_up_path;
    const bool lifecycle_ticket_preserved =
        specforge::ShellUiTestAccess::
            PendingLoadCount(activation) == 1;

    const specforge::SourceCollectionSessionResult
        switched_back_to_a = activation.Submit(
            specforge::SourceCollectionSessionIntent::
                EditSourceCollection(
                    specforge::SourceCollectionIntent::
                        SwitchActive(0)));
    const bool pending_intent_restored =
        switched_back_to_a.follow_up_spectrum_index == 1 &&
        session.CurrentSampleSnapshot() ==
            initial_snapshot;
    const bool existing_ticket_reused =
        specforge::ShellUiTestAccess::
            PendingLoadCount(activation) == 1;
    bool row_one_committed = false;
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)activation.Drain(false);
        const auto snapshot =
            session.CurrentSampleSnapshot();
        row_one_committed =
            snapshot &&
            snapshot->collection.current_index == 1 &&
            !activation.status().loading;
        if (row_one_committed) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    std::filesystem::remove(path);

    Require(
        session_navigation_preserved,
        "source B's deferred completion must not cancel source A's navigation intent");
    Require(
        completion_published &&
            lifecycle_ticket_preserved,
        "source B's completion must retain source A's lifecycle ticket");
    Require(
        pending_intent_restored,
        "switching back to A should expose its still-pending row 1 intent");
    Require(
        existing_ticket_reused,
        "restoring A should reuse its existing row 1 Shell ticket instead of restarting the worker");
    Require(
        row_one_committed,
        "source A should finish the user-requested row 1 navigation");
}

void TestDeferredRestoreFollowUpFailureClearsPendingAndAllowsRetry()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_failed_restore.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "deferred restore failure fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path&,
           std::size_t,
           const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("expected deferred restore follow-up failure");
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));

    const specforge::SourceCollectionSessionResult navigation = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool deferred_follow_up_created = navigation.follow_up_spectrum_index == 1;

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool failure_drained = false;
    while (std::chrono::steady_clock::now() < deadline) {
        Access::Drain(*shell);
        failure_drained = !Access::LoadError(*shell).empty() &&
            Access::PendingLoadCount(*shell) == 0;
        if (failure_drained) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SpectrumSnapshotHandle retained_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    const specforge::SourceCollectionSessionResult retry = Access::Session(*shell).Submit(
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool retry_created = retry.follow_up_spectrum_index == 1;
    const bool retry_canceled =
        Access::Session(*shell).CancelPendingSampleNavigation(path, 1);
    shell.reset();
    std::filesystem::remove(path);

    Require(
        deferred_follow_up_created,
        "restore navigation should produce a deferred row 1 follow-up");
    Require(
        failure_drained,
        "production DrainSourceLoads should consume the failed DeferredRestore ticket");
    Require(
        retained_snapshot && retained_snapshot->collection.current_index == 0,
        "a failed restore follow-up should retain the complete committed row 0 snapshot");
    Require(
        retry_created && retry_canceled,
        "retrying the failed row must create a new worker follow-up instead of being deduplicated");
}

void TestDeferredRestorePreservesSavedActiveSourceAfterLaterCompletion()
{
    using Access = specforge::ShellUiTestAccess;
    const std::array<std::filesystem::path, 3> source_paths{
        UniqueTempPath("_restore_a.csv"),
        UniqueTempPath("_restore_b.csv"),
        UniqueTempPath("_restore_c.csv"),
    };
    const SourceSessionCachePaths cache_paths{
        UniqueTempPath("_source_session.json"),
        UniqueTempPath("_navigation.json"),
        UniqueTempPath("_labeling.json"),
        UniqueTempPath("_workflow.json"),
    };
    const std::filesystem::path explicit_source_path =
        UniqueTempPath("_restore_explicit.csv");
    std::filesystem::remove(cache_paths.source_session);
    std::filesystem::remove(cache_paths.navigation);
    std::filesystem::remove(cache_paths.labeling);
    std::filesystem::remove(cache_paths.workflow);
    for (const std::filesystem::path& path : source_paths) {
        std::filesystem::remove(path);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "deferred restore source fixture should be created");
        stream << "fixture";
    }
    {
        std::ofstream stream(
            explicit_source_path,
            std::ios::binary | std::ios::trunc);
        Require(
            stream.good(),
            "explicit source fixture should be created");
        stream << "fixture";
    }

    {
        specforge::SourceCollectionSession saved_session =
            MakeCachedSession(cache_paths);
        for (const std::filesystem::path& path : source_paths) {
            OpenPreparedFixtureSource(saved_session, path);
        }
        (void)saved_session.Submit(
            specforge::SourceCollectionSessionIntent::EditSourceCollection(
                specforge::SourceCollectionIntent::SwitchActive(1)));
        const specforge::SourceCollectionSessionView saved_view =
            saved_session.View();
        Require(
            saved_view.current_source_index &&
                *saved_view.current_source_index == 1 &&
                specforge::SourcePathIdentityKey(
                    saved_view.sources[*saved_view.current_source_index].path) ==
                    specforge::SourcePathIdentityKey(source_paths[1]),
            "fixture should activate source B before saving");
        Require(
            saved_session.FlushStateCaches(),
            "source-session fixture should flush successfully");
    }

    const specforge::SourceCollectionSessionStateCache saved_cache =
        specforge::LoadSourceCollectionSessionStateCache(
            cache_paths.source_session)
            .cache;
    Require(
        saved_cache.sources.size() == 3 &&
            saved_cache.active_source_index &&
            *saved_cache.active_source_index == 1 &&
            specforge::SourcePathIdentityKey(
                saved_cache.sources[*saved_cache.active_source_index].path) ==
                specforge::SourcePathIdentityKey(source_paths[1]),
        "save phase should persist source B as the active source");

    std::promise<void> release_a_promise;
    std::shared_future<void> release_a =
        release_a_promise.get_future().share();
    std::promise<void> release_c_promise;
    std::shared_future<void> release_c =
        release_c_promise.get_future().share();
    std::array<std::promise<void>, 3> decoder_entered_promises;
    std::array<std::shared_future<void>, 3> decoder_entered{
        decoder_entered_promises[0].get_future().share(),
        decoder_entered_promises[1].get_future().share(),
        decoder_entered_promises[2].get_future().share(),
    };
    std::array<std::atomic_bool, 3> decoder_entered_once{};
    std::promise<void> source_b_decoded_promise;
    std::shared_future<void> source_b_decoded =
        source_b_decoded_promise.get_future().share();
    std::atomic_bool source_b_decoded_once = false;

    specforge::SourceCollectionPreparationAdapters dependencies =
        MakeFixtureLoadDependencies(cache_paths);
    dependencies.snapshot_loader =
        [&](const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            const auto match = std::find_if(
                source_paths.begin(),
                source_paths.end(),
                [&source](const std::filesystem::path& candidate) {
                    return specforge::SourcePathIdentityKey(candidate) ==
                           specforge::SourcePathIdentityKey(source);
                });
            Require(
                match != source_paths.end(),
                "deferred restore should only load isolated fixture sources");
            const std::size_t source_index =
                static_cast<std::size_t>(
                    std::distance(source_paths.begin(), match));
            if (!decoder_entered_once[source_index].exchange(
                    true,
                    std::memory_order_relaxed)) {
                decoder_entered_promises[source_index].set_value();
            }
            if (source_index == 0) {
                WaitForRelease(
                    release_a,
                    canceled,
                    "source A decoder should be released after source B");
            } else if (source_index == 2) {
                WaitForRelease(
                    release_c,
                    canceled,
                    "source C decoder should be released last");
            }
            specforge::SpectrumSnapshotHandle snapshot =
                MakeSnapshot(source, index);
            if (source_index == 1 &&
                !source_b_decoded_once.exchange(
                    true,
                    std::memory_order_relaxed)) {
                source_b_decoded_promise.set_value();
            }
            return snapshot;
        };
    std::unique_ptr<specforge::ShellUi> shell =
        MakeDeferredShell(cache_paths, std::move(dependencies));

    const bool source_b_finished_first =
        decoder_entered[0].wait_for(2s) == std::future_status::ready &&
        decoder_entered[2].wait_for(2s) == std::future_status::ready &&
        source_b_decoded.wait_for(2s) == std::future_status::ready;
    if (!source_b_finished_first) {
        release_a_promise.set_value();
        release_c_promise.set_value();
    }
    Require(
        source_b_finished_first,
        "source B should decode while source A and C remain blocked");

    release_a_promise.set_value();
    const auto source_b_activation_deadline =
        std::chrono::steady_clock::now() + 2s;
    bool source_b_activated = false;
    while (std::chrono::steady_clock::now() <
           source_b_activation_deadline) {
        Access::Drain(*shell);
        const specforge::SourceCollectionSessionView view =
            Access::Session(*shell).View();
        source_b_activated =
            view.sources.size() == 2 &&
            view.current_source_index &&
            specforge::SourcePathIdentityKey(
                view.sources[*view.current_source_index].path) ==
                specforge::SourcePathIdentityKey(source_paths[1]);
        if (source_b_activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    release_c_promise.set_value();
    Require(
        source_b_activated,
        "source B should be active before the later source C completion");

    const auto restore_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < restore_deadline) {
        Access::Drain(*shell);
        if (Access::PendingLoadCount(*shell) == 0) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SourceCollectionSessionView restored_view =
        Access::Session(*shell).View();
    Require(
        restored_view.sources.size() == 3 &&
            CurrentSourceMatches(
                Access::Session(*shell),
                source_paths[1]),
        "deferred restore should reactivate saved source B after source C completes");

    Require(
        Access::Session(*shell).FlushStateCaches(),
        "restored source-session cache should flush successfully");
    const specforge::SourceCollectionSessionStateCache flushed_cache =
        specforge::LoadSourceCollectionSessionStateCache(
            cache_paths.source_session)
            .cache;
    Require(
        flushed_cache.active_source_index &&
            *flushed_cache.active_source_index < flushed_cache.sources.size() &&
            specforge::SourcePathIdentityKey(
                flushed_cache.sources[*flushed_cache.active_source_index].path) ==
                specforge::SourcePathIdentityKey(source_paths[1]),
        "flush after deferred restore should retain source B as active");

    shell.reset();
    shell = MakeDeferredShell(
        cache_paths,
        MakeFixtureLoadDependencies(cache_paths));
    Require(
        DrainAllSourceLoads(*shell),
        "second deferred restore should finish");
    const specforge::SourceCollectionSessionView second_restored_view =
        Access::Session(*shell).View();
    Require(
        second_restored_view.sources.size() == 3 &&
            CurrentSourceMatches(
                Access::Session(*shell),
                source_paths[1]),
        "a second deferred Shell should restore source B from the flushed cache");
    shell.reset();

    shell = MakeDeferredShell(
        cache_paths,
        MakeFixtureLoadDependencies(cache_paths));
    shell->OpenSource(explicit_source_path);
    Require(
        DrainAllSourceLoads(*shell),
        "deferred restore with an explicit open should finish");
    Require(
        CurrentSourceMatches(
            Access::Session(*shell),
            explicit_source_path),
        "an explicit open during restore should override the saved source B intent");
    shell.reset();

    for (const std::filesystem::path& path : source_paths) {
        std::filesystem::remove(path);
    }
    std::filesystem::remove(explicit_source_path);
    std::filesystem::remove(cache_paths.source_session);
    std::filesystem::remove(cache_paths.navigation);
    std::filesystem::remove(cache_paths.labeling);
    std::filesystem::remove(cache_paths.workflow);
}

void TestIdlePrefetchIsConsumedBySecondForwardNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path =
        UniqueTempPath("_prefetch_consumed.csv");
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        stream << "fixture";
    }
    std::array<std::atomic_int, 3> decoder_calls{};
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&decoder_calls](
            const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            ++decoder_calls.at(index);
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            MakePreparedDeferredSession(path),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));
    Access::EnableNavigationTracing(*shell, 300);

    const specforge::SourceCollectionSessionResult first =
        Access::SubmitNavigation(
            *shell,
            specforge::SourceCollectionSessionIntent::
                UpdateSampleNavigation(
                    specforge::SampleNavigationIntent::Move(
                        specforge::
                            SampleNavigationRequest::Next())),
            specforge::NavigationLatencyInputKind::UiNext);
    Require(
        first.follow_up_spectrum_index == 1,
        "first next should queue raw row 1");
    const auto first_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           first_deadline) {
        Access::Drain(*shell);
        const auto snapshot =
            Access::Session(*shell)
                .CurrentSampleSnapshot();
        if (snapshot &&
            snapshot->collection.current_index == 1) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const auto first_reports =
        Access::CompleteFramePresentation(
            *shell,
            300);
    Require(
        first_reports.size() == 1 &&
            first_reports.front().cache_kind ==
                specforge::
                    NavigationSnapshotCacheKind::None,
        "first next should remain a foreground decode");

    std::vector<specforge::NavigationPrefetchReport>
        prefetch_reports;
    const auto prefetch_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           prefetch_deadline) {
        Access::Drain(*shell);
        auto reports =
            Access::TakePrefetchReports(*shell);
        prefetch_reports.insert(
            prefetch_reports.end(),
            reports.begin(),
            reports.end());
        if (std::any_of(
                prefetch_reports.begin(),
                prefetch_reports.end(),
                [](const auto& report) {
                    return report.outcome ==
                        specforge::
                            NavigationPrefetchOutcome::
                                Completed;
                })) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    Access::EnableNavigationTracing(*shell, 301);
    const specforge::SourceCollectionSessionResult second =
        Access::SubmitNavigation(
            *shell,
            specforge::SourceCollectionSessionIntent::
                UpdateSampleNavigation(
                    specforge::SampleNavigationIntent::Move(
                        specforge::
                            SampleNavigationRequest::Next())),
            specforge::NavigationLatencyInputKind::UiNext);
    Require(
        second.follow_up_spectrum_index == 2,
        "second next should queue raw row 2");
    const auto second_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           second_deadline) {
        Access::Drain(*shell);
        const auto snapshot =
            Access::Session(*shell)
                .CurrentSampleSnapshot();
        if (snapshot &&
            snapshot->collection.current_index == 2 &&
            Access::PendingLoadCount(*shell) == 0) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const auto second_reports =
        Access::CompleteFramePresentation(
            *shell,
            301);
    auto consumed_reports =
        Access::TakePrefetchReports(*shell);
    prefetch_reports.insert(
        prefetch_reports.end(),
        consumed_reports.begin(),
        consumed_reports.end());

    Require(
        second_reports.size() == 1 &&
            second_reports.front().cache_hit &&
            second_reports.front().cache_kind ==
                specforge::
                    NavigationSnapshotCacheKind::
                        Prefetch,
        "second same-direction navigation should identify a prefetch cache hit");
    Require(
        second_reports.front().attempts.size() == 1 &&
            second_reports.front().attempts[0]
                    .snapshot_load_finished_ns -
                second_reports.front().attempts[0]
                    .snapshot_load_started_ns <
                5'000'000,
        "prefetch consumption should keep demand-side decode near zero");
    Require(
        decoder_calls[1].load() == 1 &&
            decoder_calls[2].load() == 1,
        "demand must consume row 2 without invoking its decoder again");
    Require(
        std::any_of(
            prefetch_reports.begin(),
            prefetch_reports.end(),
            [](const auto& report) {
                return report.outcome ==
                    specforge::
                        NavigationPrefetchOutcome::Completed;
            }) &&
            std::any_of(
                prefetch_reports.begin(),
                prefetch_reports.end(),
                [](const auto& report) {
                    return report.outcome ==
                        specforge::
                            NavigationPrefetchOutcome::Consumed;
                }),
        "prefetch observability should record completed and consumed");
    shell.reset();
    std::filesystem::remove(path);
}

void TestPublishedPrefetchBecomesStaleAfterQueryInput()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path =
        UniqueTempPath("_prefetch_stale.csv");
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        stream << "fixture";
    }
    std::array<std::atomic_int, 3> decoder_calls{};
    std::promise<void> prefetch_entered_promise;
    std::shared_future<void> prefetch_entered =
        prefetch_entered_promise.get_future().share();
    std::promise<void> release_prefetch_promise;
    std::shared_future<void> release_prefetch =
        release_prefetch_promise.get_future().share();
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&decoder_calls,
         &prefetch_entered_promise,
         release_prefetch](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            ++decoder_calls.at(index);
            if (index == 2) {
                prefetch_entered_promise.set_value();
                WaitForRelease(
                    release_prefetch,
                    canceled,
                    "timed out waiting to release published prefetch");
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            MakePreparedDeferredSession(path),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));

    (void)Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::
                        SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const auto activation_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           activation_deadline) {
        Access::Drain(*shell);
        if (Access::PrefetchActive(*shell)) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    Require(
        prefetch_entered.wait_for(2s) ==
            std::future_status::ready,
        "prefetch worker should start before publication");
    std::promise<void> prefetch_ready_promise;
    std::shared_future<void> prefetch_ready =
        prefetch_ready_promise.get_future().share();
    std::atomic_bool prefetch_ready_signaled = false;
    shell->RegisterSourceLoadCompletionReadyCallback(
        [&prefetch_ready_promise,
         &prefetch_ready_signaled]() {
            if (!prefetch_ready_signaled.exchange(true)) {
                prefetch_ready_promise.set_value();
            }
        });
    release_prefetch_promise.set_value();
    Require(
        prefetch_ready.wait_for(2s) ==
            std::future_status::ready,
        "prefetch fixture should hold one already-published completion");

    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::
                    SetSampleNameQuery("gamma")));
    Access::Drain(*shell);
    const auto reports =
        Access::TakePrefetchReports(*shell);

    Require(
        std::any_of(
            reports.begin(),
            reports.end(),
            [](const auto& report) {
                return report.outcome ==
                    specforge::
                        NavigationPrefetchOutcome::Stale;
            }),
        "query input should invalidate an already-published old prefetch");
    Require(
        Access::Session(*shell)
                .CurrentSampleSnapshot()
                ->collection.current_index == 1,
        "stale prefetch must not activate or move the committed index");
    shell.reset();
    std::filesystem::remove(path);
}

void TestCanceledPrefetchReportsOnlyAfterWorkerExit()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path =
        UniqueTempPath("_prefetch_canceled.csv");
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        stream << "fixture";
    }
    std::promise<void> prefetch_entered_promise;
    std::shared_future<void> prefetch_entered =
        prefetch_entered_promise.get_future().share();
    std::promise<void> cancellation_observed_promise;
    std::shared_future<void> cancellation_observed =
        cancellation_observed_promise.get_future().share();
    std::promise<void> release_canceled_worker_promise;
    std::shared_future<void> release_canceled_worker =
        release_canceled_worker_promise.get_future().share();
    std::atomic_bool entered_once = false;
    std::atomic_int64_t decoder_returned_ns = 0;
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&](const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 2) {
                if (!entered_once.exchange(
                        true,
                        std::memory_order_relaxed)) {
                    prefetch_entered_promise.set_value();
                }
                while (!canceled()) {
                    std::this_thread::sleep_for(1ms);
                }
                cancellation_observed_promise.set_value();
                release_canceled_worker.wait();
                decoder_returned_ns.store(
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        specforge::
                            NavigationLatencyTrace::Now()
                                .time_since_epoch())
                        .count(),
                    std::memory_order_relaxed);
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            MakePreparedDeferredSession(path),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));

    (void)Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::
                        SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const auto activation_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           activation_deadline) {
        Access::Drain(*shell);
        if (prefetch_entered.wait_for(0ms) ==
            std::future_status::ready) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::
                    SetSampleNameQuery("gamma")));
    const bool cancel_seen =
        cancellation_observed.wait_for(2s) ==
        std::future_status::ready;
    std::vector<specforge::NavigationPrefetchReport>
        reports = Access::TakePrefetchReports(*shell);
    const bool canceled_reported_before_exit =
        std::any_of(
            reports.begin(),
            reports.end(),
            [](const auto& report) {
                return report.outcome ==
                    specforge::
                        NavigationPrefetchOutcome::
                            Canceled;
            });

    release_canceled_worker_promise.set_value();
    const auto terminal_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           terminal_deadline) {
        Access::Drain(*shell);
        auto drained =
            Access::TakePrefetchReports(*shell);
        reports.insert(
            reports.end(),
            drained.begin(),
            drained.end());
        if (std::any_of(
                reports.begin(),
                reports.end(),
                [](const auto& report) {
                    return report.outcome ==
                        specforge::
                            NavigationPrefetchOutcome::
                                Canceled;
                })) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const auto canceled_report = std::find_if(
        reports.begin(),
        reports.end(),
        [](const auto& report) {
            return report.outcome ==
                specforge::
                    NavigationPrefetchOutcome::Canceled;
        });
    const bool terminal_after_decoder =
        canceled_report != reports.end() &&
        std::chrono::duration_cast<
            std::chrono::nanoseconds>(
            canceled_report->terminal_at
                .time_since_epoch())
                .count() >=
            decoder_returned_ns.load(
                std::memory_order_relaxed);
    const bool cancellation_timestamps_ordered =
        canceled_report != reports.end() &&
        canceled_report->scheduled_at <=
            canceled_report->cancel_requested_at &&
        canceled_report->cancel_requested_at <=
            canceled_report->terminal_at;

    Require(
        cancel_seen,
        "the blocked prefetch worker should observe cancellation");
    Require(
        !canceled_reported_before_exit,
        "canceled must not be recorded at request time while the worker is still unwinding");
    Require(
        terminal_after_decoder &&
            cancellation_timestamps_ordered,
        "canceled should preserve schedule, request, and worker-terminal order");
    shell.reset();
    std::filesystem::remove(path);
}

void TestAutomationPresentedViewAdvancesOnlyAfterSuccessfulPresent()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path =
        UniqueTempPath(
            "_automation_presented_view.csv");
    {
        std::ofstream stream(
            path,
            std::ios::binary |
                std::ios::trunc);
        Require(
            stream.good(),
            "automation presented-view fixture should be created");
        stream << "fixture";
    }

    auto dependencies =
        MakeFixtureLoadDependencies(
            {{}, {}, {}, {}});
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            MakePreparedDeferredSession(path),
            specforge::
                MakeSourceCollectionLoadQueueForTesting(
                    std::move(dependencies)));
    (void)Access::Submit(
        *shell,
        specforge::
            SourceCollectionSessionIntent::
                ChangeActiveSampleWorkflow(
                    specforge::
                        ActiveSampleWorkflowIntent::
                            StartOrResumeTemporaryLabelingTask()));
    (void)Access::Submit(
        *shell,
        specforge::
            SourceCollectionSessionIntent::
                ChangeActiveSampleWorkflow(
                    specforge::
                        ActiveSampleWorkflowIntent::
                            UpsertActiveLabel(
                                specforge::
                                    SampleLabelDefinition{
                                        7,
                                        "presented",
                                        'p'})));
    (void)Access::Submit(
        *shell,
        specforge::
            SourceCollectionSessionIntent::
                ChangeActiveSampleWorkflow(
                    specforge::
                        ActiveSampleWorkflowIntent::
                            AssignActiveLabelToCurrentSample(
                                7)));

    Access::EnableNavigationTracing(
        *shell,
        401);
    Access::SubmitSpectrumDraw(
        *shell,
        401,
        Access::Session(*shell).
            CurrentSampleSnapshot(),
        17);
    const specforge::NavigationLatencyPresentation
        first_presentation{
            17,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        401,
        std::span(&first_presentation, 1));
    const specforge::ShellAutomationView
        first_presented =
            shell->PresentedAutomationView();

    const specforge::ShellAutomationNavigationResult
        navigation =
            shell->GotoSpectrumForAutomation(
                1,
                std::nullopt);
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           deadline) {
        Access::Drain(*shell);
        const auto snapshot =
            Access::Session(*shell).
                CurrentSampleSnapshot();
        if (snapshot &&
            snapshot->collection.current_index ==
                1 &&
            Access::PendingLoadCount(*shell) == 0) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const specforge::ShellAutomationView live_after_drain =
        shell->AutomationView();
    const specforge::ShellAutomationView
        presented_before_retry =
            shell->PresentedAutomationView();

    Access::EnableNavigationTracing(
        *shell,
        402);
    Access::SubmitSpectrumDraw(
        *shell,
        402,
        Access::Session(*shell).
            CurrentSampleSnapshot(),
        17);
    shell->PresentFrame(402, {});
    const specforge::ShellAutomationView
        presented_after_retry =
            shell->PresentedAutomationView();
    const specforge::NavigationLatencyPresentation
        wrong_viewport{
            18,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        402,
        std::span(&wrong_viewport, 1));
    const specforge::ShellAutomationView
        presented_after_wrong_viewport =
            shell->PresentedAutomationView();
    const specforge::NavigationLatencyPresentation
        latest_presentation{
            17,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        402,
        std::span(&latest_presentation, 1));
    const specforge::ShellAutomationView
        latest_presented =
            shell->PresentedAutomationView();

    shell.reset();
    std::filesystem::remove(path);
    Require(
        navigation.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            first_presented.spectrum.present &&
            first_presented.spectrum.index == 0 &&
            first_presented.labeling
                    .has_active_task &&
            first_presented.labeling
                    .current_spectrum_code ==
                7,
        "the first successful Present should establish source, row, task, and label projection");
    Require(
        live_after_drain.spectrum.present &&
            live_after_drain.spectrum.index == 1 &&
            live_after_drain.labeling
                    .current_spectrum_code !=
                7 &&
            presented_before_retry.spectrum.index ==
                0,
        "maintenance may advance the live Session without changing the presented automation projection");
    Require(
        presented_after_retry.spectrum.index ==
                0 &&
            presented_after_retry.labeling
                    .current_spectrum_code ==
                7 &&
            presented_after_wrong_viewport
                    .spectrum.index ==
                0,
        "PresentRetry and a successful Present from the wrong viewport must retain the prior projection");
    Require(
        latest_presented.source_id ==
                live_after_drain.source_id &&
            latest_presented.spectrum.present &&
            latest_presented.spectrum.index == 1 &&
            latest_presented.labeling
                    .has_active_task &&
            latest_presented.labeling
                    .current_spectrum_code ==
                live_after_drain.labeling
                    .current_spectrum_code,
        "only the exact successful Present should publish the new source, spectrum, task, and code projection");
}

void TestShellFlushResultNamesEveryFailedOwner()
{
    specforge::ShellLocalStateFlushResult result;
    Require(
        result.all_saved() &&
            result.FailureMessage().empty(),
        "a complete shell flush should not produce a warning");

    result.application_settings_saved = false;
    result.source_collection.navigation_saved = false;
    result.source_collection.workflow_saved = false;
    result.spectral_lines_saved = false;
    const std::string message = result.FailureMessage();
    Require(
        !result.all_saved(),
        "any failed owner should make the shell flush incomplete");
    Require(
        message.find("Application settings") !=
                std::string::npos &&
            message.find("Sample navigation") !=
                std::string::npos &&
            message.find("Sample workflow") !=
                std::string::npos &&
            message.find("Spectral-line state") !=
                std::string::npos,
        "the shutdown warning should name every failed owner");
    Require(
        message.find("Source session") ==
                std::string::npos &&
            message.find("Sample labeling") ==
                std::string::npos,
        "the shutdown warning should omit successful owners");

    const std::string chinese_message =
        result.FailureMessage(
            specforge::UiLanguage::SimplifiedChinese);
    Require(
        chinese_message.find("应用设置") !=
                std::string::npos &&
            chinese_message.find("样本导航") !=
                std::string::npos &&
            chinese_message.find("样本工作流") !=
                std::string::npos &&
            chinese_message.find("谱线状态") !=
                std::string::npos,
        "the Chinese shutdown warning should name every failed owner");
    Require(
        chinese_message.find("源会话") ==
                std::string::npos &&
            chinese_message.find("样本标注") ==
                std::string::npos,
        "the Chinese shutdown warning should omit successful owners");
}

}  // namespace

int main()
{
    try {
        TestAutomationGotoAndTargetedLabelNavigationRespectActiveSequence();
        TestExplicitOpenTracesAcceptedPathThroughFirstPresent();
        TestFailedExplicitOpenProducesTerminalSourceLoadReport();
        TestRealDrainCommitsOnlyTheLatestRapidNavigation();
        TestAcceptedNavigationUsesLatestMatchingRawKeyInput();
        TestGenericRowLocationDoesNotStartPreviousNextTrace();
        TestWorkflowAutoAdvanceStartsExplicitTrace();
        TestWarmUiAndKeyboardNavigationReuseSequenceStateAtFixedIndices();
        TestNewActivationSupersedesAnUnpresentedOlderTrace();
        TestPresentationWithoutSpectrumDrawDoesNotCompleteNavigation();
        TestAutomationPresentedViewAdvancesOnlyAfterSuccessfulPresent();
        TestSameFrameSourceSwitchSupersedesActivatedNavigation();
        TestPublishedStaleCompletionIsRejectedWithoutMutatingNewNavigation();
        TestRealDrainPreservesWorkflowChangesMadeWhileFullPlanWaits();
        TestRealDrainRequeuesReconciledTargetAndRetiresIntermediateSnapshotOffThread();
        TestDeferredRestoreCompletionPreservesUnrelatedNavigationTicket();
        TestDeferredRestoreFollowUpFailureClearsPendingAndAllowsRetry();
        TestDeferredRestorePreservesSavedActiveSourceAfterLaterCompletion();
        TestIdlePrefetchIsConsumedBySecondForwardNavigation();
        TestPublishedPrefetchBecomesStaleAfterQueryInput();
        TestCanceledPrefetchReportsOnlyAfterWorkerExit();
        TestShellFlushResultNamesEveryFailedOwner();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
