#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

namespace specforge {

using LoadLatencyClock = std::chrono::steady_clock;
using LoadLatencyTimePoint = LoadLatencyClock::time_point;

[[nodiscard]] std::int64_t LoadLatencyNanoseconds(
    LoadLatencyTimePoint at) noexcept;

struct LoadLatencyFolderListingObservation {
    bool hint_present = false;
    bool generation_current_at_start = false;
    bool listing_scan_performed = false;
};

struct LoadLatencyPreparationRoundReport {
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

struct LoadLatencyAttemptReport {
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
    std::vector<LoadLatencyPreparationRoundReport> preparation_rounds;
};

class LoadLatencyAttempt {
public:
    void SetTargetIndex(std::size_t target_index) noexcept;
    void MarkSourceTaskId(std::uint64_t task_id) noexcept;
    void MarkWorkerStarted(
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkSnapshotLoadStarted(
        bool source_is_folder,
        LoadLatencyTimePoint at = LoadLatencyClock::now());
    void MarkFolderSnapshotLoadStarted(
        LoadLatencyFolderListingObservation observation,
        LoadLatencyTimePoint at = LoadLatencyClock::now());
    void MarkFolderListingScanPerformed() noexcept;
    void MarkSnapshotLoadFinished(
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkContextPrepared(
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkContextPrepared(
        bool context_reused,
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkSourceRevalidated(
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkSourceRevalidated(
        bool succeeded,
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkWorkflowReused(bool reused) noexcept;
    void MarkWorkerPrepared(
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkCompletionReady(
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkCompletionPublished(
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkCompletionDrained(
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;

    [[nodiscard]] LoadLatencyAttemptReport Report() const noexcept;

private:
    friend class LoadLatencyAttemptLifecycle;

    LoadLatencyAttempt(
        std::size_t attempt_index,
        std::size_t target_index,
        LoadLatencyTimePoint load_enqueued_at);
    void MarkSnapshotLoadStarted(
        bool source_is_folder,
        LoadLatencyFolderListingObservation observation,
        LoadLatencyTimePoint at);

    std::size_t attempt_index_ = 0;
    std::atomic_size_t target_index_ = 0;
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
    std::vector<LoadLatencyPreparationRoundReport> preparation_rounds_;
};

using LoadLatencyAttemptHandle = std::shared_ptr<LoadLatencyAttempt>;

class LoadLatencyAttemptLifecycle {
public:
    [[nodiscard]] LoadLatencyAttemptHandle Begin(
        std::size_t target_index,
        LoadLatencyTimePoint at);
    [[nodiscard]] std::vector<LoadLatencyAttemptReport> Reports() const noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<LoadLatencyAttemptHandle> attempts_;
};

namespace profile_internal {

template <typename Outcome>
struct LoadLatencyTerminalPresentationSnapshot {
    std::size_t target_index = 0;
    Outcome outcome = Outcome::Pending;
    std::uint64_t activation_frame = 0;
    unsigned int presentation_viewport_id = 0;
    std::int64_t snapshot_activated_ns = 0;
    std::int64_t ui_updated_ns = 0;
    std::int64_t first_present_ns = 0;
    std::int64_t terminal_ns = 0;
};

template <typename Outcome>
class LoadLatencyTerminalPresentationLifecycle {
    static_assert(std::is_enum_v<Outcome>);

public:
    explicit LoadLatencyTerminalPresentationLifecycle(
        std::size_t target_index) noexcept
        : target_index_(target_index)
    {
    }

    void SetTargetIndex(std::size_t target_index) noexcept
    {
        target_index_.store(target_index, std::memory_order_relaxed);
    }

    void MarkSnapshotActivated(
        std::uint64_t frame_index,
        LoadLatencyTimePoint at) noexcept
    {
        snapshot_activated_ns_.store(
            LoadLatencyNanoseconds(at),
            std::memory_order_relaxed);
        std::lock_guard lock(terminal_mutex_);
        activation_frame_ = frame_index;
    }

    void MarkUiUpdated(LoadLatencyTimePoint at) noexcept
    {
        ui_updated_ns_.store(
            LoadLatencyNanoseconds(at),
            std::memory_order_relaxed);
    }

    [[nodiscard]] bool MarkPresentedForViewport(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        LoadLatencyTimePoint at) noexcept
    {
        std::lock_guard lock(terminal_mutex_);
        if (outcome_ != Outcome::Pending || activation_frame_ == 0 ||
            frame_index < activation_frame_ || viewport_id == 0) {
            return false;
        }
        presentation_viewport_id_ = viewport_id;
        first_present_ns_ = LoadLatencyNanoseconds(at);
        terminal_ns_ = first_present_ns_;
        outcome_ = Outcome::Presented;
        return true;
    }

    [[nodiscard]] bool MarkTerminal(
        Outcome outcome,
        LoadLatencyTimePoint at) noexcept
    {
        if (outcome == Outcome::Pending || outcome == Outcome::Presented) {
            return false;
        }
        std::lock_guard lock(terminal_mutex_);
        if (outcome_ != Outcome::Pending) {
            return false;
        }
        outcome_ = outcome;
        terminal_ns_ = LoadLatencyNanoseconds(at);
        return true;
    }

    [[nodiscard]] std::optional<
        LoadLatencyTerminalPresentationSnapshot<Outcome>>
    TerminalSnapshot() const noexcept
    {
        LoadLatencyTerminalPresentationSnapshot<Outcome> snapshot;
        {
            std::lock_guard lock(terminal_mutex_);
            if (outcome_ == Outcome::Pending) {
                return std::nullopt;
            }
            snapshot.activation_frame = activation_frame_;
            snapshot.presentation_viewport_id = presentation_viewport_id_;
            snapshot.outcome = outcome_;
            snapshot.first_present_ns = first_present_ns_;
            snapshot.terminal_ns = terminal_ns_;
        }
        snapshot.target_index =
            target_index_.load(std::memory_order_relaxed);
        snapshot.snapshot_activated_ns =
            snapshot_activated_ns_.load(std::memory_order_relaxed);
        snapshot.ui_updated_ns =
            ui_updated_ns_.load(std::memory_order_relaxed);
        return snapshot;
    }

private:
    std::atomic_size_t target_index_ = 0;
    std::atomic_int64_t snapshot_activated_ns_ = 0;
    std::atomic_int64_t ui_updated_ns_ = 0;

    mutable std::mutex terminal_mutex_;
    Outcome outcome_ = Outcome::Pending;
    std::uint64_t activation_frame_ = 0;
    unsigned int presentation_viewport_id_ = 0;
    std::int64_t first_present_ns_ = 0;
    std::int64_t terminal_ns_ = 0;
};

}  // namespace profile_internal

}  // namespace specforge
