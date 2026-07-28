#pragma once

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

class ShellUi;
struct ShellRuntimeResourceObservation;
struct D3D11LiveObjectReport;
struct RuntimeResourceWorkloadOperations;
struct RuntimeResourceWorkloadTestAccess;

struct RuntimeResourceWorkloadConfiguration {
    std::filesystem::path status_path;
    std::vector<std::filesystem::path> source_paths;
    std::size_t warmup_cycles = 2;
    std::size_t measured_cycles = 12;
    std::size_t burst_count = 3;
    std::chrono::milliseconds settle_duration{750};
    std::chrono::milliseconds final_settle_duration{2000};
    std::chrono::milliseconds poll_interval{25};
    std::chrono::milliseconds phase_timeout{120000};
    bool enable_graphics_debug = true;
    bool require_graphics_diagnostics = false;
};

struct RuntimeResourceWorkloadConfigurationLoadResult {
    std::optional<RuntimeResourceWorkloadConfiguration>
        configuration;
    std::string error_message;
};

[[nodiscard]]
RuntimeResourceWorkloadConfigurationLoadResult
LoadRuntimeResourceWorkloadConfiguration(
    const std::filesystem::path& path);

[[nodiscard]]
RuntimeResourceWorkloadConfigurationLoadResult
LoadRuntimeResourceWorkloadConfigurationFromEnvironment();

class RuntimeResourceWorkload {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit RuntimeResourceWorkload(
        RuntimeResourceWorkloadConfiguration configuration);

    RuntimeResourceWorkload(
        const RuntimeResourceWorkload&) = delete;
    RuntimeResourceWorkload& operator=(
        const RuntimeResourceWorkload&) = delete;

    void Start(
        std::uint32_t process_id,
        const ShellUi& ui,
        TimePoint now = Clock::now());
    void Service(
        ShellUi& ui,
        HWND window,
        TimePoint now = Clock::now());
    void RecordGraphicsDiagnostics(
        const D3D11LiveObjectReport& report,
        const ShellUi& ui);
    void RecordRenderTargetResize() noexcept;
    void RecordLocalStateFlushFailure(std::string message);
    void Fail(std::string message);

    [[nodiscard]] std::optional<TimePoint>
    next_deadline() const noexcept;
    [[nodiscard]] int exit_code() const noexcept;
    [[nodiscard]] bool graphics_debug_requested() const noexcept;

private:
    enum class Phase {
        NotStarted,
        StartPreconditioningSource,
        WaitPreconditioningSourceIdle,
        StartStressBurst,
        WaitStressCancellationCheckpoint,
        WaitStressIdle,
        WaitStressPresented,
        StartBaseline,
        WaitBaselineIdle,
        WaitBaselinePresented,
        Settling,
        Completed,
        Failed,
    };

    struct PresentationEvidence {
        std::size_t source_index = 0;
        std::string source_id;
        std::filesystem::path source_path;
        std::size_t spectrum_index = 0;
        std::uint64_t presentation_sequence = 0;
        std::uint64_t source_load_id = 0;
        std::uint64_t activation_frame = 0;
        unsigned int viewport_id = 0;
        std::uint64_t render_target_resize_sequence = 0;
    };

    struct MeasuredCyclePresentationEvidence {
        std::size_t measured_cycle = 0;
        PresentationEvidence stress;
        PresentationEvidence baseline;
    };

    [[nodiscard]] bool WriteStatus(
        const ShellRuntimeResourceObservation&
            observation);
    [[nodiscard]] bool CheckPhaseTimeout(
        TimePoint now);
    void StartWithOperations(
        std::uint32_t process_id,
        const RuntimeResourceWorkloadOperations&
            operations,
        TimePoint now);
    void ServiceWithOperations(
        const RuntimeResourceWorkloadOperations&
            operations,
        TimePoint now);
    [[nodiscard]] std::size_t TotalCycleCount() const noexcept;
    [[nodiscard]] std::size_t CompletedMeasuredCycles() const noexcept;
    [[nodiscard]] std::size_t
    CurrentPreconditioningSourceIndex() const noexcept;
    [[nodiscard]] std::size_t CurrentStressSourceIndex() const noexcept;
    [[nodiscard]] const char* StateName() const noexcept;
    [[nodiscard]] const char* PhaseName() const noexcept;
    void SetPhase(
        Phase phase,
        TimePoint now,
        std::chrono::milliseconds delay =
            std::chrono::milliseconds{0});
    void RecordMeasurementBaseline(
        const ShellRuntimeResourceObservation& observation);
    [[nodiscard]] std::optional<PresentationEvidence>
    TakePresentationEvidence(
        const ShellRuntimeResourceObservation& observation,
        std::size_t source_index,
        std::uint64_t presentation_sequence_floor,
        std::uint64_t resize_sequence_floor) const;
    void CompleteCycle(
        const ShellRuntimeResourceObservation& observation,
        TimePoint now);
    void RequestClose(
        const RuntimeResourceWorkloadOperations&
            operations);

    RuntimeResourceWorkloadConfiguration configuration_;
    Phase phase_ = Phase::NotStarted;
    TimePoint started_at_;
    TimePoint phase_started_at_;
    std::optional<TimePoint> next_deadline_;
    std::uint32_t process_id_ = 0;
    std::size_t preconditioning_position_ = 0;
    std::size_t preconditioned_source_count_ = 0;
    std::size_t completed_cycles_ = 0;
    std::uint64_t measured_start_successful_cancellations_ = 0;
    std::uint64_t measured_start_retired_objects_ = 0;
    std::uint64_t last_measured_cycle_successful_cancellations_ = 0;
    std::uint64_t last_measured_cycle_retired_objects_ = 0;
    std::size_t measured_cycles_with_cancellation_ = 0;
    std::size_t measured_cycles_with_retirement_ = 0;
    std::uint64_t stress_presentation_sequence_floor_ = 0;
    std::uint64_t baseline_presentation_sequence_floor_ = 0;
    std::uint64_t stress_resize_sequence_floor_ = 0;
    std::uint64_t baseline_resize_sequence_floor_ = 0;
    std::uint64_t render_target_resize_count_ = 0;
    std::optional<PresentationEvidence>
        pending_stress_presentation_;
    std::optional<PresentationEvidence>
        pending_baseline_presentation_;
    std::vector<MeasuredCyclePresentationEvidence>
        measured_cycle_presentations_;
    std::string failure_message_;
    int exit_code_ = 0;
    bool measurement_baseline_recorded_ = false;
    bool close_requested_ = false;
    bool graphics_report_recorded_ = false;
    bool graphics_available_ = false;
    std::uint64_t graphics_unexpected_live_objects_ = 0;
    std::uint64_t graphics_allowed_live_objects_ = 0;
    std::string graphics_detail_;
    std::vector<std::string> graphics_unexpected_messages_;

    friend struct RuntimeResourceWorkloadTestAccess;
};

}  // namespace specforge
