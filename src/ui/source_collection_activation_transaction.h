#pragma once

#include "profile/navigation_latency_trace.h"
#include "profile/navigation_prefetch_trace.h"
#include "profile/source_load_latency_trace.h"
#include "ui/background_retirement.h"
#include "ui/source_collection_load_queue.h"
#include "ui/source_collection_session.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

class ProfileSink;
class SpectrumViewSession;

// Owns the complete asynchronous source-activation protocol. Callers submit
// source/session intent and consume the resulting session action; task IDs,
// generations, cancellation, deferred restore, speculative prefetch, and trace
// terminal correlation remain local to this implementation.
class SourceCollectionActivationTransaction {
public:
    struct NavigationIntent {
        NavigationLatencyInputKind kind =
            NavigationLatencyInputKind::UiNext;
        std::optional<NavigationLatencyTimePoint> input_at;
    };

    struct Status {
        bool loading = false;
        std::string_view error_message;
    };

    SourceCollectionActivationTransaction(
        SourceCollectionSession& session,
        SourceCollectionLoadQueue load_queue =
            SourceCollectionLoadQueue{});
    ~SourceCollectionActivationTransaction();

    SourceCollectionActivationTransaction(
        const SourceCollectionActivationTransaction&) = delete;
    SourceCollectionActivationTransaction& operator=(
        const SourceCollectionActivationTransaction&) = delete;
    SourceCollectionActivationTransaction(
        SourceCollectionActivationTransaction&&) = delete;
    SourceCollectionActivationTransaction& operator=(
        SourceCollectionActivationTransaction&&) = delete;

    // Returns true when canceling an older pending sample navigation mutated
    // the session.
    [[nodiscard]] bool OpenSource(
        const std::filesystem::path& path,
        std::size_t spectrum_index = 0);
    [[nodiscard]] SourceCollectionSessionResult Submit(
        SourceCollectionSessionIntent intent,
        std::optional<NavigationIntent> navigation = std::nullopt);

    void BeginDeferredRestore();
    [[nodiscard]] SourceCollectionSessionAction Drain(
        bool allow_snapshot_prefetch);

    void BeginFrame(
        bool latency_tracing_enabled,
        std::uint64_t frame_index,
        ProfileSink* profile);
    void RecordSpectrumDrawSubmission(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        SpectrumSnapshotHandle snapshot);
    void PresentFrame(
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation>
            presentations);

    [[nodiscard]] Status status() const;
    // Hides the currently failed generations from the UI projection while
    // retaining their terminal outcomes.
    void AcknowledgeLoadFailures();

private:
    enum class Purpose {
        ExplicitOpen,
        SessionFollowUp,
        DeferredRestore,
    };

    struct Ticket {
        std::filesystem::path path;
        std::string path_key;
        std::size_t spectrum_index = 0;
        std::uint64_t generation = 0;
        std::uint64_t activation_epoch = 0;
        Purpose purpose = Purpose::ExplicitOpen;
        NavigationLatencyTraceHandle navigation_trace;
        SourceLoadLatencyTraceHandle source_load_trace;
        std::optional<SampleNavigationDirection>
            prefetch_direction;
    };

    struct PendingTask {
        std::uint64_t task_id = 0;
        Ticket ticket;
    };

    struct CompletionAdmission {
        Ticket ticket;
        bool accepted = false;
    };

    struct TerminalOutcome {
        std::filesystem::path path;
        std::uint64_t generation = 0;
        std::optional<std::string> error_message;
        bool failure_acknowledged = false;
    };

    struct PendingSnapshotPrefetch {
        std::uint64_t prefetch_id = 0;
        std::uint64_t task_id = 0;
        std::filesystem::path path;
        std::string path_key;
        std::size_t spectrum_index = 0;
        std::uint64_t generation = 0;
        std::uint64_t activation_epoch = 0;
        SampleNavigationDirection direction =
            SampleNavigationDirection::Next;
        NavigationLatencyTimePoint scheduled_at;
        NavigationLatencyTimePoint cancel_requested_at;
        bool invalidated = false;
    };

    struct SpectrumDrawSubmission {
        std::uint64_t frame_index = 0;
        unsigned int viewport_id = 0;
        SpectrumSnapshotHandle snapshot;
    };

