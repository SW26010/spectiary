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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

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

    struct ServiceResult {
        SourceCollectionSessionAction action;
        bool session_changed = false;
    };

    using BeforeSourceActivation =
        std::function<std::vector<BackgroundRetirementHandle>()>;
    using ConsumeSessionAction =
        std::function<void(const SourceCollectionSessionAction&)>;

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
    [[nodiscard]] ServiceResult Drain(
        bool allow_snapshot_prefetch,
        BeforeSourceActivation before_source_activation = {},
        ConsumeSessionAction consume_session_action = {});

    void SetPresentationContext(
        bool latency_tracing_enabled,
        std::uint64_t frame_index);
    void RecordSpectrumDrawSubmission(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        SpectrumSnapshotHandle snapshot);
    [[nodiscard]] std::vector<NavigationLatencyReport>
        CompleteFramePresentations(
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

    void RegisterCompletionReadyCallback(
        SourceCollectionLoadQueue::CompletionReadyCallback callback);
    void UnregisterCompletionReadyCallback();
    [[nodiscard]] bool NeedsService() const;
    [[nodiscard]] bool HasPendingLoads() const;
    [[nodiscard]] std::size_t PendingLoadCount() const;
    [[nodiscard]] bool PrefetchActive() const;
    [[nodiscard]] std::string_view ErrorMessage() const;

    void RetireResource(BackgroundRetirementHandle resource);

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
        const BeforeSourceActivation& before_source_activation,
        const ConsumeSessionAction& consume_session_action,
        ServiceResult& service_result);

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
        const ConsumeSessionAction& consume_session_action,
        ServiceResult& service_result);
    void FinishDeferredRestoreIfReady(
        const ConsumeSessionAction& consume_session_action,
        ServiceResult& service_result);

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
    static void MarkTicketSuperseded(const Ticket& ticket);

    SourceCollectionSession& session_;
    SourceCollectionLoadQueue load_queue_;
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
    std::string error_message_;

    bool latency_tracing_enabled_ = false;
    std::uint64_t current_frame_index_ = 0;
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
};

}  // namespace specforge
