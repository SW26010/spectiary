#pragma once

#include "app/runtime_resource_workload.h"
#include "ui/shell_ui.h"

#include <functional>
#include <string_view>

namespace spectiary {

struct RuntimeResourceWorkloadOperations {
    std::function<ShellRuntimeResourceObservation()>
        observe;
    std::function<bool(
        const ShellRuntimeResourceObservation&)>
        write_status;
    std::function<void(int width, int height)>
        request_resize;
    std::function<bool()> arm_cancellation_checkpoint;
    std::function<void(const std::filesystem::path&)>
        open_source;
    std::function<bool()> request_close;
};

struct RuntimeResourceWorkloadTestAccess {
    static void Start(
        RuntimeResourceWorkload& workload,
        std::uint32_t process_id,
        const RuntimeResourceWorkloadOperations&
            operations,
        RuntimeResourceWorkload::TimePoint now)
    {
        workload.StartWithOperations(
            process_id,
            operations,
            now);
    }

    static void Service(
        RuntimeResourceWorkload& workload,
        const RuntimeResourceWorkloadOperations&
            operations,
        RuntimeResourceWorkload::TimePoint now)
    {
        workload.ServiceWithOperations(
            operations,
            now);
    }

    [[nodiscard]] static std::string_view StateName(
        const RuntimeResourceWorkload& workload) noexcept
    {
        return workload.StateName();
    }

    [[nodiscard]] static std::string_view PhaseName(
        const RuntimeResourceWorkload& workload) noexcept
    {
        return workload.PhaseName();
    }

    [[nodiscard]] static std::size_t
    CompletedMeasuredCycles(
        const RuntimeResourceWorkload& workload) noexcept
    {
        return workload.CompletedMeasuredCycles();
    }

    [[nodiscard]] static std::size_t
    MeasuredCyclePresentationCount(
        const RuntimeResourceWorkload& workload) noexcept
    {
        return workload.measured_cycle_presentations_.size();
    }
};

}  // namespace spectiary
