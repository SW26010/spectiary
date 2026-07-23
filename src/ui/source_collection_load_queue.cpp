#include "ui/source_collection_load_queue.h"

#include "domain/spectrum_loader.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/sample_workflow_state_cache_io.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace specforge {
namespace {

class SourceLoadCanceled : public std::runtime_error {
public:
    SourceLoadCanceled()
        : std::runtime_error("source loading was canceled")
    {
    }
};

void ValidateDecodedSnapshot(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        throw std::runtime_error("The source decoder returned no result.");
    }
    const auto error = std::find_if(
        snapshot->diagnostics.begin(),
        snapshot->diagnostics.end(),
        [](const SpectrumDiagnostic& diagnostic) {
            return diagnostic.severity == SpectrumDiagnosticSeverity::Error;
        });
    if (error != snapshot->diagnostics.end()) {
        throw std::runtime_error(
            error->message.empty() ? "The source decoder rejected this input." : error->message);
    }
}

void LoadRestoredAnnotations(
    SourceCollectionContext& context,
    const std::vector<std::filesystem::path>& annotation_paths,
    const SourceCollectionCancellationCheckpoint& checkpoint)
{
    for (const std::filesystem::path& annotation_path : annotation_paths) {
        checkpoint();
        if (SourceCollectionManifestContainsAnnotation(context.manifest, annotation_path)) {
            continue;
        }
        (void)IngestReadOnlySampleAnnotationCancelable(
            context.manifest,
            annotation_path,
            context.identity.spectrum_count,
            checkpoint);
    }
}

bool CanReusePreparedWorkflow(
    const SourceCollectionIdentity& actual,
    const std::optional<SourceCollectionIdentity>& expected)
{
    return expected && actual.id == expected->id &&
           actual.context_fingerprint == expected->context_fingerprint &&
           actual.spectrum_count == expected->spectrum_count;
}

SourceCollectionLoadDependencies DefaultDependencies()
{
    SourceCollectionLoadDependencies dependencies;
    dependencies.snapshot_loader = LoadSpectrumSnapshotFromPathCancelable;
    dependencies.folder_snapshot_loader = LoadFolderSpectrumSnapshotFromListingCancelable;
    dependencies.folder_scanner =
        [](const std::filesystem::path& path,
           const SourceCollectionCancellationCheckpoint& checkpoint) {
            return ScanSourceCollectionFolder(path, {}, checkpoint);
        };
    dependencies.workflow_cache_loader = LoadSampleWorkflowPreparationCacheBundle;
    dependencies.workflow_cache_paths = {
        DefaultSampleLabelingStateCachePath(),
        DefaultSampleWorkflowStateCachePath(),
    };
    return dependencies;
}

void FillMissingDependencies(SourceCollectionLoadDependencies& dependencies)
{
    SourceCollectionLoadDependencies defaults = DefaultDependencies();
    if (!dependencies.snapshot_loader) {
        dependencies.snapshot_loader = std::move(defaults.snapshot_loader);
    }
    if (!dependencies.folder_snapshot_loader) {
        dependencies.folder_snapshot_loader = std::move(defaults.folder_snapshot_loader);
    }
    if (!dependencies.folder_scanner) {
        dependencies.folder_scanner = std::move(defaults.folder_scanner);
    }
    if (!dependencies.workflow_cache_loader) {
        dependencies.workflow_cache_loader = std::move(defaults.workflow_cache_loader);
    }
    if (dependencies.workflow_cache_paths.labeling_state_cache_path.empty()) {
        dependencies.workflow_cache_paths.labeling_state_cache_path =
            std::move(defaults.workflow_cache_paths.labeling_state_cache_path);
    }
    if (dependencies.workflow_cache_paths.workflow_state_cache_path.empty()) {
        dependencies.workflow_cache_paths.workflow_state_cache_path =
            std::move(defaults.workflow_cache_paths.workflow_state_cache_path);
    }
}

}  // namespace

