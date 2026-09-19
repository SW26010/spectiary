#pragma once

#include "profile/load_latency_trace_lifecycle.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace spectiary {

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
    std::vector<LoadLatencyAttemptReport> attempts;

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
        LoadLatencyTimePoint accepted_at);

    void SetTargetIndex(std::size_t target_index) noexcept;
    [[nodiscard]] LoadLatencyAttemptHandle BeginLoadAttempt(
        std::size_t target_index,
        LoadLatencyTimePoint at = LoadLatencyClock::now());
    void MarkSnapshotActivated(
        std::uint64_t frame_index,
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    void MarkUiUpdated(
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;

    [[nodiscard]] bool MarkPresentedForViewport(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    [[nodiscard]] bool MarkTerminal(
        SourceLoadLatencyOutcome outcome,
        LoadLatencyTimePoint at = LoadLatencyClock::now()) noexcept;
    [[nodiscard]] std::optional<SourceLoadLatencyReport> TerminalReport() const noexcept;

private:
    std::uint64_t source_load_id_ = 0;
    SourceLoadLatencyRequestKind request_kind_ =
        SourceLoadLatencyRequestKind::ExplicitOpen;
    std::int64_t accepted_ns_ = 0;

    LoadLatencyAttemptLifecycle load_attempts_;
    profile_internal::LoadLatencyTerminalPresentationLifecycle<
        SourceLoadLatencyOutcome>
        terminal_presentation_;
};

using SourceLoadLatencyTraceHandle = std::shared_ptr<SourceLoadLatencyTrace>;

[[nodiscard]] const char* SourceLoadLatencyRequestKindName(
    SourceLoadLatencyRequestKind kind) noexcept;
[[nodiscard]] const char* SourceLoadLatencyOutcomeName(
    SourceLoadLatencyOutcome outcome) noexcept;
bool WriteSourceLoadLatencyProfileEvent(
    ProfileSink& sink,
    const SourceLoadLatencyReport& report);

}  // namespace spectiary
