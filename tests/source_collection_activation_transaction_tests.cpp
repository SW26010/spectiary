#include "domain/source_collection_manifest.h"
#include "profile/navigation_latency_trace.h"
#include "profile/profile_sink.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_activation_transaction.h"
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

namespace specforge {

struct SourceCollectionActivationTransactionTestAccess {
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
};

}  // namespace specforge

namespace {

using namespace std::chrono_literals;
using Activation =
    specforge::SourceCollectionActivationTransaction;
using ActivationAccess =
    specforge::
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
        ("specforge_activation_lifecycle_" +
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

specforge::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    auto snapshot =
        std::make_shared<specforge::SpectrumSnapshot>();
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

specforge::SourceCollectionSession MakePreparedSession(
    const std::filesystem::path& path)
{
    specforge::SourceCollectionSession session({}, {}, {}, {});
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(path, 0);
    specforge::SourceCollectionContext context;
    context.identity =
        specforge::BuildSourceCollectionIdentity(
            *snapshot,
            specforge::
                CaptureSourceCollectionSingleFileState(path));
    context.manifest.sample_names = {
        "alpha",
        "beta",
        "gamma",
    };
    specforge::PreparedSampleWorkflowState workflow =
        specforge::PrepareSampleWorkflowState(
            *snapshot,
            context,
            0,
            {{}, {}});
    Require(
        session
            .OpenPreparedSource(
                path,
                0,
                snapshot,
                std::move(context),
                std::move(workflow))
            .loaded,
        "activation fixture should commit row zero");
    return session;
}

specforge::SourceCollectionPreparationAdapters
MakeDependencies(
    specforge::SourceCollectionPreparationAdapters::
        SnapshotLoader snapshot_loader)
{
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        std::move(snapshot_loader);
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
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
    specforge::SourceCollectionSession session =
        MakePreparedSession(path);
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    const auto first = activation.Submit(
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::
                        SampleNavigationRequest::Next())));
    Require(
        first.follow_up_spectrum_index == 1,
        "first navigation should target row one");
    Require(
        first_entered.wait_for(2s) ==
            std::future_status::ready,
        "first worker should start");

    const auto second = activation.Submit(
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::
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
        second.follow_up_spectrum_index == 2,
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
    specforge::SourceCollectionSession session({}, {}, {}, {});
    auto dependencies = MakeDependencies(
        [](const std::filesystem::path&,
           std::size_t,
           const auto&)
            -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error(
                "expected activation failure");
        });
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    specforge::ProfileSink profile(profile_path);
    profile.BeginFrame();
    activation.BeginFrame(true, 11, &profile);
    (void)activation.OpenSource(path, 0);

    const bool failure_drained = DrainUntil(
        activation,
        [&]() {
            return !activation.status().error_message.empty() &&
                !activation.status().loading;
        });
    const std::string error(
        activation.status().error_message);
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
            -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error(
                "synthetic service deadline failure");
        });
    specforge::SourceCollectionSession session(
        {},
        {},
        {},
        {});
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    const auto before_open =
        specforge::LocalUserStateSaveScheduler::
            Clock::now();
    (void)activation.OpenSource(path, 0);
    const auto scheduled =
        ActivationAccess::NextMaintenanceDeadline(
            activation);
    const auto after_open =
        specforge::LocalUserStateSaveScheduler::
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
    specforge::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

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
            -> specforge::SpectrumSnapshotHandle {
            if (source == first_path) {
                throw std::runtime_error(
                    "first source failed");
            }
            throw std::runtime_error(
                "second source failed");
        });
    specforge::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
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
            -> specforge::SpectrumSnapshotHandle {
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
    specforge::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
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
                        specforge::SourceLoadLatencyOutcome::
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

void TestSuccessfulRetryClearsOnlyItsSourceFailure()
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
    specforge::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
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
    const bool failure_retained_while_pending =
        !activation.status().error_message.empty();
    const bool retry_drained = DrainUntil(
        activation,
        [&]() {
            const std::string_view error =
                activation.status().error_message;
            return error.find(
                       "retryable source failure") ==
                    std::string_view::npos &&
                error.find("other source failure") !=
                    std::string_view::npos &&
                !activation.status().loading;
        });

    std::filesystem::remove(retry_path);
    std::filesystem::remove(other_path);
    Require(
        failure_drained,
        "the first source attempt should publish its failure");
    Require(
        failure_retained_while_pending,
        "starting a retry must not clear its source failure prematurely");
    Require(
        retry_drained,
        "a successful retry should clear only its source failure");
}