class SourceCollectionLoadQueue::Impl {
public:
    explicit Impl(SourceCollectionLoadDependencies dependencies)
        : dependencies_(std::move(dependencies)),
          retirement_worker_([this](std::stop_token stop_token) {
              RetirementLoop(stop_token);
          })
    {
        if (!dependencies_.folder_change_generation_factory) {
            if (dependencies_.folder_change_generation_registration_factory) {
                directory_change_generation_monitor_ =
                    std::make_unique<DirectoryChangeGenerationMonitor>(
                        std::move(
                            dependencies_
                                .folder_change_generation_registration_factory));
            } else {
                directory_change_generation_monitor_ =
                    std::make_unique<DirectoryChangeGenerationMonitor>();
            }
            dependencies_.folder_change_generation_factory =
                [monitor = directory_change_generation_monitor_.get()](
                    const std::filesystem::path& path,
                    const SourceCollectionCancellationCheckpoint& checkpoint) {
                    return monitor->Begin(path, checkpoint);
                };
        }
    }

    ~Impl()
    {
        UnregisterCompletionReadyCallback();
        StopLoadWorkers();
        retirement_worker_.request_stop();
        retirement_condition_.notify_all();
        if (retirement_worker_.joinable()) {
            retirement_worker_.join();
        }
    }

    struct BatchCompletionSlot {
        std::uint64_t task_id = 0;
        std::shared_ptr<std::atomic_bool> canceled;
        bool terminal = false;
        std::optional<SourceCollectionLoadCompletion> completion;
    };

    struct BatchState {
        std::once_flag load_once;
        std::shared_ptr<const SampleWorkflowPreparationCacheBundle> value;
        std::vector<BatchCompletionSlot> completions;
        std::size_t next_completion = 0;
        bool admitted = false;
        bool abandoned = false;
    };

    struct Task {
        std::uint64_t id = 0;
        SourceCollectionLoadRequest request;
        std::shared_ptr<std::atomic_bool> canceled;
        std::shared_ptr<std::atomic_bool> finished;
        std::shared_ptr<BatchState> batch;
        std::size_t batch_index = 0;
        std::shared_ptr<BatchCompletionSlot> ordered_completion;
    };

    struct Worker {
        Worker(
            std::shared_ptr<std::atomic_bool> finished_flag,
            Impl* owner,
            Task task)
            : finished(std::move(finished_flag)),
              thread([owner,
                      task = std::move(task),
                      finished_flag = finished](std::stop_token stop_token) mutable {
                  owner->RunTask(task, stop_token);
                  finished_flag->store(true, std::memory_order_release);
              })
        {
        }

        Worker(Worker&&) noexcept = default;
        Worker& operator=(Worker&&) noexcept = default;
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;

        std::shared_ptr<std::atomic_bool> finished;
        std::jthread thread;
    };

    std::uint64_t Enqueue(SourceCollectionLoadRequest request)
    {
        ReapFinishedWorkers();
        std::uint64_t id = 0;
        bool completion_became_ready = false;
        {
            std::lock_guard lock(mutex_);
            const bool completed_was_empty = completed_.empty();
            auto completion = std::make_shared<BatchCompletionSlot>();
            ordered_completions_.push_back(completion);
            try {
                id = StartTaskLocked(std::move(request), nullptr, 0, std::move(completion));
            } catch (...) {
                ordered_completions_.pop_back();
                throw;
            }
            completion_became_ready = completed_was_empty && !completed_.empty();
        }
        if (completion_became_ready) {
            NotifyCompletionReady();
        }
        return id;
    }

    std::vector<std::uint64_t> EnqueueBatch(std::vector<SourceCollectionLoadRequest> requests)
    {
        ReapFinishedWorkers();
        std::vector<std::uint64_t> ids;
        ids.reserve(requests.size());
        if (requests.empty()) {
            return ids;
        }
        bool completion_became_ready = false;
        {
            std::lock_guard lock(mutex_);
            const bool completed_was_empty = completed_.empty();
            auto batch = std::make_shared<BatchState>();
            batch->completions.resize(requests.size());
            try {
                for (std::size_t index = 0; index < requests.size(); ++index) {
                    ids.push_back(StartTaskLocked(std::move(requests[index]), batch, index));
                }
                batch->admitted = true;
                PublishReadyBatchCompletions(*batch);
            } catch (...) {
                batch->abandoned = true;
                for (const std::uint64_t id : ids) {
                    const auto match = cancellation_.find(id);
                    if (match != cancellation_.end()) {
                        match->second->store(true, std::memory_order_relaxed);
                        cancellation_.erase(match);
                    }
                }
                throw;
            }
            completion_became_ready = completed_was_empty && !completed_.empty();
        }
        if (completion_became_ready) {
            NotifyCompletionReady();
        }
        return ids;
    }

