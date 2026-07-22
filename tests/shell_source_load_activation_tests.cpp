#include "ui/shell_ui.h"

#include "domain/source_path_identity.h"
#include "domain/sample_labeling.h"

#include <algorithm>
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

struct ShellUiTestAccess {
    using Purpose = ShellUi::PendingSourceLoadPurpose;
    using PendingLoad = ShellUi::PendingSourceLoad;

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

    static void QueueFollowUp(
        ShellUi& shell,
        const SourceCollectionSessionResult& result,
        bool deferred_restore)
    {
        shell.QueueSessionFollowUp(result, deferred_restore);
    }

    static void Drain(ShellUi& shell)
    {
        shell.DrainSourceLoads();
    }

    static std::vector<SourceCollectionLoadCompletion> TakeCompleted(ShellUi& shell)
    {
        return shell.source_load_queue_.TakeCompleted();
    }

    static void DrainCompleted(
        ShellUi& shell,
        std::vector<SourceCollectionLoadCompletion> completions)
    {
        shell.DrainSourceLoadCompletions(std::move(completions));
    }

    static std::size_t PendingLoadCount(const ShellUi& shell)
    {
        return shell.pending_source_loads_.size();
    }

    static std::string_view LoadError(const ShellUi& shell)
    {
        return shell.source_load_error_;
    }

    static bool CompletionStartsActivation(Purpose purpose, bool loaded)
    {
        return ShellUi::CompletionStartsSourceActivationIntent(purpose, loaded);
    }

    static void AdvanceActivation(
        std::uint64_t& activation_epoch,
        std::unordered_map<std::uint64_t, PendingLoad>& pending_loads,
        bool preserve_pending_explicit_opens,
        std::vector<std::uint64_t>& canceled)
    {
        ShellUi::AdvanceSourceActivationIntent(
            activation_epoch,
            pending_loads,
            preserve_pending_explicit_opens,
            [&canceled](std::uint64_t task_id) { canceled.push_back(task_id); });
    }

    static void CancelSourceFollowUps(
        const SourceCollectionSessionResult& result,
        std::unordered_map<std::uint64_t, PendingLoad>& pending_loads,
        std::unordered_set<std::uint64_t>& deferred_restore_task_ids,
        const std::function<void(std::uint64_t)>& cancel)
    {
        ShellUi::CancelSourceFollowUpsForResultInState(
            result,
            pending_loads,
            deferred_restore_task_ids,
            cancel);
    }

    static std::optional<PendingLoad> TakeCurrent(
        const SourceCollectionLoadCompletion& completion,
        std::uint64_t activation_epoch,
        std::unordered_map<std::uint64_t, PendingLoad>& pending_loads,
        const std::unordered_map<std::string, std::uint64_t>& generations)
    {
        return ShellUi::TakeCurrentPendingSourceLoad(
            completion,
            activation_epoch,
            pending_loads,
            generations);
    }

    static bool HasMatchingFollowUp(
        const std::filesystem::path& path,
        std::size_t spectrum_index,
        const std::unordered_map<std::uint64_t, PendingLoad>& pending_loads)
    {
        return ShellUi::HasMatchingSourceFollowUp(path, spectrum_index, pending_loads);
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

specforge::ShellUiTestAccess::PendingLoad Pending(
    std::string_view name,
    std::uint64_t activation_epoch,
    specforge::ShellUiTestAccess::Purpose purpose)
{
    const std::filesystem::path path(name);
    return {
        .path = path,
        .path_key = specforge::SourcePathIdentityKey(path),
        .activation_epoch = activation_epoch,
        .purpose = purpose,
    };
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
    return specforge::SaveSampleLabelResultNpy(path, task, error);
}

std::vector<specforge::SourceCollectionLoadCompletion> WaitForCompletions(
    specforge::SourceCollectionLoadQueue& queue)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<specforge::SourceCollectionLoadCompletion> ready = queue.TakeCompleted();
        if (!ready.empty()) {
            return ready;
        }
        std::this_thread::sleep_for(2ms);
    }
    return queue.TakeCompleted();
}

