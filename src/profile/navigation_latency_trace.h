#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace specforge {

class ProfileSink;

using NavigationLatencyClock = std::chrono::steady_clock;
using NavigationLatencyTimePoint = NavigationLatencyClock::time_point;

enum class NavigationLatencyInputKind : std::uint8_t {
    KeyboardPrevious,
    KeyboardNext,
    UiPrevious,
    UiNext,
    AutoAdvance,
};

enum class NavigationLatencyOutcome : std::uint8_t {
    Pending,
    Presented,
    Failed,
    Rejected,
    Superseded,
    Coalesced,
};

struct NavigationLatencyFolderListingObservation {
    bool hint_present = false;
    bool generation_current_at_start = false;
    bool listing_scan_performed = false;
};

struct NavigationLatencyPreparationRoundReport {
    std::size_t round_index = 0;
    bool source_is_folder = false;
    bool hint_present = false;
    bool generation_current_at_start = false;
    bool listing_scan_performed = false;
    bool context_reused = false;
    bool revalidation_succeeded = false;
    std::int64_t preparation_started_ns = 0;
    std::int64_t snapshot_load_started_ns = 0;
    std::int64_t snapshot_load_finished_ns = 0;
    std::int64_t context_prepared_ns = 0;
    std::int64_t source_revalidated_ns = 0;
};

struct NavigationLatencyAttemptReport {
    std::size_t attempt_index = 0;
    std::size_t target_index = 0;
    std::uint64_t source_task_id = 0;
    bool source_is_folder = false;
    bool workflow_reused = false;
    bool context_reused = false;

    std::int64_t load_enqueued_ns = 0;
    std::int64_t worker_started_ns = 0;
    std::int64_t snapshot_load_started_ns = 0;
    std::int64_t snapshot_load_finished_ns = 0;
    std::int64_t context_prepared_ns = 0;
    std::int64_t source_revalidated_ns = 0;
    std::int64_t worker_prepared_ns = 0;
    std::int64_t completion_ready_ns = 0;
    std::int64_t completion_published_ns = 0;
    std::int64_t completion_drained_ns = 0;
    std::vector<NavigationLatencyPreparationRoundReport> preparation_rounds;
};