    void Cancel(std::uint64_t task_id)
    {
        std::lock_guard lock(mutex_);
        const auto match = cancellation_.find(task_id);
        if (match != cancellation_.end()) {
            match->second->store(true, std::memory_order_relaxed);
        }
    }

    std::vector<SourceCollectionLoadCompletion> TakeCompleted()
    {
        std::vector<SourceCollectionLoadCompletion> result;
        std::vector<std::jthread> finished_workers;
        {
            std::lock_guard lock(mutex_);
            result.reserve(completed_.size());
            while (!completed_.empty()) {
                result.push_back(std::move(completed_.front()));
                completed_.pop_front();
            }
            CollectFinishedWorkersLocked(finished_workers);
        }
        return result;
    }

    bool NeedsService() const
    {
        std::lock_guard lock(mutex_);
        return active_task_count_ > 0 || !completed_.empty();
    }

    void RegisterCompletionReadyCallback(CompletionReadyCallback callback)
    {
        if (!callback) {
            throw std::invalid_argument("completion-ready callback must not be empty");
        }
        std::unique_lock lock(completion_callback_mutex_);
        if (completion_ready_callback_) {
            throw std::logic_error("completion-ready callback is already registered");
        }
        completion_callback_idle_.wait(lock, [this]() {
            return completion_callbacks_in_flight_ == 0;
        });
        completion_ready_callback_ = std::move(callback);
    }

    void UnregisterCompletionReadyCallback()
    {
        std::unique_lock lock(completion_callback_mutex_);
        completion_ready_callback_ = {};
        completion_callback_idle_.wait(lock, [this]() {
            return completion_callbacks_in_flight_ == 0;
        });
    }

    void RetirePrepared(PreparedSourceCollection prepared)
    {
        {
            std::lock_guard lock(mutex_);
            retired_prepared_.push_back(std::move(prepared));
        }
        retirement_condition_.notify_one();
    }

    void RetireResource(BackgroundRetirementHandle resource)
    {
        if (!resource) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            retired_resources_.push_back(std::move(resource));
        }
        retirement_condition_.notify_one();
    }

