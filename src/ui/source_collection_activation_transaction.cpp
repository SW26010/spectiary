#include "ui/source_collection_activation_transaction.h"

#include "domain/source_path_identity.h"

#include <stdexcept>
#include <utility>

namespace specforge {

SourceCollectionActivationTransaction::Ticket
SourceCollectionActivationTransaction::ReserveLoad(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    Purpose purpose,
    NavigationLatencyTraceHandle navigation_trace,
    SourceLoadLatencyTraceHandle source_load_trace,
    std::optional<SampleNavigationDirection> prefetch_direction)
{
    const std::string path_key = SourcePathIdentityKey(path);
    return Ticket{
        .path = path,
        .path_key = path_key,
        .spectrum_index = spectrum_index,
        .generation = ++generations_[path_key],
        .activation_epoch = activation_epoch_,
        .purpose = purpose,
        .navigation_trace = std::move(navigation_trace),
        .source_load_trace = std::move(source_load_trace),
        .prefetch_direction = prefetch_direction,
    };
}

void SourceCollectionActivationTransaction::RegisterLoad(
    std::uint64_t task_id,
    Ticket ticket)
{
    const Purpose purpose = ticket.purpose;
    const auto [pending, inserted] =
        pending_loads_.emplace(task_id, std::move(ticket));
    if (!inserted) {
        throw std::logic_error(
            "source activation task ID was already registered");
    }
    if (purpose == Purpose::DeferredRestore) {
        deferred_restore_task_ids_.insert(pending->first);
    }
}

std::vector<SourceCollectionActivationTransaction::PendingTask>
SourceCollectionActivationTransaction::RegisterOrReplaceLoad(
    std::uint64_t task_id,
    Ticket ticket)
{
    std::vector<PendingTask> replaced =
        RemoveLoadsForPath(ticket.path_key, false);
    RegisterLoad(task_id, std::move(ticket));
    return replaced;
}

std::vector<SourceCollectionActivationTransaction::PendingTask>
SourceCollectionActivationTransaction::AdvanceIntent(
    bool preserve_pending_explicit_opens)
{
    ++activation_epoch_;
    std::vector<PendingTask> superseded;
    for (auto pending = pending_loads_.begin();
         pending != pending_loads_.end();) {
        Ticket& ticket = pending->second;
        if (ticket.purpose == Purpose::DeferredRestore) {
            ++pending;
            continue;
        }
        if (preserve_pending_explicit_opens &&
            ticket.purpose == Purpose::ExplicitOpen) {
            ticket.activation_epoch = activation_epoch_;
            ++pending;
            continue;
        }

        const std::uint64_t task_id = pending->first;
        superseded.push_back(PendingTask{
            .task_id = task_id,
            .ticket = std::move(ticket),
        });
        EraseDeferredRestoreTask(task_id);
        pending = pending_loads_.erase(pending);
    }
    return superseded;
}

std::optional<SourceCollectionActivationTransaction::CompletionAdmission>
SourceCollectionActivationTransaction::TakeCompletion(
    std::uint64_t task_id,
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    const auto pending = pending_loads_.find(task_id);
    if (pending == pending_loads_.end()) {
        return std::nullopt;
    }

    Ticket ticket = std::move(pending->second);
    pending_loads_.erase(pending);
    EraseDeferredRestoreTask(task_id);

    const auto current_generation =
        generations_.find(ticket.path_key);
    const bool activation_current =
        ticket.purpose == Purpose::DeferredRestore ||
        ticket.activation_epoch == activation_epoch_;
    const bool accepted =
        activation_current &&
        current_generation != generations_.end() &&
        current_generation->second == ticket.generation &&
        SourcePathIdentityKey(path) == ticket.path_key &&
        spectrum_index == ticket.spectrum_index;
    return CompletionAdmission{
        .ticket = std::move(ticket),
        .accepted = accepted,
    };
}

std::vector<SourceCollectionActivationTransaction::PendingTask>
SourceCollectionActivationTransaction::CancelNonExplicitFollowUps(
    const std::filesystem::path& path)
{
    return RemoveLoadsForPath(
        SourcePathIdentityKey(path),
        true);
}

bool SourceCollectionActivationTransaction::HasMatchingFollowUp(
    const std::filesystem::path& path,
    std::size_t spectrum_index) const
{
    const std::string path_key = SourcePathIdentityKey(path);
    for (const auto& [task_id, ticket] : pending_loads_) {
        (void)task_id;
        if (ticket.path_key == path_key &&
            ticket.spectrum_index == spectrum_index &&
            ticket.purpose != Purpose::ExplicitOpen) {
            return true;
        }
    }
    return false;
}

std::optional<std::uint64_t>
SourceCollectionActivationTransaction::GenerationForPath(
    const std::filesystem::path& path) const
{
    const auto generation =
        generations_.find(SourcePathIdentityKey(path));
    if (generation == generations_.end()) {
        return std::nullopt;
    }
    return generation->second;
}

std::uint64_t
SourceCollectionActivationTransaction::ActivationEpoch() const
{
    return activation_epoch_;
}

std::size_t
SourceCollectionActivationTransaction::PendingLoadCount() const
{
    return pending_loads_.size();
}

bool SourceCollectionActivationTransaction::HasPendingLoads() const
{
    return !pending_loads_.empty();
}

bool SourceCollectionActivationTransaction::HasPendingDeferredRestore()
    const
{
    return !deferred_restore_task_ids_.empty();
}

bool SourceCollectionActivationTransaction::CompletionStartsActivationIntent(
    Purpose purpose,
    bool loaded)
{
    return loaded && purpose == Purpose::ExplicitOpen;
}

std::vector<SourceCollectionActivationTransaction::PendingTask>
SourceCollectionActivationTransaction::RemoveLoadsForPath(
    std::string_view path_key,
    bool retain_explicit_opens)
{
    std::vector<PendingTask> removed;
    for (auto pending = pending_loads_.begin();
         pending != pending_loads_.end();) {
        Ticket& ticket = pending->second;
        if (ticket.path_key != path_key ||
            (retain_explicit_opens &&
             ticket.purpose == Purpose::ExplicitOpen)) {
            ++pending;
            continue;
        }

        const std::uint64_t task_id = pending->first;
        removed.push_back(PendingTask{
            .task_id = task_id,
            .ticket = std::move(ticket),
        });
        EraseDeferredRestoreTask(task_id);
        pending = pending_loads_.erase(pending);
    }
    return removed;
}

void SourceCollectionActivationTransaction::EraseDeferredRestoreTask(
    std::uint64_t task_id)
{
    deferred_restore_task_ids_.erase(task_id);
}

}  // namespace specforge
