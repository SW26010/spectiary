#include "ui/source_collection_load_queue.h"

#include "profile/source_load_latency_trace.h"
#include "ui/source_collection_load_queue_internal.h"
#include "ui/source_collection_preparation_internal.h"

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

#include <windows.h>

namespace specforge {

class SourceCollectionLoadQueue::Impl {
public:
    explicit Impl(SourceCollectionLoadDependencies adapters,
        SourceCollectionLoadQueueExecutionOptions options)
        : preparation_(std::move(adapters)),
          foreground_limit_(options.foreground_limit == 0
              ? std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 4)
              : std::clamp<std::size_t>(options.foreground_limit, 1, 4)),
          before_worker_start_(std::move(options.before_worker_start)),
          retirement_worker_([this](std::stop_token stop_token) {
              RetirementLoop(stop_token);
          })
    {
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
        std::shared_ptr<std::stop_source> canceled;
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
        std::shared_ptr<std::stop_source> canceled;
        std::shared_ptr<BatchState> batch;
        std::size_t batch_index = 0;
        std::shared_ptr<BatchCompletionSlot> ordered_completion;
        bool prefetch = false;
        bool wait_at_runtime_resource_cancellation_checkpoint =
            false;
    };

    struct Worker {
        Worker(
            std::shared_ptr<std::atomic_bool> finished_flag,
            Impl* owner,
            Task task)
            : finished(std::move(finished_flag)),
              prefetch(task.prefetch),
              thread([owner,
                      task = std::move(task),
                      finished_flag = finished](std::stop_token stop_token) mutable {
                  if (task.prefetch) {
                      (void)::SetThreadPriority(
                          ::GetCurrentThread(),
                          THREAD_PRIORITY_BELOW_NORMAL);
                  }
                  owner->RunTaskLoop(std::move(task), stop_token, finished_flag);
              })
        {
        }

        Worker(Worker&&) noexcept = default;
        Worker& operator=(Worker&&) noexcept = default;
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;

        std::shared_ptr<std::atomic_bool> finished;
        bool prefetch = false;
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