private:
    void NotifyCompletionReady() noexcept
    {
        CompletionReadyCallback callback;
        try {
            std::lock_guard lock(completion_callback_mutex_);
            if (!completion_ready_callback_) {
                return;
            }
            callback = completion_ready_callback_;
            ++completion_callbacks_in_flight_;
        } catch (...) {
            return;
        }

        try {
            callback();
        } catch (...) {
        }

        {
            std::lock_guard lock(completion_callback_mutex_);
            --completion_callbacks_in_flight_;
        }
        completion_callback_idle_.notify_all();
    }

    std::uint64_t StartTaskLocked(
        SourceCollectionLoadRequest request,
        std::shared_ptr<BatchState> batch,
        std::size_t batch_index = 0,
        std::shared_ptr<BatchCompletionSlot> ordered_completion = nullptr)
    {
        const std::uint64_t id = next_task_id_++;
        if (request.navigation_attempt) {
            request.navigation_attempt->MarkSourceTaskId(id);
        }
        auto canceled = std::make_shared<std::atomic_bool>(false);
        auto finished = std::make_shared<std::atomic_bool>(false);
        const std::filesystem::path failure_path = request.path;
        const std::size_t failure_spectrum_index = request.spectrum_index;
        NavigationLatencyAttemptHandle failure_navigation_attempt = request.navigation_attempt;
        std::shared_ptr<BatchState> failure_batch = batch;
        std::shared_ptr<BatchCompletionSlot> failure_ordered_completion = ordered_completion;
        Task task{
            id,
            std::move(request),
            canceled,
            finished,
            std::move(batch),
            batch_index,
            std::move(ordered_completion),
        };
        cancellation_.emplace(id, canceled);
        ++active_task_count_;
        try {
            workers_.emplace_back(std::move(finished), this, std::move(task));
        } catch (const std::system_error& error) {
            --active_task_count_;
            SourceCollectionLoadCompletion completion;
            completion.task_id = id;
            completion.path = failure_path;
            completion.spectrum_index = failure_spectrum_index;
            completion.error_message =
                std::string("Could not start the source loading thread: ") + error.what();
            completion.navigation_attempt = std::move(failure_navigation_attempt);
            if (completion.navigation_attempt) {
                completion.navigation_attempt->MarkCompletionReady();
            }
            if (failure_batch) {
                FinishBatchTask(
                    *failure_batch,
                    batch_index,
                    id,
                    canceled,
                    std::move(completion));
            } else {
                FinishOrderedTask(
                    *failure_ordered_completion,
                    id,
                    canceled,
                    std::move(completion));
            }
            return id;
        } catch (...) {
            cancellation_.erase(id);
            --active_task_count_;
            throw;
        }
        return id;
    }

    void CollectFinishedWorkersLocked(std::vector<std::jthread>& finished_workers)
    {
        for (auto worker = workers_.begin(); worker != workers_.end();) {
            if (!worker->finished->load(std::memory_order_acquire)) {
                ++worker;
                continue;
            }
            finished_workers.push_back(std::move(worker->thread));
            worker = workers_.erase(worker);
        }
    }

    void ReapFinishedWorkers()
    {
        std::vector<std::jthread> finished_workers;
        {
            std::lock_guard lock(mutex_);
            CollectFinishedWorkersLocked(finished_workers);
        }
    }

    void StopLoadWorkers()
    {
        std::vector<std::jthread> workers;
        {
            std::lock_guard lock(mutex_);
            workers.reserve(workers_.size());
            for (const auto& [id, canceled] : cancellation_) {
                (void)id;
                canceled->store(true, std::memory_order_relaxed);
            }
            for (Worker& worker : workers_) {
                worker.thread.request_stop();
                workers.push_back(std::move(worker.thread));
            }
            workers_.clear();
        }
    }

    SourceCollectionCancellationCheckpoint CheckpointFor(
        const Task& task,
        std::stop_token stop_token) const
    {
        return [canceled = task.canceled, stop_token]() {
            if (stop_token.stop_requested() || canceled->load(std::memory_order_relaxed)) {
                throw SourceLoadCanceled();
            }
        };
    }

    std::shared_ptr<const SampleWorkflowPreparationCacheBundle> WorkflowCache(
        const Task& task,
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        if (!task.batch) {
            return std::make_shared<SampleWorkflowPreparationCacheBundle>(
                dependencies_.workflow_cache_loader(
                    dependencies_.workflow_cache_paths,
                    checkpoint));
        }
        std::call_once(task.batch->load_once, [this, &task, &checkpoint]() {
            task.batch->value = std::make_shared<SampleWorkflowPreparationCacheBundle>(
                dependencies_.workflow_cache_loader(
                    dependencies_.workflow_cache_paths,
                    checkpoint));
        });
        checkpoint();
        return task.batch->value;
    }

    PreparedSampleWorkflowState PrepareWorkflow(
        const Task& task,
        const SpectrumSnapshot& snapshot,
        const SourceCollectionContext& context,
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        const std::shared_ptr<const SampleWorkflowPreparationCacheBundle> cache =
            WorkflowCache(task, checkpoint);
        PreparedSampleWorkflowState prepared = PrepareSampleWorkflowStateFromCache(
            snapshot,
            context,
            task.request.spectrum_index,
            *cache,
            nullptr,
            nullptr,
            checkpoint);
        prepared.preparation_cache = cache;
        return prepared;
    }

    void FinalizeContext(
        const Task& task,
        SourceCollectionContext& context,
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        LoadRestoredAnnotations(context, task.request.annotation_paths, checkpoint);
        FinalizeSourceCollectionAnnotationContextFingerprint(
            context,
            task.request.annotation_paths,
            checkpoint);
    }

    PreparedSourceCollection BuildPrepared(
        const Task& task,
        SpectrumSnapshotHandle snapshot,
        SourceCollectionContext context,
        const SourceCollectionCancellationCheckpoint& checkpoint,
        SourceCollectionFolderListingGenerationHandle folder_listing_generation = {})
    {
        if (CanReusePreparedWorkflow(context.identity, task.request.reuse_identity)) {
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkWorkflowReused(true);
            }
            SourceCollectionIdentity identity = context.identity;
            PreparedSourceCollection prepared{
                task.id,
                task.request.path,
                task.request.spectrum_index,
                std::move(snapshot),
                PreparedSourceCollectionReuse{std::move(identity)},
            };
            prepared.folder_listing_generation = std::move(folder_listing_generation);
            return prepared;
        }
        if (task.request.navigation_attempt) {
            task.request.navigation_attempt->MarkWorkflowReused(false);
        }
        PreparedSampleWorkflowState workflow =
            PrepareWorkflow(task, *snapshot, context, checkpoint);
        PreparedSourceCollection prepared{
            task.id,
            task.request.path,
            task.request.spectrum_index,
            std::move(snapshot),
            PreparedSourceCollectionPlan{
                std::move(context),
                std::move(workflow),
                task.request.base_live_workflow_revision},
        };
        prepared.folder_listing_generation = std::move(folder_listing_generation);
        return prepared;
    }

    SourceCollectionFolderListingGenerationHandle ScanFolderListingGeneration(
        const std::filesystem::path& path,
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        DirectoryChangeGenerationHandle change_generation =
            dependencies_.folder_change_generation_factory(path, checkpoint);
        SourceCollectionFolderListing listing =
            dependencies_.folder_scanner(path, checkpoint);
        return std::make_shared<const SourceCollectionFolderListingGeneration>(
            SourceCollectionFolderListingGeneration{
                std::move(listing),
                std::move(change_generation),
            });
    }

    PreparedSourceCollection PrepareFolder(
        const Task& task,
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        constexpr std::size_t kMaximumAttempts = 2;
        SourceCollectionFolderListingGenerationHandle listing_generation =
            task.request.folder_listing_generation_hint;
        for (std::size_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
            checkpoint();
            const bool hint_present = listing_generation != nullptr;
            const bool generation_current_at_start =
                listing_generation &&
                listing_generation->change_generation &&
                listing_generation->IsCurrent();
            bool listing_scan_performed = false;
            // A retained generation may be stale. Poll its invalidation token
            // and validate the requested file cheaply before decoding so
            // deletion or replacement refreshes the full listing instead of
            // surfacing a stale-path decode error.
            if (listing_generation &&
                ((listing_generation->change_generation &&
                  !generation_current_at_start) ||
                 task.request.spectrum_index >=
                     listing_generation->listing.spectra.size() ||
                 !SourceCollectionFolderSpectrumFileMatchesCurrentState(
                     listing_generation
                         ->listing.spectra[task.request.spectrum_index]))) {
                listing_generation.reset();
            }
            if (!listing_generation) {
                listing_generation =
                    ScanFolderListingGeneration(task.request.path, checkpoint);
                listing_scan_performed = true;
            }
            const SourceCollectionFolderListing& listing =
                listing_generation->listing;
            const SourceCollectionSingleFileState initial_state =
                CaptureSourceCollectionSingleFileState(
                    task.request.path,
                    task.request.annotation_paths,
                    checkpoint);
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkFolderSnapshotLoadStarted({
                    .hint_present = hint_present,
                    .generation_current_at_start =
                        generation_current_at_start,
                    .listing_scan_performed = listing_scan_performed,
                });
            }
            SpectrumSnapshotHandle snapshot = dependencies_.folder_snapshot_loader(
                task.request.path,
                task.request.spectrum_index,
                listing,
                [checkpoint]() {
                    try {
                        checkpoint();
                        return false;
                    } catch (const SourceLoadCanceled&) {
                        return true;
                    }
                });
            checkpoint();
            ValidateDecodedSnapshot(snapshot);
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkSnapshotLoadFinished();
            }
            SourceCollectionContext context = BuildFolderSourceCollectionContextCancelable(
                *snapshot,
                listing,
                checkpoint);
            FinalizeContext(task, context, checkpoint);
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkContextPrepared();
            }
            const SourceCollectionSingleFileState verified_state =
                CaptureSourceCollectionSingleFileState(
                    task.request.path,
                    task.request.annotation_paths,
                    checkpoint);
            SourceCollectionFolderListingGenerationHandle verified_generation =
                listing_generation;
            bool listing_generation_is_current = false;
            if (listing_generation->change_generation) {
                listing_generation_is_current = listing_generation->IsCurrent();
            } else {
                // Change notifications are an optimization. If the platform
                // cannot provide a generation boundary, preserve the previous
                // full post-decode comparison as the correctness fallback.
                verified_generation =
                    ScanFolderListingGeneration(task.request.path, checkpoint);
                if (task.request.navigation_attempt) {
                    task.request.navigation_attempt
                        ->MarkFolderListingScanPerformed();
                }
                listing_generation_is_current =
                    SourceCollectionFolderListingsMatch(
                        listing,
                        verified_generation->listing,
                        checkpoint) &&
                    (!verified_generation->change_generation ||
                     verified_generation->IsCurrent());
            }
            const bool revalidation_succeeded =
                SourceCollectionSingleFileStatesMatch(initial_state, verified_state) &&
                listing_generation_is_current;
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkSourceRevalidated(
                    revalidation_succeeded);
            }
            if (revalidation_succeeded) {
                return BuildPrepared(
                    task,
                    std::move(snapshot),
                    std::move(context),
                    checkpoint,
                    std::move(verified_generation));
            }
            if (listing_generation_is_current) {
                // The folder generation is still reusable when only an
                // annotation dependency changed during preparation.
                listing_generation = std::move(verified_generation);
            } else if (!listing_generation->change_generation &&
                       verified_generation &&
                       (!verified_generation->change_generation ||
                        verified_generation->IsCurrent())) {
                // In fallback mode, promote the fresh observation to the next
                // attempt just as the previous scan-and-compare path did.
                listing_generation = std::move(verified_generation);
            } else {
                listing_generation.reset();
            }
        }
        throw std::runtime_error(
            "The source folder kept changing while it was loaded; try again after synchronization settles.");
    }

    PreparedSourceCollection PrepareFile(
        const Task& task,
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        constexpr std::size_t kMaximumAttempts = 2;
        for (std::size_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
            checkpoint();
            const SourceCollectionSingleFileState initial_state =
                CaptureSourceCollectionSingleFileState(
                    task.request.path,
                    task.request.annotation_paths,
                    checkpoint);
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkSnapshotLoadStarted(false);
            }
            SpectrumSnapshotHandle snapshot = dependencies_.snapshot_loader(
                task.request.path,
                task.request.spectrum_index,
                [checkpoint]() {
                    try {
                        checkpoint();
                        return false;
                    } catch (const SourceLoadCanceled&) {
                        return true;
                    }
                });
            checkpoint();
            ValidateDecodedSnapshot(snapshot);
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkSnapshotLoadFinished();
            }
            SourceCollectionContext context = LoadSourceCollectionContextCancelable(
                *snapshot,
                initial_state,
                checkpoint);
            FinalizeContext(task, context, checkpoint);
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkContextPrepared();
            }
            const SourceCollectionSingleFileState verified_state =
                CaptureSourceCollectionSingleFileState(
                    task.request.path,
                    task.request.annotation_paths,
                    checkpoint);
            const bool revalidation_succeeded =
                SourceCollectionSingleFileStatesMatch(initial_state, verified_state);
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkSourceRevalidated(
                    revalidation_succeeded);
            }
            if (revalidation_succeeded) {
                return BuildPrepared(task, std::move(snapshot), std::move(context), checkpoint);
            }
        }
        throw std::runtime_error(
            "The source file or one of its companions kept changing while it was loaded; "
            "try again after synchronization settles.");
    }

    PreparedSourceCollection Prepare(const Task& task, std::stop_token stop_token)
    {
        const SourceCollectionCancellationCheckpoint checkpoint =
            CheckpointFor(task, stop_token);
        checkpoint();
        std::error_code directory_error;
        const bool is_directory =
            std::filesystem::is_directory(task.request.path, directory_error);
        if (!directory_error && is_directory) {
            return PrepareFolder(task, checkpoint);
        }
        return PrepareFile(task, checkpoint);
    }

    void PublishCompletion(BatchCompletionSlot& slot)
    {
        const bool canceled =
            slot.canceled && slot.canceled->load(std::memory_order_relaxed);
        if (slot.completion) {
            if (!canceled) {
                if (slot.completion->navigation_attempt) {
                    slot.completion->navigation_attempt->MarkCompletionPublished();
                }
                completed_.push_back(std::move(*slot.completion));
            } else if (slot.completion->prepared) {
                retired_prepared_.push_back(std::move(*slot.completion->prepared));
                retirement_condition_.notify_one();
            }
            slot.completion.reset();
        }
        cancellation_.erase(slot.task_id);
    }

    void PublishReadyBatchCompletions(BatchState& batch)
    {
        if (!batch.admitted || batch.abandoned) {
            return;
        }
        while (batch.next_completion < batch.completions.size()) {
            BatchCompletionSlot& slot = batch.completions[batch.next_completion];
            if (!slot.terminal) {
                return;
            }
            PublishCompletion(slot);
            ++batch.next_completion;
        }
    }

    void FinishBatchTask(
        BatchState& batch,
        std::size_t batch_index,
        std::uint64_t task_id,
        std::shared_ptr<std::atomic_bool> canceled,
        std::optional<SourceCollectionLoadCompletion> completion)
    {
        BatchCompletionSlot& slot = batch.completions[batch_index];
        slot.task_id = task_id;
        slot.canceled = std::move(canceled);
        slot.terminal = true;
        slot.completion = std::move(completion);
        PublishReadyBatchCompletions(batch);
    }

    void FinishBatchTask(
        const Task& task,
        std::optional<SourceCollectionLoadCompletion> completion)
    {
        FinishBatchTask(
            *task.batch,
            task.batch_index,
            task.id,
            task.canceled,
            std::move(completion));
    }

    void PublishReadyOrderedCompletions()
    {
        while (!ordered_completions_.empty() && ordered_completions_.front()->terminal) {
            PublishCompletion(*ordered_completions_.front());
            ordered_completions_.pop_front();
        }
    }

    void FinishOrderedTask(
        BatchCompletionSlot& slot,
        std::uint64_t task_id,
        std::shared_ptr<std::atomic_bool> canceled,
        std::optional<SourceCollectionLoadCompletion> completion)
    {
        slot.task_id = task_id;
        slot.canceled = std::move(canceled);
        slot.terminal = true;
        slot.completion = std::move(completion);
        PublishReadyOrderedCompletions();
    }

    void FinishOrderedTask(
        const Task& task,
        std::optional<SourceCollectionLoadCompletion> completion)
    {
        FinishOrderedTask(
            *task.ordered_completion,
            task.id,
            task.canceled,
            std::move(completion));
    }

    void FinishTask(const Task& task, SourceCollectionLoadCompletion completion)
    {
        bool completion_became_ready = false;
        {
            std::lock_guard lock(mutex_);
            const bool completed_was_empty = completed_.empty();
            if (active_task_count_ > 0) {
                --active_task_count_;
            }
            if (task.batch) {
                FinishBatchTask(
                    task,
                    task.canceled->load(std::memory_order_relaxed)
                        ? std::nullopt
                        : std::optional<SourceCollectionLoadCompletion>{std::move(completion)});
            } else {
                FinishOrderedTask(task, std::move(completion));
            }
            task.finished->store(true, std::memory_order_release);
            completion_became_ready = completed_was_empty && !completed_.empty();
        }
        if (completion_became_ready) {
            NotifyCompletionReady();
        }
    }

    void FinishCanceledTask(const Task& task)
    {
        bool completion_became_ready = false;
        {
            std::lock_guard lock(mutex_);
            const bool completed_was_empty = completed_.empty();
            if (active_task_count_ > 0) {
                --active_task_count_;
            }
            if (task.batch) {
                FinishBatchTask(task, std::nullopt);
            } else {
                FinishOrderedTask(task, std::nullopt);
            }
            task.finished->store(true, std::memory_order_release);
            completion_became_ready = completed_was_empty && !completed_.empty();
        }
        if (completion_became_ready) {
            NotifyCompletionReady();
        }
    }

    void DrainRetirementForShutdown(
        std::deque<PreparedSourceCollection>& prepared,
        std::deque<BackgroundRetirementHandle>& resources)
    {
        for (SourceCollectionLoadCompletion& completion : completed_) {
            if (completion.prepared) {
                prepared.push_back(std::move(*completion.prepared));
            }
        }
        completed_.clear();
        prepared.insert(
            prepared.end(),
            std::make_move_iterator(retired_prepared_.begin()),
            std::make_move_iterator(retired_prepared_.end()));
        retired_prepared_.clear();
        resources.swap(retired_resources_);
        cancellation_.clear();
        active_task_count_ = 0;
    }

    void RunTask(const Task& task, std::stop_token stop_token)
    {
        if (task.request.navigation_attempt) {
            task.request.navigation_attempt->MarkWorkerStarted();
        }
        if (task.canceled->load(std::memory_order_relaxed)) {
            FinishCanceledTask(task);
            return;
        }

        SourceCollectionLoadCompletion completion;
        completion.task_id = task.id;
        completion.path = task.request.path;
        completion.spectrum_index = task.request.spectrum_index;
        completion.navigation_attempt = task.request.navigation_attempt;
        try {
            completion.prepared = Prepare(task, stop_token);
            if (task.request.navigation_attempt) {
                task.request.navigation_attempt->MarkWorkerPrepared();
            }
        } catch (const SourceLoadCanceled&) {
            FinishCanceledTask(task);
            return;
        } catch (const std::exception& error) {
            completion.error_message = error.what();
        } catch (...) {
            completion.error_message = "Unknown source loading failure.";
        }
        if (task.request.navigation_attempt) {
            task.request.navigation_attempt->MarkCompletionReady();
        }
        FinishTask(task, std::move(completion));
    }

    void RetirementLoop(std::stop_token stop_token)
    {
        for (;;) {
            std::deque<PreparedSourceCollection> retired_prepared;
            std::deque<BackgroundRetirementHandle> retired_resources;
            {
                std::unique_lock lock(mutex_);
                retirement_condition_.wait(lock, stop_token, [this]() {
                    return !retired_prepared_.empty() || !retired_resources_.empty();
                });
                if (stop_token.stop_requested()) {
                    DrainRetirementForShutdown(retired_prepared, retired_resources);
                } else {
                    retired_prepared.swap(retired_prepared_);
                    retired_resources.swap(retired_resources_);
                }
            }

            // Destruction happens here, outside the mutex and off the UI thread.
            retired_prepared.clear();
            retired_resources.clear();
            if (stop_token.stop_requested()) {
                return;
            }
        }
    }

    std::unique_ptr<DirectoryChangeGenerationMonitor>
        directory_change_generation_monitor_;
    SourceCollectionLoadDependencies dependencies_;
    mutable std::mutex mutex_;
    std::condition_variable_any retirement_condition_;
    std::mutex completion_callback_mutex_;
    std::condition_variable completion_callback_idle_;
    CompletionReadyCallback completion_ready_callback_;
    std::size_t completion_callbacks_in_flight_ = 0;
    std::deque<SourceCollectionLoadCompletion> completed_;
    std::deque<std::shared_ptr<BatchCompletionSlot>> ordered_completions_;
    std::deque<PreparedSourceCollection> retired_prepared_;
    std::deque<BackgroundRetirementHandle> retired_resources_;
    std::unordered_map<std::uint64_t, std::shared_ptr<std::atomic_bool>> cancellation_;
    std::uint64_t next_task_id_ = 1;
    std::size_t active_task_count_ = 0;
    std::vector<Worker> workers_;
    std::jthread retirement_worker_;
};

