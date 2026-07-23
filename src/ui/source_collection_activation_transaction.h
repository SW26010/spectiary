#pragma once

#include "domain/sample_navigation_direction.h"
#include "profile/navigation_latency_trace.h"
#include "profile/source_load_latency_trace.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

class SourceCollectionActivationTransaction {
public:
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
        std::optional<SampleNavigationDirection> prefetch_direction;
    };

    struct PendingTask {
        std::uint64_t task_id = 0;
        Ticket ticket;
    };

    struct CompletionAdmission {
        Ticket ticket;
        bool accepted = false;
    };

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
    [[nodiscard]] std::vector<PendingTask> CancelNonExplicitFollowUps(
        const std::filesystem::path& path);

    [[nodiscard]] bool HasMatchingFollowUp(
        const std::filesystem::path& path,
        std::size_t spectrum_index) const;
    [[nodiscard]] std::optional<std::uint64_t> GenerationForPath(
        const std::filesystem::path& path) const;
    [[nodiscard]] std::uint64_t ActivationEpoch() const;
    [[nodiscard]] std::size_t PendingLoadCount() const;
    [[nodiscard]] bool HasPendingLoads() const;
    [[nodiscard]] bool HasPendingDeferredRestore() const;

    [[nodiscard]] static bool CompletionStartsActivationIntent(
        Purpose purpose,
        bool loaded);

private:
    [[nodiscard]] std::vector<PendingTask> RemoveLoadsForPath(
        std::string_view path_key,
        bool retain_explicit_opens);
    void EraseDeferredRestoreTask(std::uint64_t task_id);

    std::unordered_map<std::uint64_t, Ticket> pending_loads_;
    std::unordered_map<std::string, std::uint64_t> generations_;
    std::unordered_set<std::uint64_t> deferred_restore_task_ids_;
    std::uint64_t activation_epoch_ = 0;
};

}  // namespace specforge