    [[nodiscard]] std::uint64_t QueueSourceLoad(
        const std::filesystem::path& path,
        std::size_t spectrum_index,
        std::vector<std::filesystem::path> annotation_paths,
        Purpose purpose,
        NavigationLatencyTraceHandle navigation_trace = {},
        SourceLoadLatencyTraceHandle source_load_trace = {},
        std::optional<SampleNavigationDirection> prefetch_direction =
            std::nullopt);
    void BeginActivationIntent(bool preserve_pending_explicit_opens);
    void CancelPendingTasks(std::vector<PendingTask> pending_tasks);
    void QueueSessionFollowUp(
        const SourceCollectionSessionResult& result,
        bool deferred_restore = false,
        NavigationLatencyTraceHandle navigation_trace = {},
        std::optional<SampleNavigationDirection> prefetch_direction =
            std::nullopt);
    void CancelSourceFollowUps(
        const SourceCollectionSessionResult& result);
    void RetireSessionResources(SourceCollectionSessionResult& result);
    void DrainCompletions(
        std::vector<SourceCollectionLoadCompletion> completions,
        SourceCollectionSessionAction& action);
    void ApplyPresentationAction(
        const SourceCollectionSessionAction& action);
    void BindPresentationLifecycle(
        std::function<
            std::vector<BackgroundRetirementHandle>()>
            retain_resources,
        std::function<void()> reset_for_snapshot_change);
    void RetirePendingSessionViews();
    void RetireResources(
        std::vector<BackgroundRetirementHandle> resources);
    void RunMaintenance(
        LocalUserStateSaveScheduler::TimePoint now);
    [[nodiscard]] std::optional<
        LocalUserStateSaveScheduler::TimePoint>
        NextMaintenanceDeadline() const;
    void ScheduleServiceNow();
    void RefreshServiceDeadline(
        LocalUserStateSaveScheduler::TimePoint now);
    void RegisterCompletionReadyCallback(
        SourceCollectionLoadQueue::CompletionReadyCallback callback);
    void UnregisterCompletionReadyCallback();

    void ScheduleSnapshotPrefetch(
        SampleNavigationDirection direction);
    void ServiceSnapshotPrefetch(bool allow_snapshot_prefetch);
    void CancelSnapshotPrefetch();
    void DrainSnapshotPrefetchCompletion(
        SourceCollectionLoadCompletion completion);
    void RecordSnapshotPrefetchOutcome(
        std::uint64_t prefetch_id,
        std::uint64_t source_task_id,
        std::size_t target_index,
        SampleNavigationDirection direction,
        NavigationPrefetchOutcome outcome,
        NavigationLatencyTimePoint scheduled_at,
        NavigationLatencyTimePoint terminal_at = {},
        NavigationLatencyTimePoint cancel_requested_at = {});
    [[nodiscard]] static std::optional<SampleNavigationDirection>
        PrefetchDirectionForInputKind(
            NavigationLatencyInputKind kind);

    void RestoreDeferredActiveSourceIfAvailable(
        SourceCollectionSessionAction& action);
    void FinishDeferredRestoreIfReady(
        SourceCollectionSessionAction& action);

    [[nodiscard]] std::vector<NavigationLatencyReport>
        CompleteNavigationFramePresentations(
            std::uint64_t frame_index,
            std::span<const NavigationLatencyPresentation>
                presentations);
    [[nodiscard]] std::vector<SourceLoadLatencyReport>
        CompleteSourceLoadFramePresentations(
            std::uint64_t frame_index,
            std::span<const NavigationLatencyPresentation>
                presentations);
    [[nodiscard]] std::vector<NavigationPrefetchReport>
        TakeNavigationPrefetchReports();
    [[nodiscard]] bool NeedsService() const;
    [[nodiscard]] bool HasPendingLoads() const;
    [[nodiscard]] std::size_t PendingLoadCount() const;
    [[nodiscard]] bool PrefetchActive() const;
    [[nodiscard]] std::string_view ErrorMessage() const;

    [[nodiscard]] NavigationLatencyTraceHandle StartNavigationTrace(
        const SourceCollectionSessionResult& result,
        std::optional<std::size_t> from_index,
        NavigationLatencyTimePoint requested_at,
        NavigationLatencyTimePoint target_resolved_at,
        NavigationTargetResolutionReport target_resolution,
        std::optional<NavigationIntent> navigation);
    [[nodiscard]] SourceLoadLatencyTraceHandle StartSourceLoadTrace(
        std::size_t target_index,
        NavigationLatencyTimePoint accepted_at);
    void SupersedePresentableNavigationIfSnapshotChanged(
        const SpectrumSnapshotHandle& current_snapshot);
    void SupersedePresentableSourceLoadIfSnapshotChanged(
        const SpectrumSnapshotHandle& current_snapshot);
    [[nodiscard]] static bool CancelFailedPendingSampleNavigation(
        SourceCollectionSession& session,
        const Ticket& ticket);