specforge::SourceCollectionSession MakePreparedDeferredSession(
    const std::filesystem::path& path,
    std::optional<std::string> context_fingerprint_override = std::nullopt)
{
    specforge::SourceCollectionSession session(
        [](const std::filesystem::path&, std::size_t) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("Shell drain tests must not use synchronous source loading");
        },
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        specforge::SourceCollectionSessionRestoreMode::Deferred);
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

void TestLaterExplicitOpenCancelsEarlierFollowUp()
{
    using Access = specforge::ShellUiTestAccess;
    std::uint64_t activation_epoch = 10;
    std::unordered_map<std::uint64_t, Access::PendingLoad> pending_loads;
    pending_loads.emplace(2, Pending("B.csv", activation_epoch, Access::Purpose::ExplicitOpen));
    pending_loads.emplace(3, Pending("C.csv", activation_epoch, Access::Purpose::ExplicitOpen));
    pending_loads.emplace(4, Pending("restore.csv", activation_epoch, Access::Purpose::DeferredRestore));
    std::vector<std::uint64_t> canceled;

    Require(
        Access::CompletionStartsActivation(Access::Purpose::ExplicitOpen, true),
        "a successful explicit completion should advance source activation");
    Access::AdvanceActivation(activation_epoch, pending_loads, true, canceled);
    Require(activation_epoch == 11, "the first successful explicit completion should advance the epoch");
    Require(canceled.empty(), "later explicit opens and deferred restore must remain pending");
    Require(
        pending_loads.at(2).activation_epoch == 11 &&
            pending_loads.at(3).activation_epoch == 11,
        "later explicit opens should remain current");

    pending_loads.emplace(5, Pending("A.csv", activation_epoch, Access::Purpose::SessionFollowUp));
    pending_loads.erase(2);  // B is the explicit completion currently being applied.
    Access::AdvanceActivation(activation_epoch, pending_loads, true, canceled);

    Require(activation_epoch == 12, "the later explicit completion should advance the epoch again");
    Require(
        canceled == std::vector<std::uint64_t>{5},
        "the later explicit completion should cancel the earlier source follow-up");
    Require(!pending_loads.contains(5), "the canceled earlier follow-up should leave pending state");
    Require(
        pending_loads.contains(3) && pending_loads.at(3).activation_epoch == 12,
        "an even later explicit open should remain pending and current");
    Require(pending_loads.contains(4), "deferred restore should remain independent");
    Require(
        !Access::CompletionStartsActivation(Access::Purpose::ExplicitOpen, false) &&
            !Access::CompletionStartsActivation(Access::Purpose::SessionFollowUp, true),
        "failed or non-explicit completions must not supersede source activation");
}