class NavigationLatencyAttempt {
public:
    void MarkSourceTaskId(std::uint64_t task_id) noexcept;
    void MarkWorkerStarted(NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;
    void MarkSnapshotLoadStarted(
        bool source_is_folder,
        NavigationLatencyTimePoint at = NavigationLatencyClock::now());
    void MarkFolderSnapshotLoadStarted(
        NavigationLatencyFolderListingObservation observation,
        NavigationLatencyTimePoint at = NavigationLatencyClock::now());
    void MarkFolderListingScanPerformed() noexcept;
    void MarkSnapshotLoadFinished(NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;
    void MarkContextPrepared(NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;
    void MarkContextPrepared(
        bool context_reused,
        NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;
    void MarkSourceRevalidated(NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;
    void MarkSourceRevalidated(
        bool succeeded,
        NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;
    void MarkWorkflowReused(bool reused) noexcept;
    void MarkWorkerPrepared(NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;
    void MarkCompletionReady(NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;
    void MarkCompletionPublished(NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;
    void MarkCompletionDrained(NavigationLatencyTimePoint at = NavigationLatencyClock::now()) noexcept;

    [[nodiscard]] NavigationLatencyAttemptReport Report() const noexcept;

private:
    friend class NavigationLatencyTrace;

    NavigationLatencyAttempt(
        std::size_t attempt_index,
        std::size_t target_index,
        NavigationLatencyTimePoint load_enqueued_at);
    [[nodiscard]] static std::int64_t ToNanoseconds(NavigationLatencyTimePoint at) noexcept;
    void MarkSnapshotLoadStarted(
        bool source_is_folder,
        NavigationLatencyFolderListingObservation observation,
        NavigationLatencyTimePoint at);

    std::size_t attempt_index_ = 0;
    std::size_t target_index_ = 0;
    std::int64_t load_enqueued_ns_ = 0;
    std::atomic_uint64_t source_task_id_ = 0;
    std::atomic_bool source_is_folder_ = false;
    std::atomic_bool workflow_reused_ = false;
    std::atomic_bool context_reused_ = false;
    std::atomic_int64_t worker_started_ns_ = 0;
    std::atomic_int64_t snapshot_load_started_ns_ = 0;
    std::atomic_int64_t snapshot_load_finished_ns_ = 0;
    std::atomic_int64_t context_prepared_ns_ = 0;
    std::atomic_int64_t source_revalidated_ns_ = 0;
    std::atomic_int64_t worker_prepared_ns_ = 0;
    std::atomic_int64_t completion_ready_ns_ = 0;
    std::atomic_int64_t completion_published_ns_ = 0;
    std::atomic_int64_t completion_drained_ns_ = 0;
    mutable std::mutex preparation_rounds_mutex_;
    std::vector<NavigationLatencyPreparationRoundReport> preparation_rounds_;
};

using NavigationLatencyAttemptHandle = std::shared_ptr<NavigationLatencyAttempt>;

struct NavigationLatencyPresentation {
    unsigned int viewport_id = 0;
    NavigationLatencyTimePoint completed_at;
};

struct NavigationLatencyReport {
    std::uint64_t navigation_id = 0;
    std::uint64_t activation_frame = 0;
    unsigned int presentation_viewport_id = 0;
    std::size_t from_index = 0;
    std::size_t target_index = 0;
    NavigationLatencyInputKind input_kind = NavigationLatencyInputKind::UiNext;
    NavigationLatencyOutcome outcome = NavigationLatencyOutcome::Pending;
    bool cache_hit = false;
    std::vector<NavigationLatencyAttemptReport> attempts;

    std::int64_t input_ns = 0;
    std::int64_t requested_ns = 0;
    std::int64_t target_resolved_ns = 0;
    std::int64_t snapshot_activated_ns = 0;
    std::int64_t ui_updated_ns = 0;
    std::int64_t first_present_ns = 0;
    std::int64_t terminal_ns = 0;
};

class NavigationLatencyTrace {
public:
    NavigationLatencyTrace(
        std::uint64_t navigation_id,
        std::size_t from_index,
        std::size_t target_index,
        NavigationLatencyInputKind input_kind,
        NavigationLatencyTimePoint input_at,
        NavigationLatencyTimePoint requested_at,
        NavigationLatencyTimePoint target_resolved_at);

    [[nodiscard]] static NavigationLatencyTimePoint Now() noexcept;

    void SetTargetIndex(std::size_t target_index) noexcept;
    [[nodiscard]] NavigationLatencyAttemptHandle BeginLoadAttempt(
        std::size_t target_index,
        NavigationLatencyTimePoint at = Now());
    void MarkSnapshotActivated(
        std::uint64_t frame_index,
        NavigationLatencyTimePoint at = Now()) noexcept;
    void MarkUiUpdated(NavigationLatencyTimePoint at = Now()) noexcept;

    [[nodiscard]] bool MarkPresentedForViewport(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        NavigationLatencyTimePoint at = Now()) noexcept;
    [[nodiscard]] bool MarkTerminal(
        NavigationLatencyOutcome outcome,
        NavigationLatencyTimePoint at = Now()) noexcept;
    [[nodiscard]] std::optional<NavigationLatencyReport> TerminalReport() const noexcept;

private:
    [[nodiscard]] static std::int64_t ToNanoseconds(NavigationLatencyTimePoint at) noexcept;

    std::uint64_t navigation_id_ = 0;
    std::size_t from_index_ = 0;
    NavigationLatencyInputKind input_kind_ = NavigationLatencyInputKind::UiNext;
    bool cache_hit_ = false;
    std::int64_t input_ns_ = 0;
    std::int64_t requested_ns_ = 0;
    std::int64_t target_resolved_ns_ = 0;

    std::atomic_size_t target_index_ = 0;
    std::atomic_int64_t snapshot_activated_ns_ = 0;
    std::atomic_int64_t ui_updated_ns_ = 0;

    mutable std::mutex attempts_mutex_;
    std::vector<NavigationLatencyAttemptHandle> attempts_;

    mutable std::mutex terminal_mutex_;
    NavigationLatencyOutcome outcome_ = NavigationLatencyOutcome::Pending;
    std::uint64_t activation_frame_ = 0;
    unsigned int presentation_viewport_id_ = 0;
    std::int64_t first_present_ns_ = 0;
    std::int64_t terminal_ns_ = 0;
};

using NavigationLatencyTraceHandle = std::shared_ptr<NavigationLatencyTrace>;

[[nodiscard]] const char* NavigationLatencyInputKindName(NavigationLatencyInputKind kind) noexcept;
[[nodiscard]] const char* NavigationLatencyOutcomeName(NavigationLatencyOutcome outcome) noexcept;
bool WriteNavigationLatencyProfileEvent(ProfileSink& sink, const NavigationLatencyReport& report);

}  // namespace specforge
