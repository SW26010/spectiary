#include "helpers/source_load_test_support.h"
#include "helpers/temporary_directory.h"
#include "domain/source_collection_manifest.h"
#include "profile/navigation_latency_trace.h"
#include "profile/profile_sink.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_activation_transaction.h"
#include "ui/source_collection_session_state_cache_io.h"
#include "ui/source_collection_load_queue_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace spectiary {

struct SourceCollectionActivationTransactionTestAccess {
    static void ObserveSnapshotChanges(
        SourceCollectionActivationTransaction& activation,
        std::function<void(SourceCollectionSnapshotChangeReason)> observer)
    {
        activation.BindPresentationLifecycle({}, std::move(observer));
    }

    static std::vector<NavigationLatencyReport>
    CompleteNavigationFrame(
        SourceCollectionActivationTransaction& activation,
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation>
            presentations)
    {
        return activation.
            CompleteNavigationFramePresentations(
                frame_index,
                presentations);
    }

    static std::vector<SourceLoadLatencyReport>
    CompleteSourceLoadFrame(
        SourceCollectionActivationTransaction& activation,
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation>
            presentations)
    {
        return activation.
            CompleteSourceLoadFramePresentations(
                frame_index,
                presentations);
    }

    static std::vector<NavigationPrefetchReport>
    TakePrefetchReports(
        SourceCollectionActivationTransaction& activation)
    {
        return activation.TakeNavigationPrefetchReports();
    }

    static std::optional<
        LocalUserStateSaveScheduler::TimePoint>
    NextMaintenanceDeadline(
        const SourceCollectionActivationTransaction&
            activation)
    {
        return activation.NextMaintenanceDeadline();
    }

    static std::size_t CompletedLoadCount(
        const SourceCollectionActivationTransaction&
            activation)
    {
        return activation.load_queue_
            .ActivitySnapshot()
            .completed_count;
    }

    static SourceCollectionLoadActivitySnapshot QueueActivity(const SourceCollectionActivationTransaction& activation)
    {
        return activation.load_queue_.ActivitySnapshot();
    }

    static bool PrefetchActive(
        const SourceCollectionActivationTransaction&
            activation)
    {
        return activation.PrefetchActive();
    }
};

}  // namespace spectiary

namespace {

using namespace std::chrono_literals;
using Activation =
    spectiary::SourceCollectionActivationTransaction;
using ActivationAccess =
    spectiary::
        SourceCollectionActivationTransactionTestAccess;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path UniqueTempPath(
    std::string_view suffix)
{
    static std::atomic_uint64_t next_id = 1;
    return std::filesystem::temp_directory_path() /
        ("spectiary_activation_lifecycle_" +
         std::to_string(next_id.fetch_add(1)) +
         std::string(suffix));
}

void WriteFixture(const std::filesystem::path& path)
{
    std::ofstream stream(
        path,
        std::ios::binary | std::ios::trunc);
    Require(
        stream.good(),
        "activation lifecycle fixture should be created");
    stream << "fixture";
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
}

spectiary::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    auto snapshot =
        std::make_shared<spectiary::SpectrumSnapshot>();
    snapshot->source.id = "activation-fixture";
    snapshot->source.display_name =
        "activation-fixture";
    snapshot->source.path = path;
    snapshot->collection.spectrum_count = 3;
    snapshot->collection.current_index =
        spectrum_index;
    snapshot->collection.can_move_previous =
        spectrum_index > 0;
    snapshot->collection.can_move_next =
        spectrum_index + 1 < 3;
    snapshot->capabilities.can_plot_current_spectrum =
        true;
    snapshot->capabilities.can_switch_spectrum = true;
    return snapshot;
}

spectiary::SourceCollectionSession MakePreparedSession(
    const std::filesystem::path& path,
    std::size_t spectrum_index = 0)
{
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    const spectiary::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(path, spectrum_index);
    spectiary::SourceCollectionContext context;
    context.identity =
        spectiary::BuildSourceCollectionIdentity(
            *snapshot,
            spectiary::
                CaptureSourceCollectionSingleFileState(path));
    context.manifest.sample_names = {
        "alpha",
        "beta",
        "gamma",
    };
    spectiary::PreparedSampleWorkflowState workflow =
        spectiary::PrepareSampleWorkflowState(
            *snapshot,
            context,
            spectrum_index,
            {{}, {}});
    Require(
        session
            .CommitPreparedOpen(spectiary::PreparedSourceCollection{
                .path = path,
                .spectrum_index = spectrum_index,
                .snapshot = snapshot,
                .payload = spectiary::PreparedSourceCollectionPlan{std::move(context), std::move(workflow)},
            })
            .loaded,
        "activation fixture should commit row zero");
    return session;
}

spectiary::SourceCollectionLoadDependencies
MakeDependencies(
    spectiary::SourceCollectionLoadDependencies::
        SnapshotLoader snapshot_loader)
{
    spectiary::SourceCollectionLoadDependencies dependencies;
    dependencies.workflow_cache_paths = spectiary::test_support::EmptyWorkflowCachePaths();
    dependencies.snapshot_loader =
        std::move(snapshot_loader);
    dependencies.workflow_cache_paths = spectiary::test_support::EmptyWorkflowCachePaths();
    return dependencies;
}

