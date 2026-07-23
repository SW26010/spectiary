#include "domain/source_collection_manifest.h"
#include "profile/navigation_latency_trace.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_activation_transaction.h"

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

namespace {

using namespace std::chrono_literals;
using Activation =
    specforge::SourceCollectionActivationTransaction;

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
    specforge::SourceCollectionSession session(
        [](const std::filesystem::path&,
           std::size_t)
            -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error(
                "activation tests must not load synchronously");
        },
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        specforge::SourceCollectionSessionRestoreMode::
            Deferred);
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

specforge::SourceCollectionLoadDependencies
MakeDependencies(
    specforge::SourceCollectionLoadDependencies::
        SnapshotLoader snapshot_loader)
{
    specforge::SourceCollectionLoadDependencies dependencies;
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
        specforge::SourceCollectionLoadQueue(
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
                !activation.HasPendingLoads();
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
    WriteFixture(path);
    specforge::SourceCollectionSession session(
        [](const std::filesystem::path&,
           std::size_t)
            -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error(
                "activation test must remain asynchronous");
        },
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        specforge::SourceCollectionSessionRestoreMode::
            Deferred);
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
        specforge::SourceCollectionLoadQueue(
            std::move(dependencies)));
    activation.SetPresentationContext(true, 11);
    (void)activation.OpenSource(path, 0);

    const bool failure_drained = DrainUntil(
        activation,
        [&]() {
            return !activation.ErrorMessage().empty() &&
                !activation.HasPendingLoads();
        });
    const auto reports =
        activation.CompleteSourceLoadFramePresentations(
            11,
            {});

    std::filesystem::remove(path);
    Require(
        failure_drained,
        "failed load should become a visible lifecycle result");
    Require(
        reports.size() == 1 &&
            reports.front().outcome ==
                specforge::SourceLoadLatencyOutcome::Failed,
        "failed explicit open should emit one terminal trace");
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
        specforge::SourceCollectionLoadQueue(
            std::move(dependencies)));
    activation.SetPresentationContext(true, 17);
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
                !activation.HasPendingLoads();
        });
    const specforge::NavigationLatencyPresentation
        presentation{
            9,
            specforge::NavigationLatencyTrace::Now()};
    const auto before_draw =
        activation.CompleteFramePresentations(
            17,
            std::span(&presentation, 1));
    activation.RecordSpectrumDrawSubmission(
        17,
        9,
        session.CurrentSampleSnapshot());
    const auto after_draw =
        activation.CompleteFramePresentations(
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
        specforge::SourceCollectionLoadQueue(
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
                activation.TakeNavigationPrefetchReports();
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
        TestPresentationCompletesOnlyAfterExactSnapshotDraw();
        TestIdlePrefetchReportsLifecycleCompletion();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