SourceCollectionLoadQueue::SourceCollectionLoadQueue()
    : SourceCollectionLoadQueue(DefaultDependencies())
{
}

SourceCollectionLoadQueue::SourceCollectionLoadQueue(
    SourceCollectionLoadDependencies dependencies)
{
    FillMissingDependencies(dependencies);
    impl_ = std::make_unique<Impl>(std::move(dependencies));
}

SourceCollectionLoadQueue::~SourceCollectionLoadQueue() = default;
SourceCollectionLoadQueue::SourceCollectionLoadQueue(SourceCollectionLoadQueue&&) noexcept = default;
SourceCollectionLoadQueue& SourceCollectionLoadQueue::operator=(
    SourceCollectionLoadQueue&&) noexcept = default;

std::uint64_t SourceCollectionLoadQueue::Enqueue(SourceCollectionLoadRequest request)
{
    return impl_->Enqueue(std::move(request));
}

std::vector<std::uint64_t> SourceCollectionLoadQueue::EnqueueBatch(
    std::vector<SourceCollectionLoadRequest> requests)
{
    return impl_->EnqueueBatch(std::move(requests));
}

void SourceCollectionLoadQueue::Cancel(std::uint64_t task_id)
{
    impl_->Cancel(task_id);
}

std::vector<SourceCollectionLoadCompletion> SourceCollectionLoadQueue::TakeCompleted()
{
    return impl_->TakeCompleted();
}

bool SourceCollectionLoadQueue::NeedsService() const
{
    return impl_->NeedsService();
}

void SourceCollectionLoadQueue::RegisterCompletionReadyCallback(
    CompletionReadyCallback callback)
{
    impl_->RegisterCompletionReadyCallback(std::move(callback));
}

void SourceCollectionLoadQueue::UnregisterCompletionReadyCallback()
{
    impl_->UnregisterCompletionReadyCallback();
}

void SourceCollectionLoadQueue::RetirePrepared(PreparedSourceCollection prepared)
{
    impl_->RetirePrepared(std::move(prepared));
}

void SourceCollectionLoadQueue::RetireResource(BackgroundRetirementHandle resource)
{
    impl_->RetireResource(std::move(resource));
}

}  // namespace specforge
