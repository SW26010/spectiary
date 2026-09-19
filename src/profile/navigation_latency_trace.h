#pragma once

#include "profile/load_latency_trace_lifecycle.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace spectiary {

class ProfileSink;

using NavigationLatencyClock = LoadLatencyClock;
using NavigationLatencyTimePoint = LoadLatencyTimePoint;
using NavigationLatencyFolderListingObservation =
    LoadLatencyFolderListingObservation;
using NavigationLatencyPreparationRoundReport =
    LoadLatencyPreparationRoundReport;
using NavigationLatencyAttemptReport = LoadLatencyAttemptReport;
using NavigationLatencyAttempt = LoadLatencyAttempt;
using NavigationLatencyAttemptHandle = LoadLatencyAttemptHandle;

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

enum class NavigationSnapshotCacheKind : std::uint8_t {
    None,
    History,
    Prefetch,
};

struct NavigationLatencyPresentation {
    unsigned int viewport_id = 0;
    NavigationLatencyTimePoint completed_at;
};

struct NavigationTargetResolutionReport {
    std::int64_t effective_index_ns = 0;
    std::int64_t pending_activation_supersede_ns = 0;
    std::int64_t base_sequence_ns = 0;
    std::int64_t target_lookup_ns = 0;
    std::int64_t target_sequence_ns = 0;
    std::int64_t navigation_state_result_ns = 0;

    std::size_t row_count = 0;
    bool filter_active = false;
    bool sort_active = false;
    bool query_active = false;
    bool pending_present = false;
    bool sequence_cache_hit = false;
    std::size_t sequence_build_count = 0;
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
    NavigationSnapshotCacheKind cache_kind =
        NavigationSnapshotCacheKind::None;
    NavigationTargetResolutionReport target_resolution;
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
        NavigationLatencyTimePoint target_resolved_at,
        NavigationTargetResolutionReport target_resolution = {});

    [[nodiscard]] static NavigationLatencyTimePoint Now() noexcept;

    void SetTargetIndex(std::size_t target_index) noexcept;
    void SetCacheHit(bool cache_hit) noexcept;
    void SetCacheKind(NavigationSnapshotCacheKind cache_kind) noexcept;
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
    std::uint64_t navigation_id_ = 0;
    std::size_t from_index_ = 0;
    NavigationLatencyInputKind input_kind_ = NavigationLatencyInputKind::UiNext;
    std::atomic<NavigationSnapshotCacheKind> cache_kind_ =
        NavigationSnapshotCacheKind::None;
    NavigationTargetResolutionReport target_resolution_;
    std::int64_t input_ns_ = 0;
    std::int64_t requested_ns_ = 0;
    std::int64_t target_resolved_ns_ = 0;

    LoadLatencyAttemptLifecycle load_attempts_;
    profile_internal::LoadLatencyTerminalPresentationLifecycle<
        NavigationLatencyOutcome>
        terminal_presentation_;
};

using NavigationLatencyTraceHandle = std::shared_ptr<NavigationLatencyTrace>;

[[nodiscard]] const char* NavigationLatencyInputKindName(NavigationLatencyInputKind kind) noexcept;
[[nodiscard]] const char* NavigationLatencyOutcomeName(NavigationLatencyOutcome outcome) noexcept;
[[nodiscard]] const char* NavigationSnapshotCacheKindName(
    NavigationSnapshotCacheKind kind) noexcept;
bool WriteNavigationLatencyProfileEvent(ProfileSink& sink, const NavigationLatencyReport& report);

}  // namespace spectiary