void TestCanceledGenerationDoesNotPublishFailure()
{
    const std::filesystem::path path =
        UniqueTempPath("_canceled.csv");
    WriteFixture(path);
    std::promise<void> first_started_promise;
    std::shared_future<void> first_started =
        first_started_promise.get_future().share();
    std::atomic_uint32_t attempts = 0;
    auto dependencies = MakeDependencies(
        [&attempts, &first_started_promise](
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
            return MakeSnapshot(source, index);
        });
    specforge::SourceCollectionSession session({}, {}, {}, {});
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));

    (void)activation.OpenSource(path, 0);
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

    std::filesystem::remove(path);
    Require(
        retry_drained,
        "the replacement source generation should drain");
    Require(
        error.find("canceled source failure") ==
            std::string::npos,
        "a canceled source generation must not publish a user error");
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
    specforge::SourceCollectionSession session =
        MakePreparedSession(path);
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    activation.BeginFrame(true, 17, nullptr);
    const auto navigation = activation.Submit(
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::
                        SampleNavigationRequest::Next())),
        Activation::NavigationIntent{
            specforge::NavigationLatencyInputKind::UiNext,
            specforge::NavigationLatencyTrace::Now()});
    const bool activated = DrainUntil(
        activation,
        [&]() {
            const auto snapshot =
                session.CurrentSampleSnapshot();
            return snapshot &&
                snapshot->collection.current_index == 1 &&
                !activation.status().loading;
        });
    const specforge::NavigationLatencyPresentation
        presentation{
            9,
            specforge::NavigationLatencyTrace::Now()};
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
        navigation.follow_up_spectrum_index == 1 &&
            activated,
        "traced navigation should activate row one");
    Require(
        before_draw.empty(),
        "Present without the exact draw must not finish the trace");
    Require(
        after_draw.size() == 1 &&
            after_draw.front().outcome ==
                specforge::NavigationLatencyOutcome::Presented,
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
    specforge::SourceCollectionSession session(
        {},
        {},
        {},
        {});
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    specforge::ProfileSink profile(profile_path);
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
    const specforge::NavigationLatencyPresentation
        presentation{
            7,
            specforge::NavigationLatencyTrace::Now()};
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
    const specforge::NavigationLatencyPresentation
        wrong_viewport_presentation{
            8,
            specforge::NavigationLatencyTrace::Now()};
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

void TestIdlePrefetchReportsLifecycleCompletion()
{
    const std::filesystem::path path =
        UniqueTempPath("_prefetch.csv");
    WriteFixture(path);
    auto dependencies = MakeDependencies(
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        });
    specforge::SourceCollectionSession session =
        MakePreparedSession(path);
    Activation activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    (void)activation.Submit(
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::
                        SampleNavigationRequest::Next())),
        Activation::NavigationIntent{
            specforge::NavigationLatencyInputKind::UiNext,
            std::nullopt});

    const bool prefetch_finished = DrainUntil(
        activation,
        [&]() {
            const auto reports =
                ActivationAccess::TakePrefetchReports(
                    activation);
            for (const auto& report : reports) {
                if (report.outcome ==
                    specforge::NavigationPrefetchOutcome::
                        Completed) {
                    return true;
                }
            }
            return false;
        },
        true);

    std::filesystem::remove(path);
    Require(
        prefetch_finished,
        "idle prefetch should publish a terminal lifecycle result");
}

}  // namespace

int main()
{
    try {
        TestRapidNavigationPublishesOnlyLatestIntent();
        TestFailedExplicitOpenProducesTerminalLifecycleResult();
        TestActivationOwnsQueueServiceDeadline();
        TestSuccessfulSourceDoesNotHideConcurrentFailure();
        TestConcurrentFailuresRemainVisible();
        TestAcknowledgedFailuresStayTerminalAndNewGenerationReappears();
        TestSuccessfulRetryClearsOnlyItsSourceFailure();
        TestCanceledGenerationDoesNotPublishFailure();
        TestPresentationCompletesOnlyAfterExactSnapshotDraw();
        TestPublicInterfacePublishesPresentedOpenLifecycle();
        TestIdlePrefetchReportsLifecycleCompletion();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