    [[nodiscard]] Ticket ReserveLoad(
        const std::filesystem::path& path,
        std::size_t spectrum_index,
        Purpose purpose,
        NavigationLatencyTraceHandle navigation_trace = {},
        SourceLoadLatencyTraceHandle source_load_trace = {},
        std::optional<SampleNavigationDirection> prefetch_direction =
            std::nullopt);
    void RegisterLoad(std::uint64_t task_id, Ticket ticket);
    [[nodiscard]] std::vector<PendingTask> RegisterOrReplaceLoad(
        std::uint64_t task_id,
        Ticket ticket);
    [[nodiscard]] std::vector<PendingTask> AdvanceIntent(
        bool preserve_pending_explicit_opens);
    [[nodiscard]] std::optional<CompletionAdmission> TakeCompletion(
        std::uint64_t task_id,
        const std::filesystem::path& path,
        std::size_t spectrum_index);
    [[nodiscard]] std::vector<PendingTask>
        CancelNonExplicitFollowUps(
            const std::filesystem::path& path);
    [[nodiscard]] bool HasMatchingFollowUp(
        const std::filesystem::path& path,
        std::size_t spectrum_index) const;
    [[nodiscard]] std::optional<std::uint64_t> GenerationForPath(
        const std::filesystem::path& path) const;
    [[nodiscard]] std::uint64_t ActivationEpoch() const;
    [[nodiscard]] bool HasPendingDeferredRestore() const;
    [[nodiscard]] std::vector<PendingTask> RemoveLoadsForPath(
        std::string_view path_key,
        bool retain_explicit_opens);
    void EraseDeferredRestoreTask(std::uint64_t task_id);
    void RecordTerminalOutcome(
        const Ticket& ticket,
        std::optional<std::string> error_message);
    void RebuildErrorMessage();
    static void MarkTicketSuperseded(const Ticket& ticket);

    SourceCollectionSession& session_;
    SourceCollectionLoadQueue load_queue_;
    std::function<
        std::vector<BackgroundRetirementHandle>()>
        retain_presentation_resources_;
    std::function<void()>
        reset_presentation_for_snapshot_change_;
    std::unordered_map<std::uint64_t, Ticket> pending_loads_;
    std::unordered_map<std::string, std::uint64_t> generations_;
    std::unordered_set<std::uint64_t>
        deferred_restore_task_ids_;
    std::uint64_t activation_epoch_ = 0;

    std::optional<SampleNavigationDirection>
        pending_snapshot_prefetch_direction_;
    std::optional<PendingSnapshotPrefetch>
        active_snapshot_prefetch_;
    SampleNavigationPrefetchPolicy snapshot_prefetch_policy_;
    std::uint64_t next_snapshot_prefetch_id_ = 1;
    std::vector<NavigationPrefetchReport>
        navigation_prefetch_reports_;
    std::optional<std::filesystem::path>
        deferred_restore_active_path_;
    bool deferred_restore_active_ = false;
    std::map<std::string, TerminalOutcome>
        terminal_outcomes_;
    std::string error_message_;
    mutable std::optional<
        LocalUserStateSaveScheduler::TimePoint>
        service_deadline_;

    bool latency_tracing_enabled_ = false;
    std::uint64_t current_frame_index_ = 0;
    ProfileSink* profile_ = nullptr;
    std::uint64_t next_navigation_trace_id_ = 1;
    std::uint64_t next_source_load_trace_id_ = 1;
    std::optional<SpectrumDrawSubmission>
        spectrum_draw_submission_;
    NavigationLatencyTraceHandle
        presentable_navigation_trace_;
    SpectrumSnapshotHandle
        presentable_navigation_snapshot_;
    std::unordered_map<
        std::uint64_t,
        NavigationLatencyTraceHandle>
        navigation_traces_;
    SourceLoadLatencyTraceHandle
        presentable_source_load_trace_;
    SpectrumSnapshotHandle
        presentable_source_load_snapshot_;
    std::unordered_map<
        std::uint64_t,
        SourceLoadLatencyTraceHandle>
        source_load_traces_;

    friend struct ShellUiTestAccess;
    friend struct SourceCollectionActivationTransactionTestAccess;
    friend class ShellUi;
    friend void BindSourceCollectionActivationPresentationLifecycle(
        SourceCollectionActivationTransaction& activation,
        SpectrumViewSession& presentation);
};

}  // namespace specforge
