#pragma once

#include "profile/navigation_latency_trace.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace specforge {

class ProfileSink;

enum class SourceLoadLatencyRequestKind : std::uint8_t {
    ExplicitOpen,
};

enum class SourceLoadLatencyOutcome : std::uint8_t {
    Pending,
    Presented,
    Failed,
    Rejected,
    Superseded,
};

struct SourceLoadLatencyReport {
    std::uint64_t source_load_id = 0;
    std::uint64_t activation_frame = 0;
    unsigned int presentation_viewport_id = 0;
    std::size_t target_index = 0;
    SourceLoadLatencyRequestKind request_kind =
        SourceLoadLatencyRequestKind::ExplicitOpen;
    SourceLoadLatencyOutcome outcome = SourceLoadLatencyOutcome::Pending;
    std::vector<NavigationLatencyAttemptReport> attempts;

    std::int64_t accepted_ns = 0;
    std::int64_t snapshot_activated_ns = 0;
    std::int64_t ui_updated_ns = 0;
    std::int64_t first_present_ns = 0;
    std::int64_t terminal_ns = 0;
};

class SourceLoadLatencyTrace {
public:
    SourceLoadLatencyTrace(
        std::uint64_t source_load_id,
        std::size_t target_index,
        SourceLoadLatencyRequestKind request_kind,
        NavigationLatencyTimePoint accepted_at);

    void SetTargetIndex(std::size_t target_index) noexcept;
    [[nodiscard]] NavigationLatencyAttemptHandle BeginLoadAttempt(
        std::size_t target_index,
        NavigationLatencyTimePoint at = NavigationLatencyTrace::Now());
    void MarkSnapshotActivated(
        std::uint64_t frame_index,
        NavigationLatencyTimePoint at = NavigationLatencyTrace::Now()) noexcept;
    void MarkUiUpdated(
        NavigationLatencyTimePoint at = NavigationLatencyTrace::Now()) noexcept;

    [[nodiscard]] bool MarkPresentedForViewport(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        NavigationLatencyTimePoint at = NavigationLatencyTrace::Now()) noexcept;
    [[nodiscard]] bool MarkTerminal(
        SourceLoadLatencyOutcome outcome,
        NavigationLatencyTimePoint at = NavigationLatencyTrace::Now()) noexcept;
    [[nodiscard]] std::optional<SourceLoadLatencyReport> TerminalReport() const noexcept;

private:
    [[nodiscard]] static std::int64_t ToNanoseconds(
        NavigationLatencyTimePoint at) noexcept;

    std::uint64_t source_load_id_ = 0;
    SourceLoadLatencyRequestKind request_kind_ =
        SourceLoadLatencyRequestKind::ExplicitOpen;
    std::int64_t accepted_ns_ = 0;

    std::atomic_size_t target_index_ = 0;
    std::atomic_int64_t snapshot_activated_ns_ = 0;
    std::atomic_int64_t ui_updated_ns_ = 0;

    mutable std::mutex attempts_mutex_;
    std::vector<NavigationLatencyAttemptHandle> attempts_;

    mutable std::mutex terminal_mutex_;
    SourceLoadLatencyOutcome outcome_ = SourceLoadLatencyOutcome::Pending;
    std::uint64_t activation_frame_ = 0;
    unsigned int presentation_viewport_id_ = 0;
    std::int64_t first_present_ns_ = 0;
    std::int64_t terminal_ns_ = 0;
};

using SourceLoadLatencyTraceHandle = std::shared_ptr<SourceLoadLatencyTrace>;

[[nodiscard]] const char* SourceLoadLatencyRequestKindName(
    SourceLoadLatencyRequestKind kind) noexcept;
[[nodiscard]] const char* SourceLoadLatencyOutcomeName(
    SourceLoadLatencyOutcome outcome) noexcept;
bool WriteSourceLoadLatencyProfileEvent(
    ProfileSink& sink,
    const SourceLoadLatencyReport& report);

}  // namespace specforge
