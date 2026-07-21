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
          worker_([this](std::stop_token stop_token) { WorkerLoop(stop_token); })
    {
    }

    ~Impl()
    {
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    struct BatchCache {
        std::shared_ptr<const SampleWorkflowPreparationCacheBundle> value;
    };

    struct Task {
        std::uint64_t id = 0;
        SourceCollectionLoadRequest request;
        std::shared_ptr<std::atomic_bool> canceled;
        std::shared_ptr<BatchCache> batch_cache;
        SourceCollectionLoadPriority priority = SourceCollectionLoadPriority::Interactive;
    };

    std::uint64_t Enqueue(
        SourceCollectionLoadRequest request,
        SourceCollectionLoadPriority priority)
    {
        std::lock_guard lock(mutex_);
        const std::uint64_t id = EnqueueLocked(std::move(request), nullptr, priority);
        condition_.notify_one();
        return id;
    }

    std::vector<std::uint64_t> EnqueueBatch(std::vector<SourceCollectionLoadRequest> requests)
    {
        std::lock_guard lock(mutex_);
        std::vector<std::uint64_t> ids;
        ids.reserve(requests.size());
        if (requests.empty()) {
            return ids;
        }
        auto batch_cache = std::make_shared<BatchCache>();
        for (SourceCollectionLoadRequest& request : requests) {
            ids.push_back(EnqueueLocked(
                std::move(request),
                batch_cache,
                SourceCollectionLoadPriority::Restore));
        }
        condition_.notify_one();
        return ids;
    }

    void Cancel(std::uint64_t task_id)
    {
        std::lock_guard lock(mutex_);
        const auto match = cancellation_.find(task_id);
        if (match != cancellation_.end()) {
            match->second->store(true, std::memory_order_relaxed);
        }
        condition_.notify_one();
    }

    std::vector<SourceCollectionLoadCompletion> TakeCompleted()
    {
        std::lock_guard lock(mutex_);
        std::vector<SourceCollectionLoadCompletion> result;
        result.reserve(completed_.size());
        while (!completed_.empty()) {
            result.push_back(std::move(completed_.front()));
            completed_.pop_front();
        }
        return result;
    }

    bool NeedsService() const
    {
        std::lock_guard lock(mutex_);
        return active_task_count_ > 0 || !completed_.empty();
    }

    void RetirePrepared(PreparedSourceCollection prepared)
    {
        {
            std::lock_guard lock(mutex_);
            retired_prepared_.push_back(std::move(prepared));
        }
        condition_.notify_one();
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
        condition_.notify_one();
    }

private:
    std::uint64_t EnqueueLocked(
        SourceCollectionLoadRequest request,
        std::shared_ptr<BatchCache> batch_cache,
        SourceCollectionLoadPriority priority)
    {
        const std::uint64_t id = next_task_id_++;
        auto canceled = std::make_shared<std::atomic_bool>(false);
        Task task{id, std::move(request), canceled, std::move(batch_cache), priority};
        if (priority == SourceCollectionLoadPriority::Continuation) {
            pending_.push_front(std::move(task));
        } else if (priority == SourceCollectionLoadPriority::Interactive) {
            const auto first_restore = std::find_if(
                pending_.begin(),
                pending_.end(),
                [](const Task& queued) {
                    return queued.priority == SourceCollectionLoadPriority::Restore;
                });
            pending_.insert(first_restore, std::move(task));
        } else {
            pending_.push_back(std::move(task));
        }
        cancellation_.emplace(id, std::move(canceled));
        ++active_task_count_;
        return id;
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
        if (task.batch_cache && task.batch_cache->value) {
            return task.batch_cache->value;
        }
        auto cache = std::make_shared<SampleWorkflowPreparationCacheBundle>(
            dependencies_.workflow_cache_loader(
                dependencies_.workflow_cache_paths,
                checkpoint));
        if (task.batch_cache) {
            task.batch_cache->value = cache;
        }
        return cache;
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
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        if (CanReusePreparedWorkflow(context.identity, task.request.reuse_identity)) {
            SourceCollectionIdentity identity = context.identity;
            return PreparedSourceCollection{
                task.id,
                task.request.path,
                task.request.spectrum_index,
                std::move(snapshot),
                PreparedSourceCollectionReuse{std::move(identity)},
            };
        }
        PreparedSampleWorkflowState workflow =
            PrepareWorkflow(task, *snapshot, context, checkpoint);
        return PreparedSourceCollection{
            task.id,
            task.request.path,
            task.request.spectrum_index,
            std::move(snapshot),
            PreparedSourceCollectionPlan{std::move(context), std::move(workflow)},
        };
    }

    PreparedSourceCollection PrepareFolder(
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
            const SourceCollectionFolderListing listing =
                ScanSourceCollectionFolder(task.request.path, {}, checkpoint);
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
            SourceCollectionContext context = BuildFolderSourceCollectionContextCancelable(
                *snapshot,
                listing,
                checkpoint);
            FinalizeContext(task, context, checkpoint);
            const SourceCollectionFolderListing verified_listing =
                ScanSourceCollectionFolder(task.request.path, {}, checkpoint);
            const SourceCollectionSingleFileState verified_state =
                CaptureSourceCollectionSingleFileState(
                    task.request.path,
                    task.request.annotation_paths,
                    checkpoint);
            if (SourceCollectionFolderListingsMatch(listing, verified_listing, checkpoint) &&
                SourceCollectionSingleFileStatesMatch(initial_state, verified_state)) {
                return BuildPrepared(task, std::move(snapshot), std::move(context), checkpoint);
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
            SourceCollectionContext context = LoadSourceCollectionContextCancelable(
                *snapshot,
                initial_state,
                checkpoint);
            FinalizeContext(task, context, checkpoint);
            const SourceCollectionSingleFileState verified_state =
                CaptureSourceCollectionSingleFileState(
                    task.request.path,
                    task.request.annotation_paths,
                    checkpoint);
            if (SourceCollectionSingleFileStatesMatch(initial_state, verified_state)) {
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

    void FinishTask(const Task& task, SourceCollectionLoadCompletion completion)
    {
        std::lock_guard lock(mutex_);
        cancellation_.erase(task.id);
        if (active_task_count_ > 0) {
            --active_task_count_;
        }
        if (!task.canceled->load(std::memory_order_relaxed)) {
            completed_.push_back(std::move(completion));
        }
    }

    void FinishCanceledTask(const Task& task)
    {
        std::lock_guard lock(mutex_);
        cancellation_.erase(task.id);
        if (active_task_count_ > 0) {
            --active_task_count_;
        }
    }

    void DrainForShutdown(
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
        pending_.clear();
        cancellation_.clear();
        active_task_count_ = 0;
    }

    void WorkerLoop(std::stop_token stop_token)
    {
        for (;;) {
            std::optional<Task> task;
            std::deque<PreparedSourceCollection> retired_prepared;
            std::deque<BackgroundRetirementHandle> retired_resources;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stop_token, [this]() {
                    return !pending_.empty() || !retired_prepared_.empty() ||
                           !retired_resources_.empty();
                });
                if (stop_token.stop_requested()) {
                    DrainForShutdown(retired_prepared, retired_resources);
                } else {
                    retired_prepared.swap(retired_prepared_);
                    retired_resources.swap(retired_resources_);
                    if (!pending_.empty()) {
                        task = std::move(pending_.front());
                        pending_.pop_front();
                    }
                }
            }

            // Destruction happens here, outside the mutex and on the worker.
            retired_prepared.clear();
            retired_resources.clear();
            if (stop_token.stop_requested()) {
                return;
            }
            if (!task) {
                continue;
            }
            if (task->canceled->load(std::memory_order_relaxed)) {
                FinishCanceledTask(*task);
                continue;
            }

            SourceCollectionLoadCompletion completion;
            completion.task_id = task->id;
            completion.path = task->request.path;
            completion.spectrum_index = task->request.spectrum_index;
            try {
                completion.prepared = Prepare(*task, stop_token);
            } catch (const SourceLoadCanceled&) {
                FinishCanceledTask(*task);
                continue;
            } catch (const std::exception& error) {
                completion.error_message = error.what();
            }
            FinishTask(*task, std::move(completion));
        }
    }

    SourceCollectionLoadDependencies dependencies_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<Task> pending_;
    std::deque<SourceCollectionLoadCompletion> completed_;
    std::deque<PreparedSourceCollection> retired_prepared_;
    std::deque<BackgroundRetirementHandle> retired_resources_;
    std::unordered_map<std::uint64_t, std::shared_ptr<std::atomic_bool>> cancellation_;
    std::uint64_t next_task_id_ = 1;
    std::size_t active_task_count_ = 0;
    std::jthread worker_;
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

std::uint64_t SourceCollectionLoadQueue::Enqueue(
    SourceCollectionLoadRequest request,
    SourceCollectionLoadPriority priority)
{
    return impl_->Enqueue(std::move(request), priority);
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

void SourceCollectionLoadQueue::RetirePrepared(PreparedSourceCollection prepared)
{
    impl_->RetirePrepared(std::move(prepared));
}

void SourceCollectionLoadQueue::RetireResource(BackgroundRetirementHandle resource)
{
    impl_->RetireResource(std::move(resource));
}

}  // namespace specforge