void TestRemovedSourceResultCancelsOnlyThatSourcesDerivedTickets()
{
    using Access = specforge::ShellUiTestAccess;
    std::unordered_map<std::uint64_t, Access::PendingLoad> pending_loads;
    pending_loads.emplace(1, Pending("A.csv", 4, Access::Purpose::SessionFollowUp));
    pending_loads.emplace(2, Pending("B.csv", 4, Access::Purpose::SessionFollowUp));
    pending_loads.emplace(3, Pending("B.csv", 4, Access::Purpose::DeferredRestore));
    pending_loads.emplace(4, Pending("B.csv", 4, Access::Purpose::ExplicitOpen));
    std::unordered_set<std::uint64_t> deferred_restore_task_ids{3};
    std::vector<std::uint64_t> canceled;
    specforge::SourceCollectionSessionResult removed;
    removed.canceled_source_follow_up_path = "B.csv";

    Access::CancelSourceFollowUps(
        removed,
        pending_loads,
        deferred_restore_task_ids,
        [&canceled](std::uint64_t task_id) { canceled.push_back(task_id); });
    std::sort(canceled.begin(), canceled.end());

    Require(
        canceled == std::vector<std::uint64_t>{2, 3},
        "removing B should cancel B's session and deferred-restore tickets");
    Require(
        pending_loads.contains(1) && pending_loads.contains(4) &&
            !pending_loads.contains(2) && !pending_loads.contains(3),
        "removing B must retain A's navigation and B's explicit user-open intent");
    Require(
        deferred_restore_task_ids.empty(),
        "canceling B's deferred restore should remove its restore bookkeeping");
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
    specforge::SourceCollectionLoadDependencies dependencies;
    dependencies.snapshot_loader =
        [&row_one_entered_promise,
         &row_two_entered_promise,
         release_decoders](const std::filesystem::path& source, std::size_t index, const auto&) {
            if (index == 1) {
                row_one_entered_promise.set_value();
                release_decoders.wait();
            } else if (index == 2) {
                row_two_entered_promise.set_value();
                release_decoders.wait();
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
        specforge::SourceCollectionLoadQueue(std::move(dependencies)));

    const specforge::SourceCollectionSessionResult first = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool first_queued = first.follow_up_spectrum_index == 1;
    const bool row_one_started = row_one_entered.wait_for(2s) == std::future_status::ready;
    const specforge::SourceCollectionSessionResult second = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
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
    shell.reset();
    std::filesystem::remove(path);

    Require(first_queued && row_one_started, "row 1 should enter the real Shell worker chain");
    Require(second_queued && row_two_started, "row 2 should supersede row 1 while both use real workers");
    Require(
        latest_committed,
        "the production drain should admit and atomically publish only the latest row 2 completion");
    Require(no_load_error, "canceling the obsolete row 1 completion must not publish a load error");
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

    specforge::SourceCollectionLoadDependencies dependencies;
    dependencies.snapshot_loader =
        [stale_snapshot_destroyed_promise,
         &row_two_entered_promise,
         release_row_two](const std::filesystem::path& source, std::size_t index, const auto&) {
            if (index == 1) {
                return MakeSnapshotWithDestructionProbe(
                    source,
                    index,
                    stale_snapshot_destroyed_promise);
            }
            if (index == 2) {
                row_two_entered_promise.set_value();
                release_row_two.wait();
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
        specforge::SourceCollectionLoadQueue(std::move(dependencies)));
    const specforge::SpectrumSnapshotHandle initial_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();

    const specforge::SourceCollectionSessionResult first = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    std::vector<specforge::SourceCollectionLoadCompletion> stale_completions;
    const auto completion_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < completion_deadline) {
        stale_completions = Access::TakeCompleted(*shell);
        if (!stale_completions.empty()) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const bool stale_completion_taken =
        stale_completions.size() == 1 && stale_completions.front().prepared.has_value();

    const specforge::SourceCollectionSessionResult second = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool row_two_started =
        row_two_entered.wait_for(2s) == std::future_status::ready;
    const std::thread::id drain_thread = std::this_thread::get_id();
    Access::DrainCompleted(*shell, std::move(stale_completions));

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
        stale_completion_taken,
        "row 1 completion should be published and taken before supersession");
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
    specforge::SourceCollectionLoadDependencies dependencies;
    dependencies.snapshot_loader =
        [&decoder_entered_promise,
         release_decoder](const std::filesystem::path& source, std::size_t index, const auto&) {
            decoder_entered_promise.set_value();
            release_decoder.wait();
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path, "older-context"),
        specforge::SourceCollectionLoadQueue(std::move(dependencies)));

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
        specforge::LoadSampleAnnotationResultFromPath(
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
    std::atomic_int row_one_decode_count = 0;
    specforge::SourceCollectionLoadDependencies dependencies;
    dependencies.snapshot_loader =
        [&first_decode_entered_promise,
         release_first_decode,
         &row_two_entered_promise,
         release_row_two,
         intermediate_destroyed_promise,
         &row_one_decode_count](
            const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            if (index == 1) {
                if (row_one_decode_count.fetch_add(1) == 0) {
                    first_decode_entered_promise.set_value();
                    release_first_decode.wait();
                    return MakeSnapshot(source, index);
                }
                return MakeSnapshotWithDestructionProbe(
                    source,
                    index,
                    intermediate_destroyed_promise);
            }
            if (index == 2) {
                row_two_entered_promise.set_value();
                release_row_two.wait();
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
        specforge::SourceCollectionLoadQueue(std::move(dependencies)));

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

    Require(navigation_queued && first_decode_started, "row 1 should enter the real drain worker");
    Require(annotation_changed, "the annotation context should change while row 1 is decoded");
    Require(row_two_requeued, "draining row 1 should enqueue the reconciled row 2 follow-up");
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
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_late.csv");
    const std::filesystem::path other_path = UniqueTempPath("_other.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "late completion fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionSession session(
        [](const std::filesystem::path&, std::size_t) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("the Shell async chain must not use the session's synchronous loader");
        },
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        std::filesystem::path{},
        specforge::SourceCollectionSessionRestoreMode::Deferred);
    const specforge::SpectrumSnapshotHandle initial_snapshot = MakeSnapshot(path, 0);
    specforge::SourceCollectionContext initial_context;
    initial_context.identity = specforge::BuildSourceCollectionIdentity(
        *initial_snapshot,
        specforge::CaptureSourceCollectionSingleFileState(path));
    initial_context.manifest.sample_names = {"alpha", "beta", "gamma"};
    const specforge::SourceCollectionIdentity initial_identity = initial_context.identity;
    specforge::PreparedSampleWorkflowState initial_workflow =
        specforge::PrepareSampleWorkflowState(
            *initial_snapshot,
            initial_context,
            0,
            {{}, {}});
    Require(
        session.OpenPreparedSource(
                   path,
                   0,
                   initial_snapshot,
                   std::move(initial_context),
                   std::move(initial_workflow))
            .loaded,
        "source A should enter the session before its async follow-up");

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
    const specforge::SourceCollectionSessionResult navigation = session.Submit(
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    Require(
        navigation.follow_up_spectrum_index == 1,
        "source A should issue the async row 1 follow-up used by the Shell ticket");

    bool session_navigation_preserved = false;
    bool shell_ticket_preserved = false;
    bool pending_intent_restored = false;
    bool existing_ticket_reused = false;
    bool completion_admitted = false;
    bool row_one_committed = false;
    {
        specforge::SourceCollectionLoadDependencies dependencies;
        dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
            return MakeSnapshot(source, index);
        };
        dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
        dependencies.workflow_cache_paths = {{}, {}};
        specforge::SourceCollectionLoadQueue queue(std::move(dependencies));

        constexpr std::uint64_t generation = 4;
        std::uint64_t activation_epoch = 20;
        const std::uint64_t task_id = queue.Enqueue({
            .path = path,
            .spectrum_index = 1,
            .reuse_identity = initial_identity,
        });
        const std::string path_key = specforge::SourcePathIdentityKey(path);
        std::unordered_map<std::uint64_t, Access::PendingLoad> pending_loads;
        pending_loads.emplace(
            task_id,
            Access::PendingLoad{
                .path = path,
                .path_key = path_key,
                .spectrum_index = 1,
                .generation = generation,
                .activation_epoch = activation_epoch,
                .purpose = Access::Purpose::SessionFollowUp,
            });
        std::unordered_set<std::uint64_t> deferred_restore_task_ids;
        const std::unordered_map<std::string, std::uint64_t> generations{{path_key, generation}};
        std::vector<specforge::SourceCollectionLoadCompletion> completions = WaitForCompletions(queue);
        Require(
            completions.size() == 1 && completions.front().prepared.has_value(),
            "background queue should produce the follow-up before Shell drains it");

        const specforge::SourceCollectionSessionResult switched_to_b = session.OpenPreparedSource(
            other_path,
            0,
            other_snapshot,
            std::move(other_context),
            std::move(other_workflow));
        session_navigation_preserved =
            switched_to_b.loaded && !switched_to_b.canceled_source_follow_up_path;

        std::vector<std::uint64_t> canceled;
        Access::CancelSourceFollowUps(
            switched_to_b,
            pending_loads,
            deferred_restore_task_ids,
            [&queue, &canceled](std::uint64_t canceled_id) {
                canceled.push_back(canceled_id);
                queue.Cancel(canceled_id);
            });
        shell_ticket_preserved =
            canceled.empty() && pending_loads.contains(task_id) && deferred_restore_task_ids.empty();
        const specforge::SourceCollectionSessionResult switched_back_to_a =
            session.Submit(specforge::SourceCollectionSessionIntent::EditSourceCollection(
                specforge::SourceCollectionIntent::SwitchActive(0)));
        pending_intent_restored =
            switched_back_to_a.follow_up_spectrum_index == 1 &&
            session.CurrentSampleSnapshot() == initial_snapshot;
        existing_ticket_reused = switched_back_to_a.follow_up_spectrum_index &&
            Access::HasMatchingFollowUp(
                path,
                *switched_back_to_a.follow_up_spectrum_index,
                pending_loads);
        completion_admitted = Access::TakeCurrent(
            completions.front(),
            activation_epoch,
            pending_loads,
            generations).has_value();
        if (completion_admitted) {
            specforge::PreparedSourceCollection prepared = std::move(*completions.front().prepared);
            row_one_committed = session.OpenPreparedSource(
                prepared.path,
                prepared.spectrum_index,
                std::move(prepared.snapshot),
                std::move(prepared.payload)).loaded;
        } else {
            queue.RetirePrepared(std::move(*completions.front().prepared));
        }
    }
    std::filesystem::remove(path);

    Require(
        session_navigation_preserved,
        "source B's deferred completion must not cancel source A's navigation intent");
    Require(
        shell_ticket_preserved,
        "source B's deferred completion must retain source A's Shell ticket and restore bookkeeping");
    Require(
        pending_intent_restored,
        "switching back to A should expose its still-pending row 1 intent");
    Require(
        existing_ticket_reused,
        "restoring A should reuse its existing row 1 Shell ticket instead of restarting the worker");
    Require(
        completion_admitted,
        "source A's already-completed worker result should retain admission after B restores");
    Require(
        row_one_committed && session.CurrentSampleSnapshot() &&
            session.CurrentSampleSnapshot()->collection.current_index == 1,
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

    specforge::SourceCollectionLoadDependencies dependencies;
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
        specforge::SourceCollectionLoadQueue(std::move(dependencies)));

    const specforge::SourceCollectionSessionResult navigation = Access::Session(*shell).Submit(
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool deferred_follow_up_created = navigation.follow_up_spectrum_index == 1;
    Access::QueueFollowUp(*shell, navigation, true);

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

}  // namespace

int main()
{
    try {
        TestLaterExplicitOpenCancelsEarlierFollowUp();
        TestRemovedSourceResultCancelsOnlyThatSourcesDerivedTickets();
        TestRealDrainCommitsOnlyTheLatestRapidNavigation();
        TestPublishedStaleCompletionIsRejectedWithoutMutatingNewNavigation();
        TestRealDrainPreservesWorkflowChangesMadeWhileFullPlanWaits();
        TestRealDrainRequeuesReconciledTargetAndRetiresIntermediateSnapshotOffThread();
        TestDeferredRestoreCompletionPreservesUnrelatedNavigationTicket();
        TestDeferredRestoreFollowUpFailureClearsPendingAndAllowsRetry();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