    std::uint64_t EnqueuePrefetch(SourceCollectionLoadRequest request)
    {
        ReapFinishedWorkers();
        request.snapshot_only = true;
        std::uint64_t id = 0;
        bool completion_became_ready = false;
        {
            std::lock_guard lock(mutex_);
            if (active_prefetch_task_count_ > 0) {
                return 0;
            }
            const bool completed_was_empty = completed_.empty();
            id = StartTaskLocked(
                std::move(request),
                nullptr,
                0,
                nullptr,
                true);
            completion_became_ready =
                completed_was_empty && !completed_.empty();
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
                        match->second->request_stop();
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

    bool Cancel(std::uint64_t task_id)
    {
        bool found = false;
        {
            std::lock_guard lock(mutex_);
            ++cancellation_request_count_;
            const auto match = cancellation_.find(task_id);
            if (match == cancellation_.end()) {
                return false;
            }
            if (match->second->request_stop()) {
                ++successful_cancellation_count_;
            }
            found = true;
        }
        if (found) {
            runtime_resource_cancellation_condition_
                .notify_all();
        }
        return true;
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
        // Completion publication precedes the worker's finished flag. Keep
        // the activation service deadline alive until TakeCompleted reaps it,
        // even if the last completion has already been drained.
        return active_task_count_ > 0 || !completed_.empty() || !workers_.empty();
    }

    SourceCollectionLoadActivitySnapshot ActivitySnapshot() const
    {
        std::lock_guard lock(mutex_);
        return {
            .active_task_count = active_task_count_,
            .completed_count = completed_.size(),
            .worker_count = workers_.size(),
            .retirement_queued_count =
                retired_prepared_.size() + retired_resources_.size(),
            .retirement_in_flight_count =
                retirement_in_flight_count_,
            .cancellation_request_count =
                cancellation_request_count_,
            .successful_cancellation_count =
                successful_cancellation_count_,
            .retired_prepared_count = retired_prepared_count_,
            .retired_resource_count = retired_resource_count_,
            .runtime_resource_cancellation_checkpoint_waiting =
                runtime_resource_cancellation_checkpoint_waiting_,
        };
    }

    bool ArmRuntimeResourceCancellationCheckpoint()
    {
        std::lock_guard lock(mutex_);
        if (active_task_count_ != 0 ||
            !completed_.empty() ||
            runtime_resource_cancellation_checkpoint_armed_ ||
            runtime_resource_cancellation_checkpoint_waiting_) {
            return false;
        }
        runtime_resource_cancellation_checkpoint_armed_ =
            true;
        return true;
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
        std::shared_ptr<BatchCompletionSlot> ordered_completion = nullptr,
        bool prefetch = false)
    {
        const std::uint64_t id = next_task_id_++;
        if (request.latency_attempt) {
            request.latency_attempt->MarkSourceTaskId(id);
        }
        auto canceled = std::make_shared<std::stop_source>();
        auto finished = std::make_shared<std::atomic_bool>(false);
        const std::filesystem::path failure_path = request.path;
        const std::size_t failure_spectrum_index = request.spectrum_index;
        LoadLatencyAttemptHandle failure_latency_attempt =
            request.latency_attempt;
        std::shared_ptr<SourceLoadLatencyTrace>
            failure_source_load_trace =
                request.source_load_trace;
        std::shared_ptr<BatchState> failure_batch = batch;
        std::shared_ptr<BatchCompletionSlot> failure_ordered_completion = ordered_completion;
        const bool wait_at_runtime_resource_checkpoint =
            !prefetch &&
            runtime_resource_cancellation_checkpoint_armed_;
        if (wait_at_runtime_resource_checkpoint) {
            runtime_resource_cancellation_checkpoint_armed_ =
                false;
        }
        Task task{
            id,
            std::move(request),
            canceled,
            std::move(batch),
            batch_index,
            std::move(ordered_completion),
            prefetch,
            wait_at_runtime_resource_checkpoint,
        };
        cancellation_.emplace(id, canceled);
        ++active_task_count_;
        if (prefetch) {
            ++active_prefetch_task_count_;
        }
        try {
            const auto running = std::count_if(workers_.begin(), workers_.end(),
                [prefetch](const Worker& worker) {
                    return worker.prefetch == prefetch &&
                        !worker.finished->load(std::memory_order_acquire);
                });
            const std::size_t limit = prefetch ? 1 : foreground_limit_;
            if (static_cast<std::size_t>(running) < limit) {
                if (before_worker_start_) before_worker_start_();
                workers_.emplace_back(std::move(finished), this, std::move(task));
            } else {
                (prefetch ? pending_prefetch_ : pending_foreground_).push_back(std::move(task));
            }
        } catch (const std::system_error& error) {
            --active_task_count_;
            if (prefetch) {
                --active_prefetch_task_count_;
            }
            SourceCollectionLoadCompletion completion;
            completion.task_id = id;
            completion.path = failure_path;
            completion.spectrum_index = failure_spectrum_index;
            completion.error_message =
                std::string("Could not start the source loading thread: ") + error.what();
            completion.latency_attempt = std::move(failure_latency_attempt);
            completion.source_load_trace =
                std::move(failure_source_load_trace);
            if (completion.latency_attempt) {
                completion.latency_attempt->MarkCompletionReady();
            }
            if (failure_batch) {
                FinishBatchTask(
                    *failure_batch,
                    batch_index,
                    id,
                    canceled,
                    std::move(completion));
            } else if (failure_ordered_completion) {
                FinishOrderedTask(
                    *failure_ordered_completion,
                    id,
                    canceled,
                    std::move(completion));
            } else {
                FinishUnorderedTask(
                    id,
                    canceled,
                    std::move(completion));
            }
            return id;
        } catch (...) {
            cancellation_.erase(id);
            --active_task_count_;
            if (prefetch) {
                --active_prefetch_task_count_;
            }
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
                canceled->request_stop();
            }
            for (Worker& worker : workers_) {
                worker.thread.request_stop();
                workers.push_back(std::move(worker.thread));
            }
            workers_.clear();
        }
        runtime_resource_cancellation_condition_.notify_all();
    }

    SourceCollectionCancellationCheckpoint CheckpointFor(
        const Task& task,
        std::stop_token stop_token) const
    {
        return [canceled = task.canceled, stop_token]() {
            if (stop_token.stop_requested() || canceled->stop_requested()) {
                throw SourceCollectionPreparationCanceled();
            }
        };
    }

    std::shared_ptr<const SampleWorkflowPreparationCacheBundle> WorkflowCache(
        const Task& task,
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        if (!task.batch) {
            return preparation_.LoadWorkflowCache(checkpoint);
        }
        std::call_once(
            task.batch->load_once,
            [this, &task, &checkpoint]() {
                task.batch->value =
                    preparation_.LoadWorkflowCache(checkpoint);
            });
        checkpoint();
        return task.batch->value;
    }

    PreparedSourceCollection Prepare(
        const Task& task,
        std::stop_token stop_token)
    {
        const SourceCollectionCancellationCheckpoint checkpoint =
            CheckpointFor(task, stop_token);
        return preparation_.Prepare(
            task.id,
            task.request,
            checkpoint,
            [this, &task](
                const SourceCollectionCancellationCheckpoint&
                    cache_checkpoint) {
                return WorkflowCache(task, cache_checkpoint);
            },
            task.canceled->get_token());
    }

    void PublishCompletion(BatchCompletionSlot& slot)
    {
        const bool canceled =
            slot.canceled && slot.canceled->stop_requested();
        if (slot.completion) {
            if (!canceled) {
                if (slot.completion->latency_attempt) {
                    slot.completion->latency_attempt->MarkCompletionPublished();
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
        std::shared_ptr<std::stop_source> canceled,
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
        std::shared_ptr<std::stop_source> canceled,
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

    void FinishUnorderedTask(
        std::uint64_t task_id,
        const std::shared_ptr<std::stop_source>& canceled,
        std::optional<SourceCollectionLoadCompletion> completion)
    {
        const bool task_canceled =
            canceled && canceled->stop_requested();
        if (completion) {
            if (!task_canceled || completion->canceled) {
                if (completion->latency_attempt) {
                    completion->latency_attempt
                        ->MarkCompletionPublished();
                }
                completed_.push_back(std::move(*completion));
            } else if (completion->prepared) {
                retired_prepared_.push_back(
                    std::move(*completion->prepared));
                retirement_condition_.notify_one();
            }
        }
        cancellation_.erase(task_id);
    }

    void FinishUnorderedTask(
        const Task& task,
        std::optional<SourceCollectionLoadCompletion> completion)
    {
        const bool task_canceled =
            task.canceled &&
            task.canceled->stop_requested();
        if (task.prefetch && task_canceled &&
            (!completion || !completion->canceled)) {
            if (completion && completion->prepared) {
                retired_prepared_.push_back(
                    std::move(*completion->prepared));
                retirement_condition_.notify_one();
            }
            SourceCollectionLoadCompletion
                canceled_completion;
            canceled_completion.task_id = task.id;
            canceled_completion.path =
                task.request.path;
            canceled_completion.spectrum_index =
                task.request.spectrum_index;
            canceled_completion.canceled = true;
            canceled_completion.worker_terminal_at =
                LoadLatencyClock::now();
            completion =
                std::move(canceled_completion);
        }
        FinishUnorderedTask(
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
            if (task.prefetch && active_prefetch_task_count_ > 0) {
                --active_prefetch_task_count_;
            }
            if (task.batch) {
                FinishBatchTask(
                    task,
                    task.canceled->stop_requested()
                        ? std::nullopt
                        : std::optional<SourceCollectionLoadCompletion>{std::move(completion)});
            } else if (task.ordered_completion) {
                FinishOrderedTask(task, std::move(completion));
            } else {
                FinishUnorderedTask(
                    task,
                    std::move(completion));
            }
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
            if (task.prefetch && active_prefetch_task_count_ > 0) {
                --active_prefetch_task_count_;
            }
            if (task.batch) {
                FinishBatchTask(task, std::nullopt);
            } else if (task.ordered_completion) {
                FinishOrderedTask(task, std::nullopt);
            } else {
                FinishUnorderedTask(
                    task,
                    std::nullopt);
            }
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
        active_prefetch_task_count_ = 0;
    }

    void RunTaskLoop(Task task, std::stop_token stop_token,
        const std::shared_ptr<std::atomic_bool>& finished)
    {
        const bool prefetch = task.prefetch;
        for (;;) {
            RunTask(task, stop_token);
            std::optional<Task> next;
            {
                std::lock_guard lock(mutex_);
                auto& pending = prefetch ? pending_prefetch_ : pending_foreground_;
                if (pending.empty()) {
                    finished->store(true, std::memory_order_release);
                    return;
                }
                next.emplace(std::move(pending.front()));
                pending.pop_front();
            }
            // Request-owned graphs are released on this worker, outside the lock.
            task = std::move(*next);
        }
    }

    void RunTask(const Task& task, std::stop_token stop_token)
    {
        if (task.request.latency_attempt) {
            task.request.latency_attempt->MarkWorkerStarted();
        }
        if (task
                .wait_at_runtime_resource_cancellation_checkpoint) {
            {
                std::lock_guard lock(mutex_);
                runtime_resource_cancellation_checkpoint_waiting_ =
                    true;
            }
            NotifyCompletionReady();
            std::unique_lock lock(mutex_);
            (void)runtime_resource_cancellation_condition_.wait(
                lock,
                stop_token,
                [&task]() {
                    return task.canceled->stop_requested();
                });
            runtime_resource_cancellation_checkpoint_waiting_ =
                false;
        }
        if (task.canceled->stop_requested()) {
            FinishCanceledTask(task);
            return;
        }

        SourceCollectionLoadCompletion completion;
        completion.task_id = task.id;
        completion.path = task.request.path;
        completion.spectrum_index = task.request.spectrum_index;
        completion.latency_attempt = task.request.latency_attempt;
        completion.source_load_trace =
            task.request.source_load_trace;
        try {
            completion.prepared = Prepare(task, stop_token);
            if (completion.prepared) {
                if (task.request.latency_attempt) {
                    task.request.latency_attempt->SetTargetIndex(
                        completion.prepared->spectrum_index);
                }
                if (task.request.source_load_trace) {
                    task.request.source_load_trace->SetTargetIndex(
                        completion.prepared->spectrum_index);
                }
            }
            if (task.request.latency_attempt) {
                task.request.latency_attempt->MarkWorkerPrepared();
            }
        } catch (const SourceCollectionPreparationCanceled&) {
            FinishCanceledTask(task);
            return;
        } catch (const SourceCollectionPreparationStale& error) {
            completion.stale = true;
            completion.error_message = error.what();
        } catch (const std::exception& error) {
            completion.error_message = error.what();
        } catch (...) {
            completion.error_message = "Unknown source loading failure.";
        }
        if (task.request.latency_attempt) {
            task.request.latency_attempt->MarkCompletionReady();
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
                retirement_in_flight_count_ +=
                    retired_prepared.size() +
                    retired_resources.size();
            }

            // Destruction happens here, outside the mutex and off the UI thread.
            const std::size_t prepared_count =
                retired_prepared.size();
            const std::size_t resource_count =
                retired_resources.size();
            retired_prepared.clear();
            retired_resources.clear();
            {
                std::lock_guard lock(mutex_);
                retirement_in_flight_count_ -=
                    prepared_count + resource_count;
                retired_prepared_count_ += prepared_count;
                retired_resource_count_ += resource_count;
            }
            if (resource_count > 0) {
                NotifyCompletionReady();
            }
            if (stop_token.stop_requested()) {
                return;
            }
        }
    }

    SourceCollectionPreparation preparation_;
    const std::size_t foreground_limit_;
    std::function<void()> before_worker_start_;
    std::deque<Task> pending_foreground_;
    std::deque<Task> pending_prefetch_;
    mutable std::mutex mutex_;
    std::condition_variable_any retirement_condition_;
    std::condition_variable_any
        runtime_resource_cancellation_condition_;
    std::mutex completion_callback_mutex_;
    std::condition_variable completion_callback_idle_;
    CompletionReadyCallback completion_ready_callback_;
    std::size_t completion_callbacks_in_flight_ = 0;
    std::deque<SourceCollectionLoadCompletion> completed_;
    std::deque<std::shared_ptr<BatchCompletionSlot>> ordered_completions_;
    std::deque<PreparedSourceCollection> retired_prepared_;
    std::deque<BackgroundRetirementHandle> retired_resources_;
    std::unordered_map<std::uint64_t, std::shared_ptr<std::stop_source>> cancellation_;
    std::uint64_t next_task_id_ = 1;
    std::size_t active_task_count_ = 0;
    std::size_t active_prefetch_task_count_ = 0;
    std::size_t retirement_in_flight_count_ = 0;
    std::uint64_t cancellation_request_count_ = 0;
    std::uint64_t successful_cancellation_count_ = 0;
    std::uint64_t retired_prepared_count_ = 0;
    std::uint64_t retired_resource_count_ = 0;
    bool runtime_resource_cancellation_checkpoint_armed_ =
        false;
    bool runtime_resource_cancellation_checkpoint_waiting_ =
        false;
    std::vector<Worker> workers_;
    std::jthread retirement_worker_;
};

SourceCollectionLoadQueue::SourceCollectionLoadQueue()
    : SourceCollectionLoadQueue(SourceCollectionLoadDependencies{}, {})
{
}

SourceCollectionLoadQueue::SourceCollectionLoadQueue(
    SampleWorkflowPreparationPaths workflow_cache_paths)
    : SourceCollectionLoadQueue(
          [&workflow_cache_paths]() {
              SourceCollectionLoadDependencies adapters;
              adapters.workflow_cache_paths =
                  std::move(workflow_cache_paths);
              return adapters;
          }(), {})
{
}

SourceCollectionLoadQueue::SourceCollectionLoadQueue(
    SourceCollectionLoadDependencies adapters,
    SourceCollectionLoadQueueExecutionOptions options)
    : impl_(std::make_unique<Impl>(std::move(adapters), std::move(options)))
{
}

SourceCollectionLoadQueue::~SourceCollectionLoadQueue() = default;
SourceCollectionLoadQueue::SourceCollectionLoadQueue(SourceCollectionLoadQueue&&) noexcept = default;
SourceCollectionLoadQueue& SourceCollectionLoadQueue::operator=(
    SourceCollectionLoadQueue&&) noexcept = default;

std::uint64_t SourceCollectionLoadQueue::Enqueue(SourceCollectionLoadRequest request)
{
    return impl_->Enqueue(std::move(request));
}

std::uint64_t SourceCollectionLoadQueue::EnqueuePrefetch(
    SourceCollectionLoadRequest request)
{
    return impl_->EnqueuePrefetch(std::move(request));
}

std::vector<std::uint64_t> SourceCollectionLoadQueue::EnqueueBatch(
    std::vector<SourceCollectionLoadRequest> requests)
{
    return impl_->EnqueueBatch(std::move(requests));
}

bool SourceCollectionLoadQueue::Cancel(std::uint64_t task_id)
{
    return impl_->Cancel(task_id);
}

std::vector<SourceCollectionLoadCompletion> SourceCollectionLoadQueue::TakeCompleted()
{
    return impl_->TakeCompleted();
}

bool SourceCollectionLoadQueue::NeedsService() const
{
    return impl_->NeedsService();
}

SourceCollectionLoadActivitySnapshot
SourceCollectionLoadQueue::ActivitySnapshot() const
{
    return impl_->ActivitySnapshot();
}

bool SourceCollectionLoadQueue::
    ArmRuntimeResourceCancellationCheckpoint()
{
    return impl_->
        ArmRuntimeResourceCancellationCheckpoint();
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