template <typename Predicate>
bool DrainUntil(
    Activation& activation,
    Predicate predicate,
    bool allow_prefetch = false)
{
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           deadline) {
        (void)activation.Drain(allow_prefetch);
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

void TestRestoredPositionalAnnotationsRequireSavedSourceIdentity()
{
    // Production parsing, cache codec, deferred activation, and save are all
    // exercised together, including the restart after a rejected attachment.
    for (const auto [legacy_identity, without_active] :
         {std::pair{false, false}, std::pair{true, false}, std::pair{false, true}}) {
        spectiary::test_support::TemporaryDirectory temporary;
        const auto root = temporary.path();
        const auto folder = root / "spectra";
        std::filesystem::create_directory(folder);
        for (const auto* name : {"0_a.csv", "1_c.csv", "2_d.csv", "3_e.csv", "4_b.csv", "5_f.csv"}) {
            std::ofstream(folder / name) << "wav,flux\n5000,1\n5001,2\n5002,3\n";
        }
        const auto annotation = root / "oracle_y.npy";
        const std::vector<int> values{-1, 0, 0, 1, 0, 1};
        Require(spectiary::ExportLabelValuesToNpy(annotation, values), "annotation fixture should save");
        const auto cache_path = root / "sources.json";
        const auto navigation = root / "navigation.json";
        const auto labeling = root / "labeling.json";
        const auto workflow = root / "workflow.json";
        const spectiary::SampleWorkflowPreparationPaths paths{labeling, workflow, navigation};
        {
            spectiary::SourceCollectionSession session(cache_path, navigation, labeling, workflow);
            Activation activation(session, spectiary::SourceCollectionLoadQueue(paths));
            (void)activation.OpenSource(folder, 3);
            Require(DrainUntil(activation, [&] { return !activation.status().loading; }),
                "initial folder open should complete");
            (void)activation.Submit(spectiary::SourceCollectionSessionIntent::EditSourceCollection(
                spectiary::SourceCollectionIntent::AddReadOnlyAnnotationResult(annotation)));
            Require(session.View().navigation.current_annotations.size() == 1 &&
                    session.View().navigation.current_annotations[0].display_text == "1",
                "baseline sample should have its original annotation");
            Require(session.FlushStateCaches(), "initial attachment should persist");
        }
        auto saved = spectiary::LoadSourceCollectionSessionStateCache({}, cache_path).cache;
        Require(saved.sources.size() == 1 && saved.sources[0].source_identity,
            "persisted attachments must carry source-only identity");
        const auto original_identity = saved.sources[0].source_identity;
        // Changing annotation bytes alone must not invalidate source identity.
        Require(spectiary::ExportLabelValuesToNpy(annotation, std::vector<int>{0, 0, 0, 1, 0, 1}),
            "annotation should be replaceable without changing the source generation");
        {
            spectiary::SourceCollectionSession session(cache_path, navigation, labeling, workflow);
            Activation activation(session, spectiary::SourceCollectionLoadQueue(paths));
            activation.BeginDeferredRestore();
            Require(DrainUntil(activation, [&] { return !activation.status().loading; }),
                "unchanged source restore should complete");
            Require(session.View().navigation.current_annotations.size() == 1,
                "matching source identity should restore positional annotations");
            Require(session.FlushStateCaches(), "unchanged restore should save");
        }
        if (legacy_identity) {
            saved.sources[0].source_identity.reset();
            Require(spectiary::SaveSourceCollectionSessionStateCache({}, cache_path, saved),
                "unknown identity fixture should save");
        } else {
            std::filesystem::rename(folder / "4_b.csv", folder / "0_b.csv");
        }
        for (int restart = 0; restart < 2; ++restart) {
            spectiary::SourceCollectionSession session(cache_path, navigation, labeling, workflow,
                spectiary::SampleLabelingStateCacheLoadPolicy::AllowPersistentOutputs, {},
                without_active ? spectiary::SourceSessionStartupPolicy::RestoreRosterWithoutActive
                               : spectiary::SourceSessionStartupPolicy::RestoreSavedActive);
            Activation activation(session, spectiary::SourceCollectionLoadQueue(paths));
            activation.BeginDeferredRestore();
            Require(DrainUntil(activation, [&] { return !activation.status().loading; }),
                "guarded restore should complete");
            const auto view = session.View();
            Require(view.sources.size() == 1 && view.navigation.has_active_source == !without_active &&
                    view.navigation.current_annotations.empty(),
                "unsafe attachment must not obscure the source or silently reattach on a later restart");
            if (restart == 0 && !without_active) {
                Require(std::ranges::any_of(view.navigation.annotation_diagnostics, [&](const auto& diagnostic) {
                    return diagnostic.path == annotation && diagnostic.detail.find(
                        legacy_identity ? "identity is unavailable" : "identity changed") != std::string::npos;
                }), "rejected positional attachment must explain the identity failure");
            }
            Require(session.FlushStateCaches(), "guarded restore should persist");
            const auto persisted = spectiary::LoadSourceCollectionSessionStateCache({}, cache_path).cache;
            Require(persisted.sources.size() == 1 && persisted.sources[0].annotation_paths.empty(),
                "rejected attachment must leave the automatic restore roster");
            Require(persisted.active_source_index == 0,
                "automatic attachment repair must preserve the durable active source, including peer startup");
            if (!legacy_identity) {
                Require(persisted.sources[0].source_identity != original_identity,
                    "renaming a member should produce a new source identity");
            }
        }
    }
}

void TestDeferredRestoreKeepsAllThirtySixSourcesAndSavedActivation()
{
    spectiary::test_support::TemporaryDirectory temporary;
    const auto root = temporary.path();
    const auto cache_path = root / "sources.json";
    const auto navigation = root / "navigation.json";
    const auto labeling = root / "labeling.json";
    const auto workflow = root / "workflow.json";
    spectiary::SourceCollectionSessionStateCache cache;
    for (std::size_t index = 0; index < 36; ++index) {
        const auto source = root / (std::to_string(index) + ".csv");
        std::ofstream(source) << "wav,flux\n5000,1\n5001,2\n";
        cache.sources.push_back({source, 0, {}});
    }
    cache.active_source_index = 35;
    Require(spectiary::SaveSourceCollectionSessionStateCache({}, cache_path, cache),
        "36-source fixture should save");
    spectiary::SourceCollectionSession session(cache_path, navigation, labeling, workflow);
    Activation activation(session, spectiary::SourceCollectionLoadQueue(
        spectiary::SampleWorkflowPreparationPaths{labeling, workflow, navigation}));
    activation.BeginDeferredRestore();
    Require(DrainUntil(activation, [&] { return !activation.status().loading; }),
        "all 36 sources should finish restoring");
    const auto view = session.View();
    Require(view.sources.size() == 36 && view.current_source_index == 35 &&
            session.CurrentSampleSnapshot() &&
            session.CurrentSampleSnapshot()->source.path == cache.sources[35].path,
        "production deferred restore must preserve the complete roster and its last active source");
}

void TestStatusReportsCurrentLoadingSourcePath()
{
    const std::filesystem::path path =
        UniqueTempPath("_loading_status.csv");
    WriteFixture(path);

    std::promise<void> started_promise;
    std::shared_future<void> started =
        started_promise.get_future().share();
    std::promise<void> release_promise;
    std::shared_future<void> release =
        release_promise.get_future().share();
    auto dependencies = MakeDependencies(
        [&](const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            started_promise.set_value();
            release.wait();
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session(
        {}, {}, {}, {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    (void)activation.OpenSource(path, 0);
    const bool worker_started =
        started.wait_for(2s) ==
        std::future_status::ready;
    const Activation::Status loading =
        activation.status();
    release_promise.set_value();
    const bool drained = DrainUntil(
        activation,
        [&]() {
            return !activation.status().loading;
        });

    std::filesystem::remove(path);
    Require(
        worker_started && loading.loading &&
            loading.loading_source_path == path &&
            drained,
        "loading status should expose the current activation ticket path");
}

void TestRapidNavigationPublishesOnlyLatestIntent()
{
    const std::filesystem::path path =
        UniqueTempPath("_latest.csv");
    WriteFixture(path);

    std::promise<void> first_entered_promise;
    std::shared_future<void> first_entered =
        first_entered_promise.get_future().share();
    std::promise<void> release_first_promise;
    std::shared_future<void> release_first =
        release_first_promise.get_future().share();
    std::atomic_bool first_started = false;
    auto dependencies = MakeDependencies(
        [&first_entered_promise,
         release_first,
         &first_started](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 1 &&
                !first_started.exchange(true)) {
                first_entered_promise.set_value();
                while (release_first.wait_for(2ms) !=
                       std::future_status::ready) {
                    if (canceled()) {
                        break;
                    }
                }
            }
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session =
        MakePreparedSession(path);
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    const auto first = activation.Submit(
        spectiary::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                spectiary::SampleNavigationIntent::Move(
                    spectiary::
                        SampleNavigationRequest::Next())));
    Require(
        first.follow_up_spectrum_index() == 1,
        "first navigation should target row one");
    Require(
        first_entered.wait_for(2s) ==
            std::future_status::ready,
        "first worker should start");

    const auto second = activation.Submit(
        spectiary::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                spectiary::SampleNavigationIntent::Move(
                    spectiary::
                        SampleNavigationRequest::Next())));
    release_first_promise.set_value();
    const bool latest_activated = DrainUntil(
        activation,
        [&]() {
            const auto snapshot =
                session.CurrentSampleSnapshot();
            return snapshot &&
                snapshot->collection.current_index == 2 &&
                !activation.status().loading;
        });

    std::filesystem::remove(path);
    Require(
        second.follow_up_spectrum_index() == 2 && second.follow_up_source_path() == path &&
            second.canceled_source_follow_up_path == path,
        "second navigation should resolve from the pending row");
    Require(
        latest_activated,
        "only the latest navigation should commit");
}

void TestFailedExplicitOpenProducesTerminalLifecycleResult()
{
    const std::filesystem::path path =
        UniqueTempPath("_failed.csv");
    const std::filesystem::path profile_path =
        UniqueTempPath("_failed.jsonl");
    WriteFixture(path);
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    auto dependencies = MakeDependencies(
        [](const std::filesystem::path&,
           std::size_t,
           const auto&)
            -> spectiary::SpectrumSnapshotHandle {
            throw std::runtime_error(
                "expected activation failure");
        });
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    spectiary::ProfileSink profile(profile_path);
    profile.BeginFrame();
    activation.BeginFrame(true, 11, &profile);
    const Activation::SourceOpenOperation operation =
        activation.OpenSourceForAutomation(path, 0);

    const bool failure_drained = DrainUntil(
        activation,
        [&]() {
            return !activation.status().error_message.empty() &&
                !activation.status().loading;
        });
    const std::string error(
        activation.status().error_message);
    const Activation::Status status =
        activation.status();
    const auto automation_outcome =
        activation.ObserveSourceOpenOperation(
            operation);
    const bool structured_failure =
        status.failures.size() == 1 &&
        status.failures.front().source_path ==
            path &&
        status.failures.front().error.kind ==
            spectiary::
                SourceCollectionLoadErrorKind::
                    BackgroundLoadingFailed &&
        status.failures.front()
                .error.diagnostic_detail ==
            "expected activation failure";
    activation.PresentFrame(11, {});
    profile.Stop();
    const std::string profile_text =
        ReadText(profile_path);

    std::filesystem::remove(path);
    std::filesystem::remove(profile_path);
    Require(
        failure_drained,
        "failed load should become a visible lifecycle result");
    Require(
        error.find(path.string()) !=
            std::string::npos,
        "a single failed load should identify its source path");
    Require(
        structured_failure,
        "activation status should preserve the semantic load error separately from its raw diagnostic");
    Require(
        status.loading_source_path.empty(),
        "a failed load should clear its visible loading-source projection");
    Require(
        automation_outcome.state ==
                Activation::SourceOpenOperationState::
                    Failed &&
            automation_outcome.source_path == path &&
            automation_outcome.error.kind ==
                spectiary::SourceCollectionLoadErrorKind::
                    BackgroundLoadingFailed,
        "automation should receive the same terminal failure from the real source loader");
    Require(
        profile_text.find(
            "\"event\":\"source_load_latency\"") !=
                std::string::npos &&
            profile_text.find(
                "\"outcome\":\"failed\"") !=
                std::string::npos,
        "open -> prepare -> fail should publish its terminal trace through PresentFrame");
}

void TestActivationOwnsQueueServiceDeadline()
{
    const std::filesystem::path path =
        UniqueTempPath("_service_deadline.csv");
    WriteFixture(path);
    auto dependencies = MakeDependencies(
        [](const std::filesystem::path&,
           std::size_t,
           const auto&)
            -> spectiary::SpectrumSnapshotHandle {
            throw std::runtime_error(
                "synthetic service deadline failure");
        });
    spectiary::SourceCollectionSession session(
        {},
        {},
        {},
        {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    const auto before_open =
        spectiary::LocalUserStateSaveScheduler::
            Clock::now();
    (void)activation.OpenSource(path, 0);
    const auto scheduled =
        ActivationAccess::NextMaintenanceDeadline(
            activation);
    const auto after_open =
        spectiary::LocalUserStateSaveScheduler::
            Clock::now();
    const bool failure_drained = DrainUntil(
        activation,
        [&]() {
            return !activation.status().loading;
        });
    const auto after_drain =
        ActivationAccess::NextMaintenanceDeadline(
            activation);

    std::filesystem::remove(path);
    Require(
        scheduled &&
            *scheduled >= before_open &&
            *scheduled <= after_open,
        "enqueue should publish an immediate activation-owned service deadline");
    Require(
        failure_drained && !after_drain,
        "draining the terminal completion should clear the queue service deadline");
}

void TestLastWorkerIsReapedByScheduledServiceAfterCompletionDrain()
{
    const auto path = UniqueTempPath("_last_worker_service.csv");
    WriteFixture(path);
    std::promise<void> notified_promise, release_promise;
    auto notified = notified_promise.get_future();
    auto release = release_promise.get_future().share();
    auto queue = spectiary::MakeSourceCollectionLoadQueueForTesting(
        MakeDependencies([](const auto&, std::size_t, const auto&) -> spectiary::SpectrumSnapshotHandle {
            throw std::runtime_error("terminal fixture failure");
        }));
    // Hold RunTask inside its notification, after publishing the result but
    // before RunTaskLoop can mark the final worker finished.
    queue.RegisterCompletionReadyCallback([&]() {
        notified_promise.set_value();
        release.wait();
    });
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(session, std::move(queue));
    (void)activation.OpenSource(path, 0);
    const bool notification_received = notified.wait_for(2s) == std::future_status::ready;
    (void)activation.Drain(false);
    const auto drained = ActivationAccess::QueueActivity(activation);
    const auto scheduled = ActivationAccess::NextMaintenanceDeadline(activation);
    release_promise.set_value();
    Require(notification_received, "last completion must wake the UI");
    Require(drained.active_task_count == 0 && drained.completed_count == 0 && drained.worker_count == 1,
        "the completion must be drained while the final worker is still unwinding");
    Require(scheduled.has_value(), "worker teardown must retain a production service deadline after completion drain");

    const auto timeout = std::chrono::steady_clock::now() + 2s;
    while (ActivationAccess::QueueActivity(activation).worker_count != 0 &&
           std::chrono::steady_clock::now() < timeout) {
        const auto deadline = ActivationAccess::NextMaintenanceDeadline(activation);
        if (!deadline) break;
        // Drain only when the production scheduler requests service. There is
        // no unconditional polling drain and no new user operation.
        std::this_thread::sleep_until(*deadline);
        (void)activation.Drain(false);
    }
    Require(ActivationAccess::QueueActivity(activation).worker_count == 0 &&
        !ActivationAccess::NextMaintenanceDeadline(activation),
        "scheduled service must reap the final worker and then become idle");
    std::filesystem::remove(path);
}

void TestSuccessfulSourceDoesNotHideConcurrentFailure()
{
    const std::filesystem::path failed_path =
        UniqueTempPath("_failed_a.csv");
    const std::filesystem::path loaded_path =
        UniqueTempPath("_loaded_b.csv");
    WriteFixture(failed_path);
    WriteFixture(loaded_path);
    auto dependencies = MakeDependencies(
        [failed_path](
            const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            if (source == failed_path) {
                throw std::runtime_error(
                    "source A failed");
            }
            return MakeSnapshot(source, index);
        });
    auto session = MakePreparedSession(loaded_path);
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    bool successful_display_cleared = false;
    ActivationAccess::ObserveSnapshotChanges(activation, [&](auto) {
        successful_display_cleared |= !session.CurrentSampleSnapshot();
    });

    (void)activation.OpenSource(failed_path, 0);
    (void)activation.OpenSource(loaded_path, 0);
    const bool drained = DrainUntil(
        activation,
        [&]() {
            return !activation.status().loading;
        });
    const std::string error(
        activation.status().error_message);

    std::filesystem::remove(failed_path);
    std::filesystem::remove(loaded_path);
    Require(
        drained,
        "concurrent source loads should drain");
    Require(
        error.find("source A failed") !=
            std::string::npos,
        "a successful source must not hide another source's failure");
    Require(!successful_display_cleared && session.CurrentSampleSnapshot() &&
            session.CurrentSampleSnapshot()->source.path == loaded_path && !session.CurrentSampleFailure(),
        "a late background failure must not clear the current successful sample");
}

void TestConcurrentFailuresRemainVisible()
{
    const std::filesystem::path first_path =
        UniqueTempPath("_failed_first.csv");
    const std::filesystem::path second_path =
        UniqueTempPath("_failed_second.csv");
    WriteFixture(first_path);
    WriteFixture(second_path);
    auto dependencies = MakeDependencies(
        [first_path](
            const std::filesystem::path& source,
            std::size_t,
            const auto&)
            -> spectiary::SpectrumSnapshotHandle {
            if (source == first_path) {
                throw std::runtime_error(
                    "first source failed");
            }
            throw std::runtime_error(
                "second source failed");
        });
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    (void)activation.OpenSource(first_path, 0);
    (void)activation.OpenSource(second_path, 0);
    const bool drained = DrainUntil(
        activation,
        [&]() {
            return !activation.status().loading;
        });
    const std::string error(
        activation.status().error_message);

    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
    Require(
        drained,
        "concurrent failed source loads should drain");
    Require(
        error.find("first source failed") !=
                std::string::npos &&
            error.find("second source failed") !=
                std::string::npos,
        "all current source failures should remain visible");
    Require(session.CurrentSampleFailure() && session.CurrentSampleFailure()->source_path == second_path,
        "background failures must not replace the current request's Information diagnostic");
}

void TestFailedMemberClearsSampleButPreservesWorkflow()
{
    using namespace spectiary;
    const auto path = UniqueTempPath("_member_failure.csv");
    WriteFixture(path);
    auto session = MakePreparedSession(path);
    const auto retained = session.CurrentSourceSnapshot();
    auto dependencies = MakeDependencies([](const auto& source, std::size_t index, const auto&) {
        if (index == 1) throw std::runtime_error("member one cannot be decoded");
        return MakeSnapshot(source, index);
    });
    Activation activation(session, MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    (void)activation.Submit(SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        ActiveSampleWorkflowIntent::StartOrResumeTemporaryLabelingTask()));
    (void)activation.Submit(SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        ActiveSampleWorkflowIntent::UpsertActiveLabel({1, "one", 'o'})));
    (void)activation.Submit(SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(false)));
    const auto initial_write = activation.Submit(SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(1)));
    Require(initial_write.label_write && initial_write.label_write->write.changed,
        "fixture must contain a real label on the previously displayed sample");
    const auto task_id = session.View().labeling.task_id;
    (void)activation.Submit(SourceCollectionSessionIntent::UpdateSampleNavigation(
        SampleNavigationIntent::Move(SampleNavigationRequest::Next())));
    Require(DrainUntil(activation, [&] { return !activation.status().loading; }),
        "member failure should finish");
    const auto& failed = session.View();
    Require(!session.CurrentSampleSnapshot() && !failed.current_sample_snapshot &&
            session.CurrentSourceSnapshot() == retained && failed.navigation.sample_count == 3 &&
            !failed.navigation.current_index && failed.navigation.current_annotations.empty() &&
            !failed.labeling.current_index && failed.labeling.task_id == task_id &&
            failed.labeling.labeled_count == 1 && failed.current_sample_failure &&
            failed.current_sample_failure->sample_index == 1,
        "member failure must preserve collection and task but expose no previous sample");
    const auto write = activation.Submit(SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(1)));
    const auto clear = activation.Submit(SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
        ActiveSampleWorkflowIntent::ClearActiveLabelForCurrentSample()));
    Require(!write.label_write && !clear.label_write && session.View().labeling.labeled_count == 1,
        "commands must not label the retained sample while its presentation is failed");
    activation.AcknowledgeLoadFailures();
    (void)activation.Drain(false);
    Require(!session.CurrentSampleSnapshot(), "dismiss and maintenance must not restore the previous sample");
    (void)activation.Submit(SourceCollectionSessionIntent::UpdateSampleNavigation(
        SampleNavigationIntent::Move(SampleNavigationRequest::LocateRow(99))));
    Require(!session.CurrentSampleSnapshot(), "an invalid selection must not restore the previous sample");
    (void)activation.Submit(SourceCollectionSessionIntent::UpdateSampleNavigation(
        SampleNavigationIntent::Move(SampleNavigationRequest::LocateRow(2))));
    Require(DrainUntil(activation, [&] { return !activation.status().loading; }) &&
            session.CurrentSampleSnapshot() && session.CurrentSampleSnapshot()->collection.current_index == 2 &&
            session.View().labeling.task_id == task_id,
        "explicit valid sample selection must restore presentation in the same workflow");
    std::filesystem::remove(path);
}

void TestAcknowledgedFailuresStayTerminalAndNewGenerationReappears()
{
    const std::filesystem::path first_path =
        UniqueTempPath("_acknowledged_first.csv");
    const std::filesystem::path second_path =
        UniqueTempPath("_acknowledged_second.csv");
    WriteFixture(first_path);
    WriteFixture(second_path);
    std::atomic_uint32_t first_attempts = 0;
    auto dependencies = MakeDependencies(
        [first_path, &first_attempts](
            const std::filesystem::path& source,
            std::size_t,
            const auto&)
            -> spectiary::SpectrumSnapshotHandle {
            if (source == first_path) {
                if (first_attempts.fetch_add(1) == 0) {
                    throw std::runtime_error(
                        "first acknowledged failure");
                }
                throw std::runtime_error(
                    "new generation failure");
            }
            throw std::runtime_error(
                "second acknowledged failure");
        });
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    activation.BeginFrame(true, 13, nullptr);

    (void)activation.OpenSource(first_path, 0);
    (void)activation.OpenSource(second_path, 0);
    const bool failures_drained = DrainUntil(
        activation,
        [&]() {
            return !activation.status().loading;
        });
    activation.AcknowledgeLoadFailures();
    const bool failures_hidden =
        activation.status().error_message.empty();
    const auto terminal_reports =
        ActivationAccess::CompleteSourceLoadFrame(
            activation,
            13,
            {});

    (void)activation.OpenSource(first_path, 0);
    const bool new_failure_drained = DrainUntil(
        activation,
        [&]() {
            return !activation.status().loading;
        });
    const std::string new_error(
        activation.status().error_message);

    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
    Require(
        failures_drained,
        "concurrent failures should drain before acknowledgment");
    Require(
        failures_hidden,
        "one acknowledgment should hide all current failure projections");
    Require(
        terminal_reports.size() == 2 &&
            std::ranges::all_of(
                terminal_reports,
                [](const auto& report) {
                    return report.outcome ==
                        spectiary::SourceLoadLatencyOutcome::
                            Failed;
                }),
        "acknowledgment must not delete terminal failure outcomes");
    Require(
        new_failure_drained &&
            new_error.find("new generation failure") !=
                std::string::npos,
        "a later failed generation should become visible again");
    Require(
        new_error.find("second acknowledged failure") ==
            std::string::npos,
        "a new failure must not restore another source's acknowledged generation");
}

void TestSuccessfulRetryClearsPreviousFailures()
{
    const std::filesystem::path retry_path =
        UniqueTempPath("_retry.csv");
    const std::filesystem::path other_path =
        UniqueTempPath("_other_failed.csv");
    WriteFixture(retry_path);
    WriteFixture(other_path);
    std::atomic_uint32_t attempts = 0;
    auto dependencies = MakeDependencies(
        [&attempts, retry_path](
            const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            if (source != retry_path) {
                throw std::runtime_error(
                    "other source failure");
            }
            if (attempts.fetch_add(1) == 0) {
                throw std::runtime_error(
                    "retryable source failure");
            }
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    (void)activation.OpenSource(retry_path, 0);
    (void)activation.OpenSource(other_path, 0);
    const bool failure_drained = DrainUntil(
        activation,
        [&]() {
            const std::string_view error =
                activation.status().error_message;
            return error.find(
                       "retryable source failure") !=
                    std::string_view::npos &&
                error.find("other source failure") !=
                    std::string_view::npos &&
                !activation.status().loading;
        });
    (void)activation.OpenSource(retry_path, 0);
    const bool loading_without_old_failures =
        activation.status().loading &&
        activation.status().error_message.empty() &&
        activation.status().failures.empty();
    const bool retry_drained = DrainUntil(
        activation,
        [&]() {
            return activation.status().error_message.empty() &&
                activation.status().failures.empty() &&
                !activation.status().loading;
        });

    std::filesystem::remove(retry_path);
    std::filesystem::remove(other_path);
    Require(
        failure_drained,
        "the first source attempt should publish its failure");
    Require(
        loading_without_old_failures,
        "starting a retry should display loading without historical failures");
    Require(
        retry_drained,
        "a successful retry should leave no historical failure in the status bar");
    Require(session.CurrentSampleSnapshot() && !session.CurrentSampleFailure() &&
            session.CurrentSampleSnapshot()->source.path == retry_path,
        "successful explicit retry must restore presentation and clear the Information failure");
}

void TestLaterSourceOpenReplacesHistoricalFailureStatus()
{
    const auto failed_path = UniqueTempPath("_historical_failure.csv");
    const auto loaded_path = UniqueTempPath("_later_success.csv");
    WriteFixture(failed_path);
    WriteFixture(loaded_path);
    auto dependencies = MakeDependencies(
        [failed_path](const auto& source, std::size_t index, const auto&) {
            if (source == failed_path) {
                throw std::runtime_error("historical source failure");
            }
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));

    // All explicit-open entry points share the status lifecycle.
    for (int origin = 0; origin < 3; ++origin) {
        const auto failed_operation = activation.OpenSourceForAutomation(failed_path, 0);
        Require(DrainUntil(activation, [&]() { return !activation.status().loading; }),
            "failed open should complete before starting the next open");
        Require(activation.status().failures.size() == 1 &&
                !activation.status().error_message.empty() &&
                activation.ObserveSourceOpenOperation(failed_operation).state ==
                    Activation::SourceOpenOperationState::Failed,
            "a new failure should be visible and reported to automation");

        if (origin == 0) {
            (void)activation.OpenSource(loaded_path, 0);
        } else if (origin == 1) {
            (void)activation.OpenExternalSource(loaded_path, false, 0);
        } else {
            (void)activation.OpenSourceForAutomation(loaded_path, 0);
        }
        Require(activation.status().loading && activation.status().failures.empty() &&
                activation.status().error_message.empty(),
            "a later open should show loading instead of an old failure");
        Require(DrainUntil(activation, [&]() { return !activation.status().loading; }),
            "later successful open should complete");
        Require(activation.status().failures.empty() &&
                activation.status().error_message.empty() &&
                session.CurrentSampleSnapshot() &&
                session.CurrentSampleSnapshot()->source.path == loaded_path,
            "successful loading of another source should restore ready status");
    }
    std::filesystem::remove(failed_path);
    std::filesystem::remove(loaded_path);
}

void TestStartupFailuresFollowSavedActiveSourceAndRemainRemovable()
{
    for (const bool active_fails : {false, true}) {
        for (const bool failure_first : {false, true}) {
            const auto saved_path = UniqueTempPath("_startup_state.json");
            const auto navigation_path = UniqueTempPath("_startup_navigation.json");
            const auto labeling_path = UniqueTempPath("_startup_labeling.json");
            const auto workflow_path = UniqueTempPath("_startup_workflow.json");
            const auto good_path = UniqueTempPath("_startup_good.csv");
            const auto bad_path = UniqueTempPath("_startup_bad.csv");
            const auto annotation_path = UniqueTempPath("_startup_annotation.csv");
            WriteFixture(good_path);
            WriteFixture(bad_path);
            spectiary::SourceCollectionSessionStateCache saved;
            const spectiary::SourceCollectionSavedSource good{good_path, 0, {}};
            const spectiary::SourceCollectionSavedSource bad{bad_path, 0, {annotation_path}, "offline-generation"};
            saved.sources = failure_first ? std::vector{bad, good} : std::vector{good, bad};
            saved.active_source_index = active_fails == failure_first ? 0 : 1;
            Require(spectiary::SaveSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, saved_path, saved),
                "startup fixture should save");

            // Restart from the saved result as well: an unavailable active row
            // must survive shutdown without becoming a successful fallback.
            for (int restart = 0; restart < 2; ++restart) {
                auto dependencies = MakeDependencies(
                    [bad_path](const auto& source, std::size_t index, const auto&) {
                        if (source == bad_path) {
                            throw std::runtime_error("startup source unavailable");
                        }
                        return MakeSnapshot(source, index);
                    });
                spectiary::SourceCollectionSession session(saved_path, navigation_path,
                    labeling_path, workflow_path);
                Activation activation(session, spectiary::MakeSourceCollectionLoadQueueForTesting(
                    std::move(dependencies), {.foreground_limit = 1}));
                activation.BeginDeferredRestore();
                Require(DrainUntil(activation, [&]() { return !activation.status().loading; }),
                    "startup should finish in either completion order");
                const auto view = session.View();
                Require(view.sources.size() == 2 && view.current_source_index,
                    "failed and successful saved rows should both remain in Files");
                const auto bad_index = view.sources[0].path == bad_path ? 0U : 1U;
                const auto good_index = 1U - bad_index;
                Require(view.sources[bad_index].load_error &&
                        view.sources[bad_index].state == spectiary::SourceCollectionSourceState::Unavailable &&
                        view.sources[bad_index].load_error->diagnostic_detail.find("startup source unavailable") != std::string::npos,
                    "failed row must retain an unavailable state and its hover diagnostic");
                Require(view.sources[*view.current_source_index].path == (active_fails ? bad_path : good_path),
                    "startup must retain the saved active path even when it fails");
                Require(activation.status().failures.size() == (active_fails ? 1U : 0U),
                    "only failure of the saved active source should affect startup status");
                Require(active_fails ? (!view.snapshot && !view.current_sample_snapshot &&
                            !view.navigation.has_active_source && !view.labeling.has_active_source)
                        : (view.current_sample_snapshot && view.current_sample_snapshot->source.path == good_path),
                    "an unavailable active source must leave plot and sample workflow empty");
                Require(session.FlushStateCaches(), "startup state should flush");
                const auto persisted = spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, saved_path).cache;
                Require(persisted.sources.size() == 2 && persisted.active_source_index &&
                        persisted.sources[*persisted.active_source_index].path == (active_fails ? bad_path : good_path),
                    "shutdown must persist the same active source identity");
                const auto saved_bad = std::ranges::find_if(persisted.sources,
                    [&](const auto& source) { return source.path == bad_path; });
                Require(saved_bad != persisted.sources.end() && saved_bad->annotation_paths == std::vector{annotation_path},
                    "failed rows must retain saved annotation associations");
                Require(saved_bad->source_identity == bad.source_identity,
                    "failed restores must retain the attachment's original source identity");

                if (restart == 1) {
                    if (!active_fails || failure_first) {
                        (void)activation.Submit(spectiary::SourceCollectionSessionIntent::EditSourceCollection(
                            spectiary::SourceCollectionIntent::SwitchActive(good_index)));
                        (void)activation.Submit(spectiary::SourceCollectionSessionIntent::EditSourceCollection(
                            spectiary::SourceCollectionIntent::SwitchActive(bad_index)));
                        Require(session.CurrentSampleSnapshot() &&
                                session.CurrentSampleSnapshot()->source.path == good_path &&
                                activation.status().failures.empty(),
                            "unavailable rows cannot be selected and switching to a good row clears startup failure");
                    }
                    (void)activation.Submit(spectiary::SourceCollectionSessionIntent::EditSourceCollection(
                        spectiary::SourceCollectionIntent::Remove(bad_index)));
                    if (active_fails && !failure_first) {
                        Require(!session.CurrentSampleSnapshot(),
                            "removing a failed source must not implicitly restore an old sample");
                        (void)activation.Submit(spectiary::SourceCollectionSessionIntent::EditSourceCollection(
                            spectiary::SourceCollectionIntent::SwitchActive(0)));
                    }
                    Require(session.View().sources.size() == 1 && !session.HasUnresolvedSourceIntent(bad_path) &&
                            session.CurrentSampleSnapshot() &&
                            session.CurrentSampleSnapshot()->source.path == good_path &&
                            activation.status().failures.empty(),
                        "removing the failed row must forget its unresolved restore intent");
                    Require(session.FlushStateCaches(), "removal should flush");
                    const auto removed = spectiary::LoadSourceCollectionSessionStateCache(spectiary::RuntimePaths{}, saved_path).cache;
                    Require(removed.sources.size() == 1 && removed.sources.front().path == good_path,
                        "removed failures must not return at the next startup");
                }
            }
            std::filesystem::remove(saved_path);
            std::filesystem::remove(navigation_path);
            std::filesystem::remove(labeling_path);
            std::filesystem::remove(workflow_path);
            std::filesystem::remove(good_path);
            std::filesystem::remove(bad_path);
        }
    }
}

void TestCanceledGenerationDoesNotPublishFailure(bool replacement_fails)
{
    const std::filesystem::path path =
        UniqueTempPath("_canceled.csv");
    WriteFixture(path);
    std::promise<void> first_started_promise;
    std::shared_future<void> first_started =
        first_started_promise.get_future().share();
    std::atomic_uint32_t attempts = 0;
    auto dependencies = MakeDependencies(
        [&attempts, &first_started_promise, replacement_fails](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (attempts.fetch_add(1) == 0) {
                first_started_promise.set_value();
                while (!canceled()) {
                    std::this_thread::sleep_for(1ms);
                }
                throw std::runtime_error(
                    "canceled source failure");
            }
            if (replacement_fails) throw std::runtime_error("current request failed");
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    const Activation::SourceOpenOperation
        canceled_operation =
            activation.OpenSourceForAutomation(
                path,
                0);
    Require(
        first_started.wait_for(2s) ==
            std::future_status::ready,
        "the canceled source generation should start");
    (void)activation.OpenSource(path, 0);
    const bool retry_drained = DrainUntil(
        activation,
        [&]() {
            return !activation.status().loading;
        });
    const std::string error(
        activation.status().error_message);
    const auto canceled_outcome =
        activation.ObserveSourceOpenOperation(
            canceled_operation);

    std::filesystem::remove(path);
    Require(
        retry_drained,
        "the replacement source generation should drain");
    Require(
        error.find("canceled source failure") ==
            std::string::npos,
        "a canceled source generation must not publish a user error");
    Require(replacement_fails
            ? (!session.CurrentSampleSnapshot() && session.CurrentSampleFailure() &&
                session.CurrentSampleFailure()->error.diagnostic_detail == "current request failed")
            : (session.CurrentSampleSnapshot() && !session.CurrentSampleFailure()),
        "a canceled failure must change neither the replacement's presentation nor its error");
    Require(
        canceled_outcome.state ==
            Activation::SourceOpenOperationState::
                Canceled,
        "a superseded automation source generation should terminate as canceled");
}

void TestPresentationCompletesOnlyAfterExactSnapshotDraw()
{
    const std::filesystem::path path =
        UniqueTempPath("_present.csv");
    WriteFixture(path);
    auto dependencies = MakeDependencies(
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session =
        MakePreparedSession(path);
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    activation.BeginFrame(true, 17, nullptr);
    const auto navigation = activation.Submit(
        spectiary::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                spectiary::SampleNavigationIntent::Move(
                    spectiary::
                        SampleNavigationRequest::Next())),
        Activation::NavigationIntent{
            spectiary::NavigationLatencyInputKind::UiNext,
            spectiary::NavigationLatencyTrace::Now()});
    const bool activated = DrainUntil(
        activation,
        [&]() {
            const auto snapshot =
                session.CurrentSampleSnapshot();
            return snapshot &&
                snapshot->collection.current_index == 1 &&
                !activation.status().loading;
        });
    const spectiary::NavigationLatencyPresentation
        presentation{
            9,
            spectiary::NavigationLatencyTrace::Now()};
    const auto before_draw =
        ActivationAccess::CompleteNavigationFrame(
            activation,
            17,
            std::span(&presentation, 1));
    activation.RecordSpectrumDrawSubmission(
        17,
        9,
        session.CurrentSampleSnapshot());
    const auto after_draw =
        ActivationAccess::CompleteNavigationFrame(
            activation,
            17,
            std::span(&presentation, 1));

    std::filesystem::remove(path);
    Require(
        navigation.follow_up_spectrum_index() == 1 &&
            activated,
        "traced navigation should activate row one");
    Require(
        before_draw.empty(),
        "Present without the exact draw must not finish the trace");
    Require(
        after_draw.size() == 1 &&
            after_draw.front().outcome ==
                spectiary::NavigationLatencyOutcome::Presented,
        "the exact drawn snapshot should finish the trace");
}

void TestPublicInterfacePublishesPresentedOpenLifecycle()
{
    const std::filesystem::path path =
        UniqueTempPath("_public_present.csv");
    const std::filesystem::path profile_path =
        UniqueTempPath("_public_present.jsonl");
    WriteFixture(path);
    auto dependencies = MakeDependencies(
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session(
        {},
        {},
        {},
        {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    spectiary::ProfileSink profile(profile_path);
    profile.BeginFrame();
    activation.BeginFrame(true, 23, &profile);

    (void)activation.OpenSource(path, 0);
    const bool activated = DrainUntil(
        activation,
        [&]() {
            const auto snapshot =
                session.CurrentSampleSnapshot();
            return snapshot &&
                snapshot->source.path == path &&
                !activation.status().loading;
        });
    const spectiary::NavigationLatencyPresentation
        presentation{
            7,
            spectiary::NavigationLatencyTrace::Now()};
    activation.PresentFrame(
        23,
        std::span(&presentation, 1));
    const std::uint64_t sequence_without_draw =
        activation.presented_source_load_observation()
            .sequence;
    activation.RecordSpectrumDrawSubmission(
        23,
        7,
        session.CurrentSampleSnapshot());
    const spectiary::NavigationLatencyPresentation
        wrong_viewport_presentation{
            8,
            spectiary::NavigationLatencyTrace::Now()};
    activation.PresentFrame(
        23,
        std::span(&wrong_viewport_presentation, 1));
    const std::uint64_t sequence_after_wrong_viewport =
        activation.presented_source_load_observation()
            .sequence;
    activation.PresentFrame(
        23,
        std::span(&presentation, 1));
    const auto presented =
        activation.presented_source_load_observation();
    profile.Stop();
    const std::string profile_text =
        ReadText(profile_path);

    std::filesystem::remove(path);
    std::filesystem::remove(profile_path);
    Require(
        activated,
        "open -> prepare -> commit should publish the source snapshot");
    Require(
        sequence_without_draw == 0 &&
            sequence_after_wrong_viewport == 0,
        "missing or mismatched Present evidence must not advance the source-load presentation observation");
    Require(
        presented.sequence == 1 &&
            presented.source_load_id > 0 &&
            presented.activation_frame > 0 &&
            presented.viewport_id == 7 &&
            presented.source_path == path &&
            presented.spectrum_index == 0,
        "the exact drawn snapshot and successful viewport Present should publish one identity-bearing observation");
    Require(
        profile_text.find(
            "\"event\":\"source_load_latency\"") !=
                std::string::npos &&
            profile_text.find(
                "\"outcome\":\"presented\"") !=
                std::string::npos,
        "the frame fact should complete and publish the presented activation lifecycle");
}

void TestAutomationOpenCompletesAfterPresentationWithoutProfiling()
{
    const std::filesystem::path path =
        UniqueTempPath("_automation_present.csv");
    WriteFixture(path);
    auto dependencies = MakeDependencies(
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session(
        {},
        {},
        {},
        {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    activation.BeginFrame(false, 31, nullptr);

    const Activation::SourceOpenOperation operation =
        activation.OpenSourceForAutomation(path, 0);
    const bool activated = DrainUntil(
        activation,
        [&]() {
            const auto snapshot =
                session.CurrentSampleSnapshot();
            return snapshot &&
                snapshot->source.path == path &&
                !activation.status().loading;
        });
    const auto before_present =
        activation.ObserveSourceOpenOperation(
            operation);
    activation.RecordSpectrumDrawSubmission(
        31,
        11,
        session.CurrentSampleSnapshot());
    activation.PresentFrame(31, {});
    const auto after_present_retry =
        activation.presented_spectrum_observation();
    const spectiary::NavigationLatencyPresentation
        wrong_presentation{
            12,
            spectiary::NavigationLatencyTrace::Now()};
    activation.PresentFrame(
        31,
        std::span(&wrong_presentation, 1));
    const auto after_wrong_viewport =
        activation.presented_spectrum_observation();
    const spectiary::NavigationLatencyPresentation
        presentation{
            11,
            spectiary::NavigationLatencyTrace::Now()};
    activation.PresentFrame(
        31,
        std::span(&presentation, 1));
    const auto after_present =
        activation.ObserveSourceOpenOperation(
            operation);

    std::filesystem::remove(path);
    Require(
        activated &&
            before_present.state ==
                Activation::SourceOpenOperationState::
                    Pending &&
            after_present_retry.sequence == 0 &&
            after_wrong_viewport.sequence == 0,
        "automation source open should remain pending through missing or wrong-viewport Present evidence");
    Require(
        after_present.state ==
                Activation::SourceOpenOperationState::
                    Succeeded &&
            after_present.source_path == path &&
            after_present.source_id ==
                "activation-fixture" &&
            after_present.spectrum_count == 3 &&
            after_present.spectrum_index == 0 &&
            activation
                    .presented_spectrum_observation()
                    .sequence == 1 &&
            activation
                    .presented_spectrum_observation()
                    .source_id ==
                "activation-fixture" &&
            activation
                    .presented_spectrum_observation()
                    .spectrum_index == 0,
        "automation source open should complete from the normal presentation seam even when profiling is disabled");
}

void TestAutomationOpensSupersedeBeforeSingleCompletionDrain()
{
    const std::filesystem::path first_path =
        UniqueTempPath("_automation_first.csv");
    const std::filesystem::path second_path =
        UniqueTempPath("_automation_second.csv");
    WriteFixture(first_path);
    WriteFixture(second_path);

    std::promise<void> first_started_promise;
    std::shared_future<void> first_started =
        first_started_promise.get_future().share();
    std::promise<void> second_started_promise;
    std::shared_future<void> second_started =
        second_started_promise.get_future().share();
    std::promise<void> release_promise;
    std::shared_future<void> release =
        release_promise.get_future().share();
    auto dependencies = MakeDependencies(
        [&](const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            if (source == first_path) {
                first_started_promise.set_value();
            } else {
                second_started_promise.set_value();
            }
            release.wait();
            auto snapshot =
                std::make_shared<
                    spectiary::SpectrumSnapshot>(
                    *MakeSnapshot(source, index));
            snapshot->source.id =
                source.filename().string();
            snapshot->current_spectrum.name =
                source.filename().string();
            return spectiary::SpectrumSnapshotHandle(
                std::move(snapshot));
        });
    spectiary::SourceCollectionSession session(
        {}, {}, {}, {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    activation.BeginFrame(false, 41, nullptr);

    const auto first =
        activation.OpenSourceForAutomation(
            first_path,
            0);
    Require(
        first_started.wait_for(2s) ==
            std::future_status::ready,
        "the first automation open worker should start");
    const auto second =
        activation.OpenSourceForAutomation(
            second_path,
            0);
    Require(
        second_started.wait_for(2s) ==
            std::future_status::ready,
        "the superseding automation open worker should start");
    const Activation::Status superseding_status =
        activation.status();
    const auto first_before_drain =
        activation.ObserveSourceOpenOperation(first);
    release_promise.set_value();
    const auto completions_ready_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (ActivationAccess::CompletedLoadCount(
               activation) < 2U &&
           std::chrono::steady_clock::now() <
               completions_ready_deadline) {
        std::this_thread::sleep_for(2ms);
    }
    (void)activation.Drain(false);

    const auto first_after_drain =
        activation.ObserveSourceOpenOperation(first);
    const auto second_before_present =
        activation.ObserveSourceOpenOperation(second);
    const auto active_snapshot =
        session.CurrentSampleSnapshot();
    activation.RecordSpectrumDrawSubmission(
        41,
        17,
        active_snapshot);
    const spectiary::NavigationLatencyPresentation
        presentation{
            17,
            spectiary::NavigationLatencyTrace::Now()};
    activation.PresentFrame(
        41,
        std::span(&presentation, 1));
    const auto second_after_present =
        activation.ObserveSourceOpenOperation(second);

    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
    Require(
        first_before_drain.state ==
                Activation::SourceOpenOperationState::
                    Canceled &&
            first_after_drain.state ==
                Activation::SourceOpenOperationState::
                    Canceled,
        "a superseded automation open should remain terminal canceled before and after both completions drain");
    Require(
        superseding_status.loading_source_path ==
            second_path,
        "concurrent opens should project only the latest activation source");
    Require(
        active_snapshot &&
            active_snapshot->source.path ==
                second_path &&
            second_before_present.state ==
                Activation::SourceOpenOperationState::
                    Pending &&
            second_after_present.state ==
                Activation::SourceOpenOperationState::
                    Succeeded,
        "one completion drain should activate only the newer source and complete it after the exact successful Present");
}

void TestGuiOpenSupersedesPendingAutomationWithoutLaterActivation()
{
    const std::filesystem::path automation_path =
        UniqueTempPath("_automation_superseded_by_gui.csv");
    const std::filesystem::path gui_path =
        UniqueTempPath("_gui_superseding_automation.csv");
    WriteFixture(automation_path);
    WriteFixture(gui_path);

    std::promise<void> automation_started_promise;
    std::shared_future<void> automation_started =
        automation_started_promise.get_future().share();
    std::promise<void> release_automation_promise;
    std::shared_future<void> release_automation =
        release_automation_promise.get_future().share();
    std::atomic_bool automation_signaled = false;
    auto dependencies = MakeDependencies(
        [&](const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            if (source == automation_path &&
                !automation_signaled.exchange(true)) {
                automation_started_promise.set_value();
                release_automation.wait();
            }
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session(
        {}, {}, {}, {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    const auto automation =
        activation.OpenSourceForAutomation(
            automation_path,
            0);
    Require(
        automation_started.wait_for(2s) ==
            std::future_status::ready,
        "the automation worker should block before a real GUI open supersedes it");
    (void)activation.OpenSource(gui_path, 0);
    const auto canceled =
        activation.ObserveSourceOpenOperation(
            automation);
    release_automation_promise.set_value();
    const bool retired = DrainUntil(
        activation,
        [&]() {
            const auto snapshot =
                session.CurrentSampleSnapshot();
            return snapshot &&
                snapshot->source.path == gui_path &&
                !activation.status().loading;
        });
    const auto final_snapshot =
        session.CurrentSampleSnapshot();
    const auto final_view = session.View();

    std::filesystem::remove(automation_path);
    std::filesystem::remove(gui_path);
    Require(
        canceled.state ==
                Activation::SourceOpenOperationState::
                    Canceled &&
            retired &&
            final_snapshot &&
            final_snapshot->source.path ==
                gui_path &&
            final_view.sources.size() == 1 &&
            final_view.sources.front().path ==
                gui_path,
        "a GUI-superseded automation open must stay side-effect-free after its worker eventually returns");
}

void TestReselectingCurrentSourceSupersedesPendingOpen(bool completion_ready)
{
    const auto source_a = UniqueTempPath("_reselected_a.csv");
    const auto source_b = UniqueTempPath("_superseded_b.csv");
    WriteFixture(source_a);
    WriteFixture(source_b);
    std::promise<void> started_promise, release_promise;
    auto started = started_promise.get_future();
    auto release = release_promise.get_future().share();
    auto dependencies = MakeDependencies(
        [&](const auto& path, std::size_t index, const auto&) {
            started_promise.set_value();
            release.wait();
            return MakeSnapshot(path, index);
        });
    auto session = MakePreparedSession(source_a);
    const auto original_snapshot = session.CurrentSampleSnapshot();
    std::size_t presentation_changes = 0;
    Activation activation(session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    ActivationAccess::ObserveSnapshotChanges(activation,
        [&](auto) { ++presentation_changes; });
    activation.BeginFrame(true, 1, nullptr);
    (void)activation.OpenSource(source_b, 0);
    const bool worker_started = started.wait_for(2s) == std::future_status::ready;
    const auto pending_generation = activation.activation_generation();
    const bool original_still_current =
        session.CurrentSampleSnapshot() == original_snapshot &&
        activation.status().loading_source_path == source_b;

    bool completion_published = false;
    if (completion_ready) {
        release_promise.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (ActivationAccess::CompletedLoadCount(activation) == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(2ms);
        }
        completion_published = ActivationAccess::CompletedLoadCount(activation) == 1;
    }
    const auto reselected = activation.Submit(
        spectiary::SourceCollectionSessionIntent::EditSourceCollection(
            spectiary::SourceCollectionIntent::SwitchActive(0)));
    const bool intent_advanced = activation.activation_generation() > pending_generation;
    if (!completion_ready) release_promise.set_value();

    // Supersession clears UI loading before the worker exits. Wait for actual
    // queue quiescence so a late completion cannot escape these assertions.
    const bool drained = DrainUntil(activation, [&]() {
        const auto activity = ActivationAccess::QueueActivity(activation);
        return activity.active_task_count == 0 && activity.completed_count == 0 &&
               activity.worker_count == 0;
    });
    const auto reports = ActivationAccess::CompleteSourceLoadFrame(activation, 1, {});
    std::filesystem::remove(source_a);
    std::filesystem::remove(source_b);

    Require(worker_started && original_still_current &&
            (!completion_ready || completion_published),
        "source B must be pending while source A is still current before reselection");
    Require(intent_advanced && drained && !activation.status().loading,
        "reselecting current source A must supersede and drain the old B intent");
    Require(session.View().current_source_index == 0 &&
            session.View().sources.size() == 1 &&
            session.CurrentSampleSnapshot() == original_snapshot,
        "a late source B completion must not replace or enter the current A session");
    Require(!reselected.action.snapshot_changed && presentation_changes == 0,
        "current-source reselection and the superseded B completion must not change presentation");
    Require(reports.size() == 1 &&
            reports.front().outcome == spectiary::SourceLoadLatencyOutcome::Superseded,
        "the old B load must finish as superseded rather than presentable");
}

void TestSameIdentityOpenRequiresLatestActivationPresent()
{
    const std::filesystem::path path =
        UniqueTempPath("_automation_same_identity.csv");
    WriteFixture(path);
    auto dependencies = MakeDependencies(
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session(
        {}, {}, {}, {});
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    activation.BeginFrame(false, 51, nullptr);
    const auto first =
        activation.OpenSourceForAutomation(path, 0);
    Require(
        DrainUntil(
            activation,
            [&]() {
                return !activation.status().loading;
            }),
        "the first same-identity open should activate");
    activation.RecordSpectrumDrawSubmission(
        51,
        23,
        session.CurrentSampleSnapshot());
    const spectiary::NavigationLatencyPresentation
        first_presentation{
            23,
            spectiary::NavigationLatencyTrace::Now()};
    activation.PresentFrame(
        51,
        std::span(&first_presentation, 1));
    Require(
        activation.ObserveSourceOpenOperation(first)
                .state ==
            Activation::SourceOpenOperationState::
                Succeeded,
        "the initial source open should establish a presented activation");
    const auto first_observation =
        activation.presented_spectrum_observation();
    const auto previous_snapshot =
        session.CurrentSampleSnapshot();

    const auto second =
        activation.OpenSourceForAutomation(path, 0);
    activation.BeginFrame(false, 52, nullptr);
    activation.RecordSpectrumDrawSubmission(
        52,
        23,
        previous_snapshot);
    const spectiary::NavigationLatencyPresentation
        stale_presentation{
            23,
            spectiary::NavigationLatencyTrace::Now()};
    activation.PresentFrame(
        52,
        std::span(&stale_presentation, 1));
    const auto before_commit =
        activation.ObserveSourceOpenOperation(second);
    Require(
        DrainUntil(
            activation,
            [&]() {
                return !activation.status().loading;
            }),
        "the replacement same-identity open should commit");
    const auto after_commit_before_present =
        activation.ObserveSourceOpenOperation(second);

    activation.BeginFrame(false, 53, nullptr);
    activation.RecordSpectrumDrawSubmission(
        53,
        23,
        session.CurrentSampleSnapshot());
    const spectiary::NavigationLatencyPresentation
        latest_presentation{
            23,
            spectiary::NavigationLatencyTrace::Now()};
    activation.PresentFrame(
        53,
        std::span(&latest_presentation, 1));
    const auto after_latest_present =
        activation.ObserveSourceOpenOperation(second);
    const auto latest_observation =
        activation.presented_spectrum_observation();

    std::filesystem::remove(path);
    Require(
        before_commit.state ==
                Activation::SourceOpenOperationState::
                    Pending &&
            after_commit_before_present.state ==
                Activation::SourceOpenOperationState::
                    Pending,
        "an old same-source same-row Present must not complete the replacement activation");
    Require(
        after_latest_present.state ==
                Activation::SourceOpenOperationState::
                    Succeeded &&
            latest_observation.activation_generation >
                first_observation
                    .activation_generation,
        "only the latest activation generation Present should complete the same-identity open");
}

void TestSamePathOpenTokenSurvivesProductionFollowUp()
{
    const std::filesystem::path path =
        UniqueTempPath("_automation_follow_up.csv");
    WriteFixture(path);
    auto dependencies = MakeDependencies(
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session =
        MakePreparedSession(path, 2);
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    activation.BeginFrame(false, 61, nullptr);
    const auto operation =
        activation.OpenSourceForAutomation(path, 0);
    const bool completed_follow_up = DrainUntil(
        activation,
        [&]() {
            const auto snapshot =
                session.CurrentSampleSnapshot();
            return snapshot &&
                snapshot->source.path == path &&
                snapshot->collection.current_index ==
                    2 &&
                !activation.status().loading;
        });
    const auto before_present =
        activation.ObserveSourceOpenOperation(operation);
    activation.RecordSpectrumDrawSubmission(
        61,
        29,
        session.CurrentSampleSnapshot());
    const spectiary::NavigationLatencyPresentation
        presentation{
            29,
            spectiary::NavigationLatencyTrace::Now()};
    activation.PresentFrame(
        61,
        std::span(&presentation, 1));
    const auto after_present =
        activation.ObserveSourceOpenOperation(operation);

    std::filesystem::remove(path);
    Require(
        completed_follow_up &&
            before_present.state ==
                Activation::SourceOpenOperationState::
                    Pending &&
            after_present.state ==
                Activation::SourceOpenOperationState::
                    Succeeded &&
            after_present.spectrum_index == 2,
        "the latest same-path open token should survive its production follow-up load and complete after exact Present");
}

void TestIdlePrefetchReportsLifecycleCompletion()
{
    const std::filesystem::path path =
        UniqueTempPath("_prefetch.csv");
    WriteFixture(path);

    std::promise<void> prefetch_started_promise;
    std::shared_future<void> prefetch_started =
        prefetch_started_promise.get_future().share();
    std::promise<void> release_prefetch_promise;
    std::shared_future<void> release_prefetch =
        release_prefetch_promise.get_future().share();
    std::atomic_bool prefetch_started_once = false;
    auto dependencies = MakeDependencies(
        [&](const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            if (index == 2 &&
                !prefetch_started_once.exchange(true)) {
                prefetch_started_promise.set_value();
                release_prefetch.wait();
            }
            return MakeSnapshot(source, index);
        });
    spectiary::SourceCollectionSession session =
        MakePreparedSession(path);
    Activation activation(
        session,
        spectiary::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    (void)activation.Submit(
        spectiary::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                spectiary::SampleNavigationIntent::Move(
                    spectiary::
                        SampleNavigationRequest::Next())),
        Activation::NavigationIntent{
            spectiary::NavigationLatencyInputKind::UiNext,
            std::nullopt});

    const bool prefetch_active = DrainUntil(
        activation,
        [&]() {
            return prefetch_started.wait_for(0s) ==
                       std::future_status::ready &&
                ActivationAccess::PrefetchActive(
                    activation);
        },
        true);
    const Activation::Status prefetch_status =
        activation.status();
    release_prefetch_promise.set_value();

    const bool prefetch_finished = DrainUntil(
        activation,
        [&]() {
            const auto reports =
                ActivationAccess::TakePrefetchReports(
                    activation);
            for (const auto& report : reports) {
                if (report.outcome ==
                    spectiary::NavigationPrefetchOutcome::
                        Completed) {
                    return true;
                }
            }
            return false;
        },
        true);

    std::filesystem::remove(path);
    Require(
        prefetch_active &&
            prefetch_status.loading_source_path.empty() &&
            prefetch_finished,
        "idle prefetch should stay out of the visible loading-source projection and publish a terminal lifecycle result");
}

}  // namespace

int main()
{
    try {
        TestRestoredPositionalAnnotationsRequireSavedSourceIdentity();
        TestDeferredRestoreKeepsAllThirtySixSourcesAndSavedActivation();
        TestStatusReportsCurrentLoadingSourcePath();
        TestRapidNavigationPublishesOnlyLatestIntent();
        TestFailedExplicitOpenProducesTerminalLifecycleResult();
        TestActivationOwnsQueueServiceDeadline();
        TestLastWorkerIsReapedByScheduledServiceAfterCompletionDrain();
        TestSuccessfulSourceDoesNotHideConcurrentFailure();
        TestConcurrentFailuresRemainVisible();
        TestFailedMemberClearsSampleButPreservesWorkflow();
        TestAcknowledgedFailuresStayTerminalAndNewGenerationReappears();
        TestSuccessfulRetryClearsPreviousFailures();
        TestLaterSourceOpenReplacesHistoricalFailureStatus();
        TestStartupFailuresFollowSavedActiveSourceAndRemainRemovable();
        TestCanceledGenerationDoesNotPublishFailure(false);
        TestCanceledGenerationDoesNotPublishFailure(true);
        TestPresentationCompletesOnlyAfterExactSnapshotDraw();
        TestPublicInterfacePublishesPresentedOpenLifecycle();
        TestAutomationOpenCompletesAfterPresentationWithoutProfiling();
        TestAutomationOpensSupersedeBeforeSingleCompletionDrain();
        TestGuiOpenSupersedesPendingAutomationWithoutLaterActivation();
        TestReselectingCurrentSourceSupersedesPendingOpen(false);
        TestReselectingCurrentSourceSupersedesPendingOpen(true);
        TestSameIdentityOpenRequiresLatestActivationPresent();
        TestSamePathOpenTokenSurvivesProductionFollowUp();
        TestIdlePrefetchReportsLifecycleCompletion();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
