#include "app/runtime_resource_workload_internal.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Workload = spectiary::RuntimeResourceWorkload;
using WorkloadAccess =
    spectiary::RuntimeResourceWorkloadTestAccess;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct DeterministicWorkloadHost {
    explicit DeterministicWorkloadHost(
        Workload& owned_workload)
        : workload(owned_workload)
    {
    }

    spectiary::RuntimeResourceWorkloadOperations
    Operations()
    {
        return {
            .observe = [this]() {
                return observation;
            },
            .write_status =
                [this](
                    const spectiary::
                        ShellRuntimeResourceObservation&) {
                    ++status_write_count;
                    return true;
                },
            .request_resize =
                [this](int width, int height) {
                    resize_requests.push_back({
                        width,
                        height,
                    });
                    workload.RecordRenderTargetResize();
                },
            .arm_cancellation_checkpoint =
                [this]() {
                    ++checkpoint_arm_count;
                    return true;
                },
            .open_source =
                [this](
                    const std::filesystem::path& path) {
                    opened_sources.push_back(path);
                },
            .request_close =
                [this]() {
                    ++close_request_count;
                    return true;
                },
        };
    }

    void Present(
        std::uint64_t sequence,
        std::uint64_t source_load_id,
        std::uint64_t frame,
        std::string_view source_id,
        const std::filesystem::path& path)
    {
        observation.presented_source_load = {
            .sequence = sequence,
            .source_load_id = source_load_id,
            .activation_frame = frame,
            .viewport_id = 7,
            .source_id = std::string(source_id),
            .source_path = path,
            .spectrum_index = 0,
        };
    }

    Workload& workload;
    spectiary::ShellRuntimeResourceObservation
        observation;
    std::vector<std::filesystem::path>
        opened_sources;
    std::vector<std::pair<int, int>>
        resize_requests;
    std::size_t checkpoint_arm_count = 0;
    std::size_t status_write_count = 0;
    std::size_t close_request_count = 0;
};

void TestDeterministicSuccessfulLifecycle()
{
    const std::filesystem::path baseline =
        L"C:\\fixtures\\baseline.csv";
    const std::filesystem::path stress =
        L"C:\\fixtures\\stress.csv";
    spectiary::RuntimeResourceWorkloadConfiguration
        configuration;
    configuration.status_path =
        L"C:\\fixtures\\status.json";
    configuration.source_paths = {
        baseline,
        stress,
    };
    configuration.warmup_cycles = 1;
    configuration.measured_cycles = 1;
    configuration.burst_count = 2;
    configuration.settle_duration = 5ms;
    configuration.final_settle_duration = 5ms;
    configuration.poll_interval = 1ms;
    configuration.phase_timeout = 1s;

    Workload workload(std::move(configuration));
    DeterministicWorkloadHost host(workload);
    const auto operations = host.Operations();
    Workload::TimePoint now =
        Workload::TimePoint{} + 1s;
    const auto service =
        [&](std::chrono::milliseconds advance) {
            now += advance;
            WorkloadAccess::Service(
                workload,
                operations,
                now);
        };

    WorkloadAccess::Start(
        workload,
        17,
        operations,
        now);

    service(2ms);
    host.observation.active_source_path = stress;
    service(2ms);
    service(2ms);
    host.observation.active_source_path = baseline;
    service(2ms);

    service(2ms);
    host.observation.load_activity
        .runtime_resource_cancellation_checkpoint_waiting =
            true;
    service(2ms);
    host.observation.load_activity
        .runtime_resource_cancellation_checkpoint_waiting =
            false;
    host.observation.load_activity
        .successful_cancellation_count = 1;
    host.observation.load_activity
        .retired_resource_count = 1;
    host.observation.active_source_path = stress;
    service(2ms);
    host.Present(
        1,
        10,
        1,
        "stress",
        stress);
    service(2ms);
    service(2ms);
    host.observation.active_source_path = baseline;
    service(2ms);
    host.Present(
        2,
        11,
        2,
        "baseline",
        baseline);
    service(2ms);

    service(6ms);
    service(2ms);
    host.observation.load_activity
        .runtime_resource_cancellation_checkpoint_waiting =
            true;
    service(2ms);
    host.observation.load_activity
        .runtime_resource_cancellation_checkpoint_waiting =
            false;
    host.observation.load_activity
        .successful_cancellation_count = 2;
    host.observation.load_activity
        .retired_resource_count = 2;
    host.observation.active_source_path = stress;
    service(2ms);
    host.Present(
        3,
        20,
        3,
        "stress",
        stress);
    service(2ms);
    service(2ms);
    host.observation.active_source_path = baseline;
    service(2ms);
    host.Present(
        4,
        21,
        4,
        "baseline",
        baseline);
    service(2ms);

    service(6ms);
    service(2ms);

    Require(
        WorkloadAccess::StateName(workload) ==
                "completed" &&
            WorkloadAccess::PhaseName(workload) ==
                "completed",
        "the deterministic workload should reach its successful terminal state");
    Require(
        WorkloadAccess::CompletedMeasuredCycles(
            workload) == 1 &&
            WorkloadAccess::
                MeasuredCyclePresentationCount(
                    workload) == 1,
        "the successful path should record one complete measured presentation pair");
    Require(
        host.checkpoint_arm_count == 2 &&
            host.resize_requests.size() == 5 &&
            host.opened_sources ==
                std::vector<std::filesystem::path>{
                    stress,
                    baseline,
                    stress,
                    stress,
                    baseline,
                    stress,
                    stress,
                    baseline,
                } &&
            host.close_request_count == 1 &&
            workload.exit_code() == 0 &&
            !workload.next_deadline(),
        "the successful path should exercise both cancellation handshakes, every resize, and one close");

    service(2ms);
    Require(
        host.close_request_count == 1,
        "terminal service must not duplicate the close request");
}

void TestLocalStateFlushFailureBecomesRunnerFailure()
{
    spectiary::RuntimeResourceWorkloadConfiguration
        configuration;
    configuration.status_path =
        L"C:\\fixtures\\status.json";
    configuration.source_paths = {
        L"C:\\fixtures\\baseline.csv",
        L"C:\\fixtures\\stress.csv",
    };
    Workload workload(std::move(configuration));

    workload.RecordLocalStateFlushFailure(
        "Panel visibility: save failed.");

    Require(
        WorkloadAccess::StateName(workload) == "failed" &&
            workload.exit_code() == 2,
        "local-state flush failure should fail the non-interactive runner without requiring a dialog");
}

}  // namespace

int main()
{
    try {
        TestDeterministicSuccessfulLifecycle();
        TestLocalStateFlushFailureBecomesRunnerFailure();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
