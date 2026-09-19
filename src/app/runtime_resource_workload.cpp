#include "app/runtime_resource_workload.h"

#include "app/local_user_state_json.h"
#include "app/runtime_resource_workload_internal.h"
#include "renderer/d3d11_renderer.h"
#include "ui/shell_ui.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace spectiary {
namespace {

constexpr std::string_view kConfigurationFormatKind =
    "spectiary_runtime_resource_workload";
constexpr std::string_view kStatusFormatKind =
    "spectiary_runtime_resource_workload_status";
constexpr int kSchemaVersion = 1;

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

std::filesystem::path Utf8Path(std::string_view value)
{
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const char byte : value) {
        utf8.push_back(
            static_cast<char8_t>(
                static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path{utf8};
}

std::optional<std::filesystem::path>
EnvironmentPath(const wchar_t* name)
{
    wchar_t* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_wdupenv_s(
            &raw_value,
            &value_size,
            name) != 0 ||
        raw_value == nullptr) {
        std::free(raw_value);
        return std::nullopt;
    }

    std::filesystem::path result;
    if (raw_value[0] != L'\0') {
        result = raw_value;
    }
    std::free(raw_value);
    return result.empty()
        ? std::nullopt
        : std::optional<std::filesystem::path>{
              std::move(result)};
}

std::optional<std::size_t> RequiredSizeMember(
    const nlohmann::json& root,
    std::string_view name,
    std::string& error)
{
    const std::optional<std::size_t> value =
        ReadJsonSizeMember(root, name);
    if (!value) {
        error =
            "Runtime resource workload member '" +
            std::string(name) +
            "' must be a non-negative integer.";
    }
    return value;
}

bool ValidateRange(
    std::size_t value,
    std::size_t minimum,
    std::size_t maximum,
    std::string_view name,
    std::string& error)
{
    if (value >= minimum && value <= maximum) {
        return true;
    }
    error =
        "Runtime resource workload member '" +
        std::string(name) + "' must be between " +
        std::to_string(minimum) + " and " +
        std::to_string(maximum) + ".";
    return false;
}

bool SamePath(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    return _wcsicmp(
               left.lexically_normal().c_str(),
               right.lexically_normal().c_str()) == 0;
}

nlohmann::json ActivityJson(
    const ShellRuntimeResourceObservation& observation,
    bool measurement_baseline_recorded,
    std::uint64_t measured_start_cancellations,
    std::uint64_t measured_start_retired_objects)
{
    const SourceCollectionLoadActivitySnapshot& activity =
        observation.load_activity;
    const std::uint64_t retired_objects =
        activity.retired_prepared_count +
        activity.retired_resource_count;
    return nlohmann::json::object({
        {"active_task_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 activity.active_task_count))},
        {"completed_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 activity.completed_count))},
        {"worker_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 activity.worker_count))},
        {"pending_load_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 observation.pending_load_count))},
        {"retirement_queued_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 activity.retirement_queued_count))},
        {"retirement_in_flight_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 activity.retirement_in_flight_count))},
        {"cancellation_request_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 activity.cancellation_request_count))},
        {"successful_cancellation_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 activity.successful_cancellation_count))},
        {"runtime_resource_cancellation_checkpoint_waiting",
         nlohmann::json(
             activity
                 .runtime_resource_cancellation_checkpoint_waiting)},
        {"successful_cancellation_delta",
         nlohmann::json(
             static_cast<std::int64_t>(
                 measurement_baseline_recorded
                 ? activity.successful_cancellation_count -
                       std::min(
                           activity.successful_cancellation_count,
                           measured_start_cancellations)
                 : 0))},
        {"retired_prepared_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 activity.retired_prepared_count))},
        {"retired_resource_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 activity.retired_resource_count))},
        {"retired_object_delta",
         nlohmann::json(
             static_cast<std::int64_t>(
                 measurement_baseline_recorded
                 ? retired_objects -
                       std::min(
                           retired_objects,
                           measured_start_retired_objects)
                 : 0))},
        {"idle", nlohmann::json(observation.idle())},
    });
}

}  // namespace

RuntimeResourceWorkloadConfigurationLoadResult
LoadRuntimeResourceWorkloadConfiguration(
    const std::filesystem::path& path)
{
    RuntimeResourceWorkloadConfigurationLoadResult result;
    const VersionedJsonCacheLoadResult loaded =
        LoadVersionedJsonCacheFile(
            path,
            kConfigurationFormatKind,
            {kSchemaVersion},
            "runtime resource workload");
    if (!loaded.document) {
        result.error_message =
            loaded.warning.empty()
            ? "Could not load the runtime resource workload configuration."
            : loaded.warning;
        return result;
    }

    const nlohmann::json& root = loaded.document->root;
    const std::optional<std::string> status_path =
        ReadJsonStringMember(root, "status_path");
    const nlohmann::json* sources =
        JsonObjectMember(root, "source_paths");
    if (!status_path || status_path->empty()) {
        result.error_message =
            "Runtime resource workload member 'status_path' must be a non-empty string.";
        return result;
    }
    if (sources == nullptr ||
        sources->type() != nlohmann::json::value_t::array ||
        (*sources).size() < 2) {
        result.error_message =
            "Runtime resource workload member 'source_paths' must contain at least two paths.";
        return result;
    }

    RuntimeResourceWorkloadConfiguration configuration;
    configuration.status_path =
        Utf8Path(*status_path);
    for (const nlohmann::json& source : (*sources)) {
        if (source.type() != nlohmann::json::value_t::string ||
            source.get_ref<const std::string&>().empty()) {
            result.error_message =
                "Every runtime resource workload source path must be a non-empty string.";
            return result;
        }
        std::filesystem::path source_path =
            Utf8Path(source.get_ref<const std::string&>());
        std::error_code canonical_error;
        source_path = std::filesystem::weakly_canonical(
            source_path,
            canonical_error);
        if (canonical_error ||
            !std::filesystem::exists(source_path)) {
            result.error_message =
                "Runtime resource workload source does not exist: " +
                source.get_ref<const std::string&>();
            return result;
        }
        configuration.source_paths.push_back(
            std::move(source_path));
    }

    for (std::size_t index = 0;
         index < configuration.source_paths.size();
         ++index) {
        for (std::size_t other = index + 1;
             other < configuration.source_paths.size();
             ++other) {
            if (SamePath(
                    configuration.source_paths[index],
                    configuration.source_paths[other])) {
                result.error_message =
                    "Runtime resource workload source paths must be distinct.";
                return result;
            }
        }
    }

    std::string member_error;
    const std::optional<std::size_t> warmup_cycles =
        RequiredSizeMember(
            root,
            "warmup_cycles",
            member_error);
    const std::optional<std::size_t> measured_cycles =
        RequiredSizeMember(
            root,
            "measured_cycles",
            member_error);
    const std::optional<std::size_t> burst_count =
        RequiredSizeMember(
            root,
            "burst_count",
            member_error);
    const std::optional<std::size_t> settle_ms =
        RequiredSizeMember(
            root,
            "settle_ms",
            member_error);
    const std::optional<std::size_t> final_settle_ms =
        RequiredSizeMember(
            root,
            "final_settle_ms",
            member_error);
    const std::optional<std::size_t> poll_interval_ms =
        RequiredSizeMember(
            root,
            "poll_interval_ms",
            member_error);
    const std::optional<std::size_t> phase_timeout_ms =
        RequiredSizeMember(
            root,
            "phase_timeout_ms",
            member_error);
    if (!member_error.empty()) {
        result.error_message = std::move(member_error);
        return result;
    }

    if (!ValidateRange(*warmup_cycles, 1, 100, "warmup_cycles", member_error) ||
        !ValidateRange(*measured_cycles, 1, 10000, "measured_cycles", member_error) ||
        !ValidateRange(*burst_count, 2, 32, "burst_count", member_error) ||
        !ValidateRange(*settle_ms, 50, 60000, "settle_ms", member_error) ||
        !ValidateRange(*final_settle_ms, 50, 120000, "final_settle_ms", member_error) ||
        !ValidateRange(*poll_interval_ms, 10, 1000, "poll_interval_ms", member_error) ||
        !ValidateRange(*phase_timeout_ms, 1000, 600000, "phase_timeout_ms", member_error)) {
        result.error_message = std::move(member_error);
        return result;
    }

    configuration.warmup_cycles = *warmup_cycles;
    configuration.measured_cycles = *measured_cycles;
    configuration.burst_count = *burst_count;
    configuration.settle_duration =
        std::chrono::milliseconds(*settle_ms);
    configuration.final_settle_duration =
        std::chrono::milliseconds(*final_settle_ms);
    configuration.poll_interval =
        std::chrono::milliseconds(*poll_interval_ms);
    configuration.phase_timeout =
        std::chrono::milliseconds(*phase_timeout_ms);
    configuration.enable_graphics_debug =
        ReadJsonBoolMember(
            root,
            "enable_graphics_debug",
            true);
    configuration.require_graphics_diagnostics =
        ReadJsonBoolMember(
            root,
            "require_graphics_diagnostics",
            false);

    std::error_code absolute_error;
    if (!configuration.status_path.is_absolute()) {
        configuration.status_path =
            std::filesystem::absolute(
                configuration.status_path,
                absolute_error);
    }
    if (absolute_error) {
        result.error_message =
            "Could not resolve the runtime resource workload status path.";
        return result;
    }

    result.configuration = std::move(configuration);
    return result;
}

RuntimeResourceWorkloadConfigurationLoadResult
LoadRuntimeResourceWorkloadConfigurationFromEnvironment()
{
    const std::optional<std::filesystem::path> config_path =
        EnvironmentPath(
            L"SPECTIARY_RUNTIME_RESOURCE_WORKLOAD");
    if (!config_path) {
        return {};
    }
    if (!EnvironmentPath(
            L"SPECTIARY_RUNTIME_RESOURCE_STATE_DIR")) {
        return {
            .configuration = std::nullopt,
            .error_message =
                "SPECTIARY_RUNTIME_RESOURCE_STATE_DIR is required when the runtime resource workload is enabled.",
        };
    }
    return LoadRuntimeResourceWorkloadConfiguration(
        *config_path);
}

RuntimeResourceWorkload::RuntimeResourceWorkload(
    RuntimeResourceWorkloadConfiguration configuration)
    : configuration_(std::move(configuration))
{
}

void RuntimeResourceWorkload::Start(
    std::uint32_t process_id,
    const ShellUi& ui,
    TimePoint now)
{
    RuntimeResourceWorkloadOperations operations;
    operations.observe = [&ui]() {
        return ui.runtime_resource_observation();
    };
    operations.write_status =
        [this](
            const ShellRuntimeResourceObservation&
                observation) {
            return WriteStatus(observation);
        };
    StartWithOperations(
        process_id,
        operations,
        now);
}

void RuntimeResourceWorkload::StartWithOperations(
    std::uint32_t process_id,
    const RuntimeResourceWorkloadOperations& operations,
    TimePoint now)
{
    process_id_ = process_id;
    started_at_ = now;
    SetPhase(Phase::StartPreconditioningSource, now);
    if (!operations.observe ||
        !operations.write_status ||
        !operations.write_status(
            operations.observe())) {
        Fail(
            "Could not write the runtime resource workload status file.");
    }
}

void RuntimeResourceWorkload::Service(
    ShellUi& ui,
    HWND window,
    TimePoint now)
{
    RuntimeResourceWorkloadOperations operations;
    operations.observe = [&ui]() {
        return ui.runtime_resource_observation();
    };
    operations.write_status =
        [this](
            const ShellRuntimeResourceObservation&
                observation) {
            return WriteStatus(observation);
        };
    operations.request_resize =
        [window](int width, int height) {
            (void)SetWindowPos(
                window,
                nullptr,
                0,
                0,
                width,
                height,
                SWP_NOMOVE | SWP_NOZORDER |
                    SWP_NOACTIVATE);
        };
    operations.arm_cancellation_checkpoint =
        [&ui]() {
            return ui.
                ArmRuntimeResourceCancellationCheckpoint();
        };
    operations.open_source =
        [&ui](const std::filesystem::path& source) {
            ui.OpenSource(source);
        };
    operations.request_close =
        [window]() {
            return window == nullptr ||
                PostMessageW(
                    window,
                    WM_CLOSE,
                    0,
                    0) != FALSE;
        };
    ServiceWithOperations(
        operations,
        now);
}

void RuntimeResourceWorkload::ServiceWithOperations(
    const RuntimeResourceWorkloadOperations& operations,
    TimePoint now)
{
    if (phase_ == Phase::NotStarted) {
        return;
    }
    if (!operations.observe ||
        !operations.write_status ||
        !operations.request_resize ||
        !operations.arm_cancellation_checkpoint ||
        !operations.open_source ||
        !operations.request_close) {
        Fail(
            "Runtime resource workload operations are incomplete.");
        return;
    }
    const ShellRuntimeResourceObservation observation =
        operations.observe();
    if (phase_ == Phase::Failed) {
        (void)operations.write_status(observation);
        RequestClose(operations);
        return;
    }
    if (phase_ == Phase::Completed) {
        RequestClose(operations);
        return;
    }
    if (next_deadline_ && now < *next_deadline_) {
        return;
    }
    if (!CheckPhaseTimeout(now)) {
        (void)operations.write_status(observation);
        RequestClose(operations);
        return;
    }

    if (!observation.load_error.empty()) {
        Fail(
            "Source workload failed: " +
            observation.load_error);
        (void)operations.write_status(observation);
        RequestClose(operations);
        return;
    }

    switch (phase_) {
    case Phase::StartPreconditioningSource: {
        const std::size_t source_index =
            CurrentPreconditioningSourceIndex();
        if (source_index == 0) {
            operations.request_resize(
                1280,
                820);
        }
        operations.open_source(
            configuration_.source_paths[source_index]);
        SetPhase(
            Phase::WaitPreconditioningSourceIdle,
            now,
            configuration_.poll_interval);
        break;
    }
    case Phase::WaitPreconditioningSourceIdle:
        if (!observation.idle()) {
            next_deadline_ =
                now + configuration_.poll_interval;
            break;
        }
        if (!SamePath(
                observation.active_source_path,
                configuration_.source_paths[
                    CurrentPreconditioningSourceIndex()])) {
            Fail(
                "A preconditioning source did not become the active source.");
            break;
        }
        ++preconditioning_position_;
        ++preconditioned_source_count_;
        SetPhase(
            preconditioning_position_ <
                    configuration_.source_paths.size()
                ? Phase::StartPreconditioningSource
                : Phase::StartStressBurst,
            now);
        break;
    case Phase::StartStressBurst: {
        const std::size_t source_index =
            CurrentStressSourceIndex();
        const std::filesystem::path& source =
            configuration_.source_paths[source_index];
        stress_presentation_sequence_floor_ =
            observation.presented_source_load.sequence;
        stress_resize_sequence_floor_ =
            render_target_resize_count_;
        pending_stress_presentation_.reset();
        pending_baseline_presentation_.reset();
        const int stress_width =
            completed_cycles_ % 2 == 0 ? 1024 : 1184;
        const int stress_height =
            completed_cycles_ % 2 == 0 ? 720 : 760;
        operations.request_resize(
            stress_width,
            stress_height);
        if (!operations.arm_cancellation_checkpoint()) {
            Fail(
                "Could not arm the runtime resource cancellation checkpoint while the load queue was idle.");
            break;
        }
        operations.open_source(source);
        SetPhase(
            Phase::WaitStressCancellationCheckpoint,
            now,
            configuration_.poll_interval);
        break;
    }
    case Phase::WaitStressCancellationCheckpoint: {
        if (!observation.load_activity
                 .runtime_resource_cancellation_checkpoint_waiting) {
            next_deadline_ =
                now + configuration_.poll_interval;
            break;
        }
        const std::filesystem::path& source =
            configuration_.source_paths[
                CurrentStressSourceIndex()];
        for (std::size_t burst_index = 1;
             burst_index < configuration_.burst_count;
             ++burst_index) {
            operations.open_source(source);
        }
        SetPhase(
            Phase::WaitStressIdle,
            now,
            configuration_.poll_interval);
        break;
    }
    case Phase::WaitStressIdle:
        if (!observation.idle()) {
            next_deadline_ =
                now + configuration_.poll_interval;
            break;
        }
        if (!SamePath(
                observation.active_source_path,
                configuration_.source_paths[
                    CurrentStressSourceIndex()])) {
            Fail(
                "The stress source did not become the active source.");
            break;
        }
        SetPhase(
            Phase::WaitStressPresented,
            now,
            configuration_.poll_interval);
        break;
    case Phase::WaitStressPresented:
        if (!observation.idle()) {
            next_deadline_ =
                now + configuration_.poll_interval;
            break;
        }
        if (!SamePath(
                observation.active_source_path,
                configuration_.source_paths[
                    CurrentStressSourceIndex()])) {
            Fail(
                "The active source changed while waiting for the stress snapshot to be presented.");
            break;
        }
        pending_stress_presentation_ =
            TakePresentationEvidence(
                observation,
                CurrentStressSourceIndex(),
                stress_presentation_sequence_floor_,
                stress_resize_sequence_floor_);
        if (!pending_stress_presentation_) {
            next_deadline_ =
                now + configuration_.poll_interval;
            break;
        }
        SetPhase(Phase::StartBaseline, now);
        break;
    case Phase::StartBaseline:
        baseline_presentation_sequence_floor_ =
            observation.presented_source_load.sequence;
        baseline_resize_sequence_floor_ =
            render_target_resize_count_;
        operations.request_resize(
            1280,
            820);
        operations.open_source(
            configuration_.source_paths.front());
        SetPhase(
            Phase::WaitBaselineIdle,
            now,
            configuration_.poll_interval);
        break;
    case Phase::WaitBaselineIdle:
        if (!observation.idle()) {
            next_deadline_ =
                now + configuration_.poll_interval;
            break;
        }
        if (!SamePath(
                observation.active_source_path,
                configuration_.source_paths.front())) {
            Fail(
                "The baseline source did not become the active source.");
            break;
        }
        SetPhase(
            Phase::WaitBaselinePresented,
            now,
            configuration_.poll_interval);
        break;
    case Phase::WaitBaselinePresented:
        if (!observation.idle()) {
            next_deadline_ =
                now + configuration_.poll_interval;
            break;
        }
        if (!SamePath(
                observation.active_source_path,
                configuration_.source_paths.front())) {
            Fail(
                "The active source changed while waiting for the baseline snapshot to be presented.");
            break;
        }
        pending_baseline_presentation_ =
            TakePresentationEvidence(
                observation,
                0,
                baseline_presentation_sequence_floor_,
                baseline_resize_sequence_floor_);
        if (!pending_baseline_presentation_) {
            next_deadline_ =
                now + configuration_.poll_interval;
            break;
        }
        CompleteCycle(observation, now);
        break;
    case Phase::Settling:
        if (completed_cycles_ >= TotalCycleCount()) {
            const std::uint64_t successful_cancellations =
                observation.load_activity
                    .successful_cancellation_count;
            const std::uint64_t retired_objects =
                observation.load_activity
                    .retired_prepared_count +
                observation.load_activity
                    .retired_resource_count;
            if (!observation.idle()) {
                Fail(
                    "The final source workload did not become idle.");
            } else if (
                measured_cycles_with_cancellation_ !=
                configuration_.measured_cycles ||
                successful_cancellations <=
                    measured_start_successful_cancellations_) {
                Fail(
                    "Not every measured cycle observed a successful source-load cancellation.");
            } else if (
                measured_cycles_with_retirement_ !=
                configuration_.measured_cycles ||
                retired_objects <=
                    measured_start_retired_objects_) {
                Fail(
                    "Not every measured cycle observed background resource retirement.");
            } else if (
                measured_cycle_presentations_.size() !=
                configuration_.measured_cycles) {
                Fail(
                    "Not every measured cycle presented its exact stress and baseline snapshots.");
            } else {
                SetPhase(Phase::Completed, now);
            }
        } else {
            if (completed_cycles_ ==
                    configuration_.warmup_cycles &&
                !measurement_baseline_recorded_) {
                RecordMeasurementBaseline(observation);
            }
            SetPhase(Phase::StartStressBurst, now);
        }
        break;
    case Phase::NotStarted:
    case Phase::Completed:
    case Phase::Failed:
        break;
    }

    if (!operations.write_status(
            operations.observe())) {
        Fail(
            "Could not update the runtime resource workload status file.");
    }
    if (phase_ == Phase::Failed ||
        phase_ == Phase::Completed) {
        RequestClose(operations);
    }
}

void RuntimeResourceWorkload::RecordGraphicsDiagnostics(
    const D3D11LiveObjectReport& report,
    const ShellUi& ui)
{
    graphics_report_recorded_ = report.requested;
    graphics_available_ = report.available;
    graphics_allowed_live_objects_ =
        report.allowed_live_object_messages;
    graphics_unexpected_live_objects_ =
        report.unexpected_live_object_messages;
    graphics_detail_ = report.detail;
    graphics_unexpected_messages_ =
        report.unexpected_messages;

    if (report.requested && report.available &&
        report.unexpected_live_object_messages > 0) {
        Fail(
            "D3D11 debug-layer diagnostics reported unexpected live objects.");
    } else if (
        configuration_.require_graphics_diagnostics &&
        (!report.requested || !report.available)) {
        Fail(
            "Required D3D11 live-object diagnostics were unavailable.");
    }
    (void)WriteStatus(
        ui.runtime_resource_observation());
}

void RuntimeResourceWorkload::
    RecordRenderTargetResize() noexcept
{
    ++render_target_resize_count_;
}

void RuntimeResourceWorkload::RecordLocalStateFlushFailure(
    std::string message)
{
    const std::string diagnostic =
        "Spectiary local state flush failure: " +
        message + "\n";
    OutputDebugStringA(diagnostic.c_str());

    if (!failure_message_.empty()) {
        message =
            failure_message_ +
            " Shutdown local state flush also failed: " +
            message;
    } else {
        message =
            "Shutdown local state flush failed: " +
            message;
    }
    Fail(std::move(message));
}

void RuntimeResourceWorkload::Fail(std::string message)
{
    failure_message_ = std::move(message);
    phase_ = Phase::Failed;
    next_deadline_ = Clock::now();
    exit_code_ = 2;
}

std::optional<RuntimeResourceWorkload::TimePoint>
RuntimeResourceWorkload::next_deadline() const noexcept
{
    return next_deadline_;
}

int RuntimeResourceWorkload::exit_code() const noexcept
{
    return exit_code_;
}

bool RuntimeResourceWorkload::graphics_debug_requested() const noexcept
{
    return configuration_.enable_graphics_debug;
}

bool RuntimeResourceWorkload::WriteStatus(
    const ShellRuntimeResourceObservation& observation)
{
    const std::int64_t elapsed_ms =
        started_at_ == TimePoint{}
        ? 0
        : std::chrono::duration_cast<
              std::chrono::milliseconds>(
              Clock::now() - started_at_)
              .count();

    nlohmann::json unexpected_graphics_messages =
        nlohmann::json::array({});
    unexpected_graphics_messages.get_ref<nlohmann::json::array_t&>().reserve(
        graphics_unexpected_messages_.size());
    for (const std::string& message :
         graphics_unexpected_messages_) {
        unexpected_graphics_messages.push_back(
            nlohmann::json(message));
    }

    nlohmann::json graphics = nlohmann::json::object({
        {"requested",
         nlohmann::json(
             configuration_.enable_graphics_debug)},
        {"report_recorded",
         nlohmann::json(graphics_report_recorded_)},
        {"available",
         nlohmann::json(graphics_available_)},
        {"allowed_live_object_messages",
         nlohmann::json(
             static_cast<std::int64_t>(
                 graphics_allowed_live_objects_))},
        {"unexpected_live_object_messages",
         nlohmann::json(
             static_cast<std::int64_t>(
                 graphics_unexpected_live_objects_))},
        {"unexpected_messages",
         std::move(unexpected_graphics_messages)},
        {"detail", nlohmann::json(graphics_detail_)},
    });

    const auto presentation_evidence_json =
        [](const PresentationEvidence& evidence) {
            return nlohmann::json::object({
                {"source_index",
                 nlohmann::json(
                     static_cast<std::int64_t>(
                         evidence.source_index))},
                {"source_id",
                 nlohmann::json(evidence.source_id)},
                {"source_path",
                 nlohmann::json(
                     PathToUtf8(
                         evidence.source_path))},
                {"spectrum_index",
                 nlohmann::json(
                     static_cast<std::int64_t>(
                         evidence.spectrum_index))},
                {"presentation_sequence",
                 nlohmann::json(
                     static_cast<std::int64_t>(
                         evidence.presentation_sequence))},
                {"source_load_id",
                 nlohmann::json(
                     static_cast<std::int64_t>(
                         evidence.source_load_id))},
                {"activation_frame",
                 nlohmann::json(
                     static_cast<std::int64_t>(
                         evidence.activation_frame))},
                {"viewport_id",
                 nlohmann::json(
                     static_cast<std::int64_t>(
                         evidence.viewport_id))},
                {"render_target_resize_sequence",
                 nlohmann::json(
                     static_cast<std::int64_t>(
                         evidence
                             .render_target_resize_sequence))},
            });
        };
    nlohmann::json measured_cycle_presentations =
        nlohmann::json::array({});
    measured_cycle_presentations.get_ref<nlohmann::json::array_t&>().reserve(
        measured_cycle_presentations_.size());
    for (const MeasuredCyclePresentationEvidence& cycle :
         measured_cycle_presentations_) {
        measured_cycle_presentations.push_back(
            nlohmann::json::object({
                {"measured_cycle",
                 nlohmann::json(
                     static_cast<std::int64_t>(
                         cycle.measured_cycle))},
                {"stress",
                 presentation_evidence_json(
                     cycle.stress)},
                {"baseline",
                 presentation_evidence_json(
                     cycle.baseline)},
            }));
    }

    const SourceCollectionActivationTransaction::
        PresentedSourceLoadObservation& last_presentation =
            observation.presented_source_load;
    nlohmann::json presentation = nlohmann::json::object({
        {"successful_source_load_present_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 last_presentation.sequence))},
        {"last_source_load_id",
         nlohmann::json(
             static_cast<std::int64_t>(
                 last_presentation.source_load_id))},
        {"last_activation_frame",
         nlohmann::json(
             static_cast<std::int64_t>(
                 last_presentation.activation_frame))},
        {"last_viewport_id",
         nlohmann::json(
             static_cast<std::int64_t>(
                 last_presentation.viewport_id))},
        {"render_target_resize_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 render_target_resize_count_))},
    });

    const nlohmann::json body = nlohmann::json::object({
        {"state", nlohmann::json(StateName())},
        {"phase", nlohmann::json(PhaseName())},
        {"process_id",
         nlohmann::json(
             static_cast<std::int64_t>(process_id_))},
        {"elapsed_ms", nlohmann::json(elapsed_ms)},
        {"warmup_cycles",
         nlohmann::json(
             static_cast<std::int64_t>(
                 configuration_.warmup_cycles))},
        {"measured_cycles",
         nlohmann::json(
             static_cast<std::int64_t>(
                 configuration_.measured_cycles))},
        {"configured_source_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 configuration_.source_paths.size()))},
        {"preconditioned_source_count",
         nlohmann::json(
             static_cast<std::int64_t>(
                 preconditioned_source_count_))},
        {"measurement_baseline_recorded",
         nlohmann::json(
             measurement_baseline_recorded_)},
        {"measured_cycles_with_cancellation",
         nlohmann::json(
             static_cast<std::int64_t>(
                 measured_cycles_with_cancellation_))},
        {"measured_cycles_with_retirement",
         nlohmann::json(
             static_cast<std::int64_t>(
                 measured_cycles_with_retirement_))},
        {"completed_cycles",
         nlohmann::json(
             static_cast<std::int64_t>(
                 completed_cycles_))},
        {"completed_measured_cycles",
         nlohmann::json(
             static_cast<std::int64_t>(
                 CompletedMeasuredCycles()))},
        {"total_cycles",
         nlohmann::json(
             static_cast<std::int64_t>(
                 TotalCycleCount()))},
        {"stress_source_index",
         nlohmann::json(
             static_cast<std::int64_t>(
                 CurrentStressSourceIndex()))},
        {"preconditioning_source_index",
         nlohmann::json(
             static_cast<std::int64_t>(
                 CurrentPreconditioningSourceIndex()))},
        {"active_source_path",
         nlohmann::json(
             PathToUtf8(
                 observation.active_source_path))},
        {"failure", nlohmann::json(failure_message_)},
        {"measured_cycle_presentations",
         std::move(measured_cycle_presentations)},
        {"presentation", std::move(presentation)},
        {"activity",
         ActivityJson(
             observation,
             measurement_baseline_recorded_,
             measured_start_successful_cancellations_,
             measured_start_retired_objects_)},
        {"graphics", std::move(graphics)},
    });

    std::string error;
    AtomicFileReplaceRetryPolicy status_replace_retry_policy;
    status_replace_retry_policy.maximum_attempts = 5;
    status_replace_retry_policy.initial_retry_delay =
        std::chrono::milliseconds(5);
    status_replace_retry_policy.maximum_retry_delay =
        std::chrono::milliseconds(40);
    const bool written =
        WriteVersionedJsonCacheDocument(
            configuration_.status_path,
            kStatusFormatKind,
            kSchemaVersion,
            "runtime resource workload status",
            body,
            &error,
            status_replace_retry_policy);
    if (!written && failure_message_.empty()) {
        failure_message_ = std::move(error);
    }
    return written;
}

bool RuntimeResourceWorkload::CheckPhaseTimeout(
    TimePoint now)
{
    if (now - phase_started_at_ <=
        configuration_.phase_timeout) {
        return true;
    }
    Fail(
        "Runtime resource workload phase timed out: " +
        std::string(PhaseName()) + ".");
    return false;
}

std::size_t RuntimeResourceWorkload::TotalCycleCount() const noexcept
{
    return configuration_.warmup_cycles +
           configuration_.measured_cycles;
}

std::size_t
RuntimeResourceWorkload::CompletedMeasuredCycles() const noexcept
{
    return completed_cycles_ >
               configuration_.warmup_cycles
        ? completed_cycles_ -
              configuration_.warmup_cycles
        : 0;
}

std::size_t
RuntimeResourceWorkload::CurrentPreconditioningSourceIndex()
    const noexcept
{
    if (preconditioning_position_ >=
        configuration_.source_paths.size()) {
        return 0;
    }
    return (
        preconditioning_position_ + 1) %
        configuration_.source_paths.size();
}

std::size_t
RuntimeResourceWorkload::CurrentStressSourceIndex() const noexcept
{
    const std::size_t stress_source_count =
        configuration_.source_paths.size() - 1;
    return 1 +
           (completed_cycles_ % stress_source_count);
}

const char* RuntimeResourceWorkload::StateName() const noexcept
{
    switch (phase_) {
    case Phase::NotStarted:
        return "not_started";
    case Phase::Completed:
        return "completed";
    case Phase::Failed:
        return "failed";
    case Phase::StartPreconditioningSource:
    case Phase::WaitPreconditioningSourceIdle:
    case Phase::StartStressBurst:
    case Phase::WaitStressCancellationCheckpoint:
    case Phase::WaitStressIdle:
    case Phase::WaitStressPresented:
    case Phase::StartBaseline:
    case Phase::WaitBaselineIdle:
    case Phase::WaitBaselinePresented:
    case Phase::Settling:
        return "running";
    }
    return "failed";
}

const char* RuntimeResourceWorkload::PhaseName() const noexcept
{
    switch (phase_) {
    case Phase::NotStarted:
        return "not_started";
    case Phase::StartPreconditioningSource:
        return "start_preconditioning_source";
    case Phase::WaitPreconditioningSourceIdle:
        return "wait_preconditioning_source_idle";
    case Phase::StartStressBurst:
        return "start_stress_burst";
    case Phase::WaitStressCancellationCheckpoint:
        return "wait_stress_cancellation_checkpoint";
    case Phase::WaitStressIdle:
        return "wait_stress_idle";
    case Phase::WaitStressPresented:
        return "wait_stress_presented";
    case Phase::StartBaseline:
        return "start_baseline";
    case Phase::WaitBaselineIdle:
        return "wait_baseline_idle";
    case Phase::WaitBaselinePresented:
        return "wait_baseline_presented";
    case Phase::Settling:
        return "settling";
    case Phase::Completed:
        return "completed";
    case Phase::Failed:
        return "failed";
    }
    return "failed";
}

void RuntimeResourceWorkload::SetPhase(
    Phase phase,
    TimePoint now,
    std::chrono::milliseconds delay)
{
    phase_ = phase;
    phase_started_at_ = now;
    next_deadline_ =
        phase == Phase::Completed ||
                phase == Phase::Failed
        ? std::nullopt
        : std::optional<TimePoint>{now + delay};
}

void RuntimeResourceWorkload::RecordMeasurementBaseline(
    const ShellRuntimeResourceObservation& observation)
{
    measured_start_successful_cancellations_ =
        observation.load_activity
            .successful_cancellation_count;
    measured_start_retired_objects_ =
        observation.load_activity.retired_prepared_count +
        observation.load_activity.retired_resource_count;
    last_measured_cycle_successful_cancellations_ =
        measured_start_successful_cancellations_;
    last_measured_cycle_retired_objects_ =
        measured_start_retired_objects_;
    measurement_baseline_recorded_ = true;
}

std::optional<RuntimeResourceWorkload::PresentationEvidence>
RuntimeResourceWorkload::TakePresentationEvidence(
    const ShellRuntimeResourceObservation& observation,
    std::size_t source_index,
    std::uint64_t presentation_sequence_floor,
    std::uint64_t resize_sequence_floor) const
{
    const SourceCollectionActivationTransaction::
        PresentedSourceLoadObservation& presented =
            observation.presented_source_load;
    if (source_index >=
            configuration_.source_paths.size() ||
        presented.sequence <=
            presentation_sequence_floor ||
        presented.source_load_id == 0 ||
        presented.activation_frame == 0 ||
        presented.viewport_id == 0 ||
        presented.source_id.empty() ||
        presented.spectrum_index != 0 ||
        !SamePath(
            presented.source_path,
            configuration_.source_paths[source_index]) ||
        render_target_resize_count_ <=
            resize_sequence_floor) {
        return std::nullopt;
    }

    return PresentationEvidence{
        .source_index = source_index,
        .source_id = presented.source_id,
        .source_path = presented.source_path,
        .spectrum_index = presented.spectrum_index,
        .presentation_sequence = presented.sequence,
        .source_load_id = presented.source_load_id,
        .activation_frame = presented.activation_frame,
        .viewport_id = presented.viewport_id,
        .render_target_resize_sequence =
            render_target_resize_count_,
    };
}

void RuntimeResourceWorkload::CompleteCycle(
    const ShellRuntimeResourceObservation& observation,
    TimePoint now)
{
    const bool measured_cycle =
        completed_cycles_ >=
        configuration_.warmup_cycles;
    if (measured_cycle) {
        if (!measurement_baseline_recorded_) {
            Fail(
                "The measured workload started before its post-warmup baseline was recorded.");
            return;
        }
        const std::uint64_t successful_cancellations =
            observation.load_activity
                .successful_cancellation_count;
        const std::uint64_t retired_objects =
            observation.load_activity.retired_prepared_count +
            observation.load_activity.retired_resource_count;
        if (successful_cancellations <=
            last_measured_cycle_successful_cancellations_) {
            Fail(
                "A measured cycle did not observe a successful source-load cancellation.");
            return;
        }
        if (retired_objects <=
            last_measured_cycle_retired_objects_) {
            Fail(
                "A measured cycle did not observe background resource retirement.");
            return;
        }
        if (!pending_stress_presentation_ ||
            !pending_baseline_presentation_ ||
            pending_stress_presentation_
                    ->presentation_sequence >=
                pending_baseline_presentation_
                    ->presentation_sequence ||
            pending_stress_presentation_
                    ->render_target_resize_sequence >=
                pending_baseline_presentation_
                    ->render_target_resize_sequence) {
            Fail(
                "A measured cycle did not present distinct stress and baseline snapshots after their render-target resizes.");
            return;
        }
        ++measured_cycles_with_cancellation_;
        ++measured_cycles_with_retirement_;
        last_measured_cycle_successful_cancellations_ =
            successful_cancellations;
        last_measured_cycle_retired_objects_ =
            retired_objects;
        measured_cycle_presentations_.push_back({
            .measured_cycle =
                measured_cycle_presentations_.size() + 1,
            .stress =
                std::move(
                    *pending_stress_presentation_),
            .baseline =
                std::move(
                    *pending_baseline_presentation_),
        });
    }

    pending_stress_presentation_.reset();
    pending_baseline_presentation_.reset();
    ++completed_cycles_;
    const bool final_cycle =
        completed_cycles_ >= TotalCycleCount();
    SetPhase(
        Phase::Settling,
        now,
        final_cycle
            ? configuration_.final_settle_duration
            : configuration_.settle_duration);
}

void RuntimeResourceWorkload::RequestClose(
    const RuntimeResourceWorkloadOperations& operations)
{
    if (close_requested_) {
        return;
    }
    close_requested_ =
        operations.request_close &&
        operations.request_close();
    if (!close_requested_) {
        Fail(
            "Could not post the runtime workload close request.");
    }
}

}  // namespace spectiary
