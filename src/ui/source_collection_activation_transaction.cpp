#include "ui/source_collection_activation_transaction.h"

#include "domain/source_path_identity.h"

#include <chrono>
#include <stdexcept>
#include <utility>
#include <variant>

namespace specforge {
namespace {

std::int64_t ElapsedNavigationResolutionNanoseconds(
    NavigationLatencyTimePoint started_at)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               NavigationLatencyTrace::Now() - started_at)
        .count();
}

std::int64_t NavigationSteadyNanoseconds(
    NavigationLatencyTimePoint at)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               at.time_since_epoch())
        .count();
}

}  // namespace

SourceCollectionActivationTransaction::
    SourceCollectionActivationTransaction(
        SourceCollectionSession& session,
        SourceCollectionLoadQueue load_queue)
    : session_(session),
      load_queue_(std::move(load_queue))
{
}

SourceCollectionActivationTransaction::
    ~SourceCollectionActivationTransaction() = default;

bool SourceCollectionActivationTransaction::OpenSource(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    CancelSnapshotPrefetch();
    SourceLoadLatencyTraceHandle source_load_trace =
        StartSourceLoadTrace(
            spectrum_index,
            NavigationLatencyTrace::Now());
    BeginActivationIntent(true);
    const bool session_changed =
        session_.CancelActivePendingSampleNavigation();
    if (deferred_restore_active_) {
        deferred_restore_active_path_ = path;
    }
    (void)QueueSourceLoad(
        path,
        spectrum_index,
        session_.AnnotationPathsForSource(path),
        Purpose::ExplicitOpen,
        {},
        std::move(source_load_trace));
    return session_changed;
}

SourceCollectionSessionResult
SourceCollectionActivationTransaction::Submit(
    SourceCollectionSessionIntent intent,
    std::optional<NavigationIntent> navigation)
{
    const std::optional<SampleNavigationDirection>
        prefetch_direction = navigation
        ? PrefetchDirectionForInputKind(navigation->kind)
        : std::nullopt;
    CancelSnapshotPrefetch();
    const bool trace_requested =
        latency_tracing_enabled_ && navigation.has_value();
    const NavigationLatencyTimePoint requested_at =
        trace_requested ? NavigationLatencyTrace::Now()
                        : NavigationLatencyTimePoint{};
    NavigationTargetResolutionReport target_resolution;
    std::optional<std::size_t> from_index;
    if (trace_requested) {
        from_index = session_.EffectiveSampleNavigationIndex();
        target_resolution.effective_index_ns =
            ElapsedNavigationResolutionNanoseconds(requested_at);
    }

    const NavigationLatencyTimePoint
        pending_activation_started_at =
            trace_requested ? NavigationLatencyTrace::Now()
                            : NavigationLatencyTimePoint{};
    const bool supersedes_source_activation =
        session_.SupersedesPendingSourceActivation(intent);
    if (supersedes_source_activation) {
        BeginActivationIntent(false);
    }
    if (trace_requested) {
        target_resolution.pending_activation_supersede_ns +=
            ElapsedNavigationResolutionNanoseconds(
                pending_activation_started_at);
    }

    SourceCollectionSessionResult result = session_.Submit(
        std::move(intent),
        trace_requested ? &target_resolution : nullptr);
    const NavigationLatencyTimePoint target_resolved_at =
        trace_requested ? NavigationLatencyTrace::Now()
                        : NavigationLatencyTimePoint{};
    NavigationLatencyTraceHandle navigation_trace =
        StartNavigationTrace(
            result,
            from_index,
            requested_at,
            target_resolved_at,
            std::move(target_resolution),
            std::move(navigation));
    if (deferred_restore_active_ &&
        supersedes_source_activation) {
        const SpectrumSnapshotHandle snapshot =
            session_.CurrentSourceSnapshot();
        if (snapshot && !snapshot->source.path.empty()) {
            deferred_restore_active_path_ =
                snapshot->source.path;
        }
    }
    QueueSessionFollowUp(
        result,
        false,
        std::move(navigation_trace),
        prefetch_direction);
    RetireSessionResources(result);
    return result;
}

void SourceCollectionActivationTransaction::BeginDeferredRestore()
{
    std::optional<SourceCollectionDeferredRestorePlan> plan =
        session_.TakeDeferredRestorePlan();
    if (!plan || plan->sources.empty()) {
        return;
    }

    deferred_restore_active_ = true;
    if (plan->active_source_index &&
        *plan->active_source_index < plan->sources.size()) {
        deferred_restore_active_path_ =
            plan->sources[*plan->active_source_index].path;
    }

    std::vector<SourceCollectionLoadRequest> requests;
    std::vector<Ticket> tickets;
    requests.reserve(plan->sources.size());
    tickets.reserve(plan->sources.size());
    for (SourceCollectionSavedSource& source : plan->sources) {
        tickets.push_back(ReserveLoad(
            source.path,
            source.last_spectrum_index,
            Purpose::DeferredRestore));
        requests.push_back(SourceCollectionLoadRequest{
            .path = std::move(source.path),
            .spectrum_index = source.last_spectrum_index,
            .annotation_paths =
                std::move(source.annotation_paths),
        });
    }
    const std::vector<std::uint64_t> task_ids =
        load_queue_.EnqueueBatch(std::move(requests));
    for (std::size_t index = 0; index < task_ids.size();
         ++index) {
        RegisterLoad(
            task_ids[index],
            std::move(tickets[index]));
    }
}

SourceCollectionActivationTransaction::ServiceResult
SourceCollectionActivationTransaction::Drain(
    bool allow_snapshot_prefetch,
    BeforeSourceActivation before_source_activation,
    ConsumeSessionAction consume_session_action)
{
    ServiceResult result;
    DrainCompletions(
        load_queue_.TakeCompleted(),
        before_source_activation,
        consume_session_action,
        result);
    ServiceSnapshotPrefetch(allow_snapshot_prefetch);
    return result;
}

void SourceCollectionActivationTransaction::
    SetPresentationContext(
        bool latency_tracing_enabled,
        std::uint64_t frame_index)
{
    current_frame_index_ = frame_index;
    spectrum_draw_submission_.reset();
    latency_tracing_enabled_ = latency_tracing_enabled;
    if (latency_tracing_enabled_) {
        return;
    }
    presentable_navigation_trace_.reset();
    presentable_navigation_snapshot_.reset();
    navigation_traces_.clear();
    presentable_source_load_trace_.reset();
    presentable_source_load_snapshot_.reset();
    source_load_traces_.clear();
}

void SourceCollectionActivationTransaction::
    RecordSpectrumDrawSubmission(
        std::uint64_t frame_index,
        unsigned int viewport_id,
        SpectrumSnapshotHandle snapshot)
{
    spectrum_draw_submission_ = SpectrumDrawSubmission{
        frame_index,
        viewport_id,
        std::move(snapshot),
    };
    SupersedePresentableNavigationIfSnapshotChanged(
        spectrum_draw_submission_->snapshot);
    SupersedePresentableSourceLoadIfSnapshotChanged(
        spectrum_draw_submission_->snapshot);
}

std::vector<NavigationLatencyReport>
SourceCollectionActivationTransaction::
    CompleteFramePresentations(
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation>
            presentations)
{
    std::vector<NavigationLatencyReport> reports;
    SupersedePresentableNavigationIfSnapshotChanged(
        session_.CurrentSampleSnapshot());
    for (auto trace = navigation_traces_.begin();
         trace != navigation_traces_.end();) {
        const bool matching_draw_submission =
            trace->second == presentable_navigation_trace_ &&
            spectrum_draw_submission_ &&
            spectrum_draw_submission_->frame_index ==
                frame_index &&
            spectrum_draw_submission_->snapshot ==
                presentable_navigation_snapshot_;
        if (matching_draw_submission) {
            for (const NavigationLatencyPresentation&
                     presentation : presentations) {
                if (presentation.viewport_id !=
                    spectrum_draw_submission_->viewport_id) {
                    continue;
                }
                (void)trace->second->MarkPresentedForViewport(
                    frame_index,
                    presentation.viewport_id,
                    presentation.completed_at);
                break;
            }
        }
        std::optional<NavigationLatencyReport> report =
            trace->second->TerminalReport();
        if (!report) {
            ++trace;
            continue;
        }
        reports.push_back(std::move(*report));
        if (trace->second ==
            presentable_navigation_trace_) {
            presentable_navigation_trace_.reset();
            presentable_navigation_snapshot_.reset();
        }
        trace = navigation_traces_.erase(trace);
    }
    return reports;
}

std::vector<SourceLoadLatencyReport>
SourceCollectionActivationTransaction::
    CompleteSourceLoadFramePresentations(
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation>
            presentations)
{
    std::vector<SourceLoadLatencyReport> reports;
    SupersedePresentableSourceLoadIfSnapshotChanged(
        session_.CurrentSampleSnapshot());
    for (auto trace = source_load_traces_.begin();
         trace != source_load_traces_.end();) {
        const bool matching_draw_submission =
            trace->second == presentable_source_load_trace_ &&
            spectrum_draw_submission_ &&
            spectrum_draw_submission_->frame_index ==
                frame_index &&
            spectrum_draw_submission_->snapshot ==
                presentable_source_load_snapshot_;
        if (matching_draw_submission) {
            for (const NavigationLatencyPresentation&
                     presentation : presentations) {
                if (presentation.viewport_id !=
                    spectrum_draw_submission_->viewport_id) {
                    continue;
                }
                (void)trace->second->MarkPresentedForViewport(
                    frame_index,
                    presentation.viewport_id,
                    presentation.completed_at);
                break;
            }
        }
        std::optional<SourceLoadLatencyReport> report =
            trace->second->TerminalReport();
        if (!report) {
            ++trace;
            continue;
        }
        reports.push_back(std::move(*report));
        if (trace->second ==
            presentable_source_load_trace_) {
            presentable_source_load_trace_.reset();
            presentable_source_load_snapshot_.reset();
        }
        trace = source_load_traces_.erase(trace);
    }
    return reports;
}

std::vector<NavigationPrefetchReport>
SourceCollectionActivationTransaction::
    TakeNavigationPrefetchReports()
{
    return std::exchange(
        navigation_prefetch_reports_,
        {});
}

void SourceCollectionActivationTransaction::
    RegisterCompletionReadyCallback(
        SourceCollectionLoadQueue::CompletionReadyCallback
            callback)
{
    load_queue_.RegisterCompletionReadyCallback(
        std::move(callback));
}

void SourceCollectionActivationTransaction::
    UnregisterCompletionReadyCallback()
{
    load_queue_.UnregisterCompletionReadyCallback();
}

bool SourceCollectionActivationTransaction::NeedsService() const
{
    return load_queue_.NeedsService();
}

bool SourceCollectionActivationTransaction::HasPendingLoads() const
{
    return !pending_loads_.empty();
}

std::size_t SourceCollectionActivationTransaction::
    PendingLoadCount() const
{
    return pending_loads_.size();
}

bool SourceCollectionActivationTransaction::PrefetchActive() const
{
    return active_snapshot_prefetch_.has_value();
}

std::string_view
SourceCollectionActivationTransaction::ErrorMessage() const
{
    return error_message_;
}

void SourceCollectionActivationTransaction::RetireResource(
    BackgroundRetirementHandle resource)
{
    load_queue_.RetireResource(std::move(resource));
}

std::uint64_t
SourceCollectionActivationTransaction::QueueSourceLoad(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    std::vector<std::filesystem::path> annotation_paths,
    Purpose purpose,
    NavigationLatencyTraceHandle navigation_trace,
    SourceLoadLatencyTraceHandle source_load_trace,
    std::optional<SampleNavigationDirection>
        prefetch_direction)
{
    CancelSnapshotPrefetch();
    std::optional<SourceCollectionLoadHint> hint =
        session_.LoadHintForSource(path, spectrum_index);
    NavigationLatencyAttemptHandle latency_attempt;
    if (navigation_trace) {
        navigation_trace->SetTargetIndex(spectrum_index);
        latency_attempt =
            navigation_trace->BeginLoadAttempt(
                spectrum_index);
    } else if (source_load_trace) {
        source_load_trace->SetTargetIndex(spectrum_index);
        latency_attempt =
            source_load_trace->BeginLoadAttempt(
                spectrum_index);
    }

    Ticket ticket = ReserveLoad(
        path,
        spectrum_index,
        purpose,
        std::move(navigation_trace),
        std::move(source_load_trace),
        prefetch_direction);
    const std::uint64_t task_id = load_queue_.Enqueue({
        .path = path,
        .spectrum_index = spectrum_index,
        .annotation_paths = std::move(annotation_paths),
        .reuse_identity =
            hint ? std::optional<SourceCollectionIdentity>{
                       hint->identity}
                 : std::nullopt,
        .context_reuse_proof =
            hint ? hint->context_reuse_proof
                 : std::optional<
                       SourceCollectionContextReuseProof>{},
        .base_live_workflow_revision =
            hint ? std::optional<std::uint64_t>{
                       hint->live_workflow_revision}
                 : std::nullopt,
        .folder_listing_generation_hint =
            hint ? hint->folder_listing_generation_hint
                 : SourceCollectionFolderListingGenerationHandle{},
        .resident_snapshot =
            hint ? std::move(hint->resident_snapshot)
                 : std::optional<
                       SourceCollectionResidentSnapshot>{},
        .latency_attempt = std::move(latency_attempt),
    });
    CancelPendingTasks(
        RegisterOrReplaceLoad(task_id, std::move(ticket)));
    error_message_.clear();
    return task_id;
}

void SourceCollectionActivationTransaction::
    BeginActivationIntent(
        bool preserve_pending_explicit_opens)
{
    CancelSnapshotPrefetch();
    CancelPendingTasks(
        AdvanceIntent(preserve_pending_explicit_opens));
}

void SourceCollectionActivationTransaction::CancelPendingTasks(
    std::vector<PendingTask> pending_tasks)
{
    for (const PendingTask& pending : pending_tasks) {
        MarkTicketSuperseded(pending.ticket);
        load_queue_.Cancel(pending.task_id);
    }
}

void SourceCollectionActivationTransaction::
    QueueSessionFollowUp(
        const SourceCollectionSessionResult& result,
        bool deferred_restore,
        NavigationLatencyTraceHandle navigation_trace,
        std::optional<SampleNavigationDirection>
            prefetch_direction)
{
    CancelSourceFollowUps(result);
    if (!result.follow_up_spectrum_index) {
        return;
    }
    const SpectrumSnapshotHandle snapshot =
        session_.CurrentSourceSnapshot();
    if (!snapshot || snapshot->source.path.empty()) {
        return;
    }
    if (HasMatchingFollowUp(
            snapshot->source.path,
            *result.follow_up_spectrum_index)) {
        if (navigation_trace) {
            (void)navigation_trace->MarkTerminal(
                NavigationLatencyOutcome::Coalesced);
        }
        return;
    }
    (void)QueueSourceLoad(
        snapshot->source.path,
        *result.follow_up_spectrum_index,
        session_.AnnotationPathsForSource(
            snapshot->source.path),
        deferred_restore ? Purpose::DeferredRestore
                         : Purpose::SessionFollowUp,
        std::move(navigation_trace),
        {},
        prefetch_direction);
}

void SourceCollectionActivationTransaction::
    CancelSourceFollowUps(
        const SourceCollectionSessionResult& result)
{
    if (!result.canceled_source_follow_up_path) {
        return;
    }
    CancelPendingTasks(CancelNonExplicitFollowUps(
        *result.canceled_source_follow_up_path));
}

void SourceCollectionActivationTransaction::
    RetireSessionResources(
        SourceCollectionSessionResult& result)
{
    for (BackgroundRetirementHandle& resource :
         result.background_retirement) {
        load_queue_.RetireResource(std::move(resource));
    }
    result.background_retirement.clear();
}

void SourceCollectionActivationTransaction::DrainCompletions(
    std::vector<SourceCollectionLoadCompletion> completions,
    const BeforeSourceActivation& before_source_activation,
    const ConsumeSessionAction& consume_session_action,
    ServiceResult& service_result)
{
    for (SourceCollectionLoadCompletion& completion :
         completions) {
        if (active_snapshot_prefetch_ &&
            completion.task_id ==
                active_snapshot_prefetch_->task_id) {
            DrainSnapshotPrefetchCompletion(
                std::move(completion));
            continue;
        }

        std::optional<CompletionAdmission> admission =
            TakeCompletion(
                completion.task_id,
                completion.path,
                completion.spectrum_index);
        if (!admission || !admission->accepted) {
            if (admission) {
                MarkTicketSuperseded(admission->ticket);
            }
            if (completion.prepared) {
                load_queue_.RetirePrepared(
                    std::move(*completion.prepared));
            }
            continue;
        }

        Ticket ticket = std::move(admission->ticket);
        if (completion.latency_attempt) {
            completion.latency_attempt->
                MarkCompletionDrained();
        }
        if (!completion.prepared) {
            error_message_ =
                completion.error_message.empty()
                ? "Background source loading failed."
                : std::move(completion.error_message);
            if (CancelFailedPendingSampleNavigation(
                    session_,
                    ticket)) {
                service_result.session_changed = true;
            }
            if (ticket.navigation_trace) {
                (void)ticket.navigation_trace->MarkTerminal(
                    NavigationLatencyOutcome::Failed);
            }
            if (ticket.source_load_trace) {
                (void)ticket.source_load_trace->MarkTerminal(
                    SourceLoadLatencyOutcome::Failed);
            }
            continue;
        }

        PreparedSourceCollection prepared =
            std::move(*completion.prepared);
        if (ticket.navigation_trace) {
            NavigationSnapshotCacheKind cache_kind =
                NavigationSnapshotCacheKind::None;
            if (prepared.snapshot_cache_origin ==
                SourceCollectionResidentSnapshotOrigin::
                    History) {
                cache_kind =
                    NavigationSnapshotCacheKind::History;
            } else if (
                prepared.snapshot_cache_origin ==
                SourceCollectionResidentSnapshotOrigin::
                    Prefetch) {
                cache_kind =
                    NavigationSnapshotCacheKind::Prefetch;
            }
            ticket.navigation_trace->SetCacheKind(
                cache_kind);
        }
        if (before_source_activation) {
            for (BackgroundRetirementHandle& resource :
                 before_source_activation()) {
                load_queue_.RetireResource(
                    std::move(resource));
            }
        }

        SourceCollectionSessionResult result =
            session_.OpenPreparedSource(
                prepared.path,
                prepared.spectrum_index,
                std::move(prepared.snapshot),
                std::move(prepared.payload),
                std::move(
                    prepared.folder_listing_generation),
                std::move(
                    prepared.context_reuse_proof));
        service_result.session_changed = true;
        MergeSourceCollectionSessionAction(
            service_result.action,
            result.action);

        const bool completes_navigation_trace =
            result.loaded &&
            !result.follow_up_spectrum_index &&
            ticket.navigation_trace;
        const bool completes_source_load_trace =
            result.loaded &&
            !result.follow_up_spectrum_index &&
            ticket.source_load_trace;
        if (completes_navigation_trace) {
            if (presentable_navigation_trace_ &&
                presentable_navigation_trace_ !=
                    ticket.navigation_trace) {
                (void)presentable_navigation_trace_->
                    MarkTerminal(
                        NavigationLatencyOutcome::
                            Superseded);
            }
            presentable_navigation_trace_ =
                ticket.navigation_trace;
            presentable_navigation_snapshot_ =
                session_.CurrentSampleSnapshot();
            ticket.navigation_trace->MarkSnapshotActivated(
                current_frame_index_);
        }
        if (completes_source_load_trace) {
            if (presentable_source_load_trace_ &&
                presentable_source_load_trace_ !=
                    ticket.source_load_trace) {
                (void)presentable_source_load_trace_->
                    MarkTerminal(
                        SourceLoadLatencyOutcome::
                            Superseded);
            }
            presentable_source_load_trace_ =
                ticket.source_load_trace;
            presentable_source_load_snapshot_ =
                session_.CurrentSampleSnapshot();
            ticket.source_load_trace->MarkSnapshotActivated(
                current_frame_index_);
        }
        CancelSourceFollowUps(result);
        if (consume_session_action) {
            consume_session_action(result.action);
        }
        if (completes_navigation_trace) {
            ticket.navigation_trace->MarkUiUpdated();
        }
        if (completes_source_load_trace) {
            ticket.source_load_trace->MarkUiUpdated();
        }
        RetireSessionResources(result);

        const bool starts_activation_intent =
            result.loaded &&
            ticket.purpose == Purpose::ExplicitOpen;
        if (!result.loaded) {
            error_message_ = result.message.empty()
                ? "The prepared source result was no longer applicable."
                : std::move(result.message);
            if (CancelFailedPendingSampleNavigation(
                    session_,
                    ticket)) {
                service_result.session_changed = true;
            }
            if (ticket.navigation_trace) {
                (void)ticket.navigation_trace->MarkTerminal(
                    NavigationLatencyOutcome::Rejected);
            }
            if (ticket.source_load_trace) {
                (void)ticket.source_load_trace->MarkTerminal(
                    SourceLoadLatencyOutcome::Rejected);
            }
        } else {
            error_message_.clear();
            if (starts_activation_intent) {
                BeginActivationIntent(true);
            }
        }

        if (result.loaded &&
            !result.follow_up_spectrum_index &&
            prepared.snapshot_cache_origin ==
                SourceCollectionResidentSnapshotOrigin::
                    Prefetch &&
            prepared.snapshot_prefetch_id != 0 &&
            prepared.snapshot_prefetch_task_id != 0 &&
            prepared.snapshot_prefetch_scheduled_ns > 0) {
            RecordSnapshotPrefetchOutcome(
                prepared.snapshot_prefetch_id,
                prepared.snapshot_prefetch_task_id,
                prepared.spectrum_index,
                prepared.snapshot_prefetch_direction,
                NavigationPrefetchOutcome::Consumed,
                NavigationLatencyTimePoint{
                    std::chrono::nanoseconds{
                        prepared
                            .snapshot_prefetch_scheduled_ns}});
        }
        if (result.follow_up_spectrum_index) {
            if (ticket.navigation_trace) {
                ticket.navigation_trace->SetTargetIndex(
                    *result.follow_up_spectrum_index);
            }
            if (ticket.source_load_trace) {
                ticket.source_load_trace->SetTargetIndex(
                    *result.follow_up_spectrum_index);
            }
            (void)QueueSourceLoad(
                ticket.path,
                *result.follow_up_spectrum_index,
                session_.AnnotationPathsForSource(
                    ticket.path),
                ticket.purpose == Purpose::DeferredRestore
                    ? Purpose::DeferredRestore
                    : Purpose::SessionFollowUp,
                ticket.navigation_trace,
                ticket.source_load_trace,
                ticket.prefetch_direction);
        } else if (
            result.loaded &&
            ticket.prefetch_direction) {
            ScheduleSnapshotPrefetch(
                *ticket.prefetch_direction);
        }
        RestoreDeferredActiveSourceIfAvailable(
            consume_session_action,
            service_result);
    }
    FinishDeferredRestoreIfReady(
        consume_session_action,
        service_result);
}

void SourceCollectionActivationTransaction::
    ScheduleSnapshotPrefetch(
        SampleNavigationDirection direction)
{
    pending_snapshot_prefetch_direction_ = direction;
}

void SourceCollectionActivationTransaction::
    ServiceSnapshotPrefetch(bool allow_snapshot_prefetch)
{
    if (!pending_snapshot_prefetch_direction_ ||
        active_snapshot_prefetch_ ||
        HasPendingLoads() ||
        deferred_restore_active_ ||
        !allow_snapshot_prefetch) {
        return;
    }

    const SampleNavigationDirection direction =
        *pending_snapshot_prefetch_direction_;
    pending_snapshot_prefetch_direction_.reset();
    std::optional<SourceCollectionSnapshotPrefetchPlan>
        plan = session_.PlanSnapshotPrefetch(
            direction,
            snapshot_prefetch_policy_);
    if (!plan) {
        return;
    }

    const NavigationLatencyTimePoint scheduled_at =
        NavigationLatencyTrace::Now();
    const std::uint64_t prefetch_id =
        next_snapshot_prefetch_id_++;
    const std::string path_key =
        SourcePathIdentityKey(plan->path);
    const std::uint64_t expected_generation =
        GenerationForPath(plan->path).value_or(0);
    SourceCollectionLoadHint hint =
        std::move(plan->load_hint);
    const std::uint64_t task_id =
        load_queue_.EnqueuePrefetch({
            .path = plan->path,
            .spectrum_index = plan->spectrum_index,
            .annotation_paths =
                std::move(plan->annotation_paths),
            .reuse_identity = hint.identity,
            .context_reuse_proof =
                std::move(hint.context_reuse_proof),
            .base_live_workflow_revision =
                hint.live_workflow_revision,
            .folder_listing_generation_hint =
                std::move(
                    hint.folder_listing_generation_hint),
            .snapshot_only = true,
        });
    if (task_id == 0) {
        pending_snapshot_prefetch_direction_ =
            direction;
        return;
    }
    active_snapshot_prefetch_ =
        PendingSnapshotPrefetch{
            .prefetch_id = prefetch_id,
            .task_id = task_id,
            .path = std::move(plan->path),
            .path_key = path_key,
            .spectrum_index = plan->spectrum_index,
            .generation = expected_generation,
            .activation_epoch = ActivationEpoch(),
            .direction = direction,
            .scheduled_at = scheduled_at,
        };
}

void SourceCollectionActivationTransaction::
    CancelSnapshotPrefetch()
{
    pending_snapshot_prefetch_direction_.reset();
    if (!active_snapshot_prefetch_ ||
        active_snapshot_prefetch_->invalidated) {
        return;
    }
    if (load_queue_.Cancel(
            active_snapshot_prefetch_->task_id)) {
        active_snapshot_prefetch_->
            cancel_requested_at =
                NavigationLatencyTrace::Now();
        active_snapshot_prefetch_->invalidated = true;
        return;
    }
    active_snapshot_prefetch_->invalidated = true;
}

void SourceCollectionActivationTransaction::
    DrainSnapshotPrefetchCompletion(
        SourceCollectionLoadCompletion completion)
{
    PendingSnapshotPrefetch ticket =
        std::move(*active_snapshot_prefetch_);
    active_snapshot_prefetch_.reset();
    if (completion.canceled) {
        RecordSnapshotPrefetchOutcome(
            ticket.prefetch_id,
            ticket.task_id,
            ticket.spectrum_index,
            ticket.direction,
            NavigationPrefetchOutcome::Canceled,
            ticket.scheduled_at,
            completion.worker_terminal_at,
            ticket.cancel_requested_at);
        return;
    }

    const std::optional<std::uint64_t> generation =
        GenerationForPath(ticket.path);
    const bool current =
        !ticket.invalidated &&
        ticket.activation_epoch == ActivationEpoch() &&
        generation &&
        *generation == ticket.generation &&
        SourcePathIdentityKey(completion.path) ==
            ticket.path_key &&
        completion.spectrum_index ==
            ticket.spectrum_index;
    if (!current) {
        if (completion.prepared) {
            load_queue_.RetirePrepared(
                std::move(*completion.prepared));
        }
        RecordSnapshotPrefetchOutcome(
            ticket.prefetch_id,
            ticket.task_id,
            ticket.spectrum_index,
            ticket.direction,
            NavigationPrefetchOutcome::Stale,
            ticket.scheduled_at);
        return;
    }

    if (!completion.prepared) {
        RecordSnapshotPrefetchOutcome(
            ticket.prefetch_id,
            ticket.task_id,
            ticket.spectrum_index,
            ticket.direction,
            completion.stale
                ? NavigationPrefetchOutcome::Stale
                : NavigationPrefetchOutcome::Failed,
            ticket.scheduled_at);
        return;
    }

    PreparedSourceCollection prepared =
        std::move(*completion.prepared);
    if (!std::holds_alternative<
            PreparedSourceCollectionReuse>(
            prepared.payload) ||
        !prepared.context_reuse_proof) {
        load_queue_.RetirePrepared(
            std::move(prepared));
        RecordSnapshotPrefetchOutcome(
            ticket.prefetch_id,
            ticket.task_id,
            ticket.spectrum_index,
            ticket.direction,
            NavigationPrefetchOutcome::Stale,
            ticket.scheduled_at);
        return;
    }

    SourceCollectionSnapshotPrefetchStoreResult stored =
        session_.StorePrefetchedSnapshot(
            prepared.path,
            SourceCollectionResidentSnapshot{
                prepared.spectrum_index,
                std::move(prepared.snapshot),
                std::move(
                    *prepared.context_reuse_proof),
                std::move(
                    prepared
                        .folder_listing_generation),
                SourceCollectionResidentSnapshotOrigin::
                    Prefetch,
                ticket.prefetch_id,
                ticket.task_id,
                NavigationSteadyNanoseconds(
                    ticket.scheduled_at),
                ticket.direction,
            });
    for (BackgroundRetirementHandle& resource :
         stored.background_retirement) {
        load_queue_.RetireResource(
            std::move(resource));
    }
    RecordSnapshotPrefetchOutcome(
        ticket.prefetch_id,
        ticket.task_id,
        ticket.spectrum_index,
        ticket.direction,
        stored.stored
            ? NavigationPrefetchOutcome::Completed
            : NavigationPrefetchOutcome::Stale,
        ticket.scheduled_at);
}

void SourceCollectionActivationTransaction::
    RecordSnapshotPrefetchOutcome(
        std::uint64_t prefetch_id,
        std::uint64_t source_task_id,
        std::size_t target_index,
        SampleNavigationDirection direction,
        NavigationPrefetchOutcome outcome,
        NavigationLatencyTimePoint scheduled_at,
        NavigationLatencyTimePoint terminal_at,
        NavigationLatencyTimePoint cancel_requested_at)
{
    if (terminal_at == NavigationLatencyTimePoint{}) {
        terminal_at = NavigationLatencyTrace::Now();
    }
    navigation_prefetch_reports_.push_back({
        .prefetch_id = prefetch_id,
        .source_task_id = source_task_id,
        .target_index = target_index,
        .direction = direction,
        .outcome = outcome,
        .scheduled_at = scheduled_at,
        .cancel_requested_at = cancel_requested_at,
        .terminal_at = terminal_at,
    });
}

std::optional<SampleNavigationDirection>
SourceCollectionActivationTransaction::
    PrefetchDirectionForInputKind(
        NavigationLatencyInputKind kind)
{
    switch (kind) {
    case NavigationLatencyInputKind::KeyboardPrevious:
    case NavigationLatencyInputKind::UiPrevious:
        return SampleNavigationDirection::Previous;
    case NavigationLatencyInputKind::KeyboardNext:
    case NavigationLatencyInputKind::UiNext:
    case NavigationLatencyInputKind::AutoAdvance:
        return SampleNavigationDirection::Next;
    }
    return std::nullopt;
}

void SourceCollectionActivationTransaction::
    RestoreDeferredActiveSourceIfAvailable(
        const ConsumeSessionAction& consume_session_action,
        ServiceResult& service_result)
{
    if (!deferred_restore_active_ ||
        !deferred_restore_active_path_) {
        return;
    }
    const std::string active_path_key =
        SourcePathIdentityKey(
            *deferred_restore_active_path_);
    const SourceCollectionSessionView view = session_.View();
    for (std::size_t index = 0;
         index < view.sources.size();
         ++index) {
        if (SourcePathIdentityKey(view.sources[index].path) !=
            active_path_key) {
            continue;
        }
        if (view.current_source_index &&
            *view.current_source_index == index) {
            return;
        }
        SourceCollectionSessionResult result =
            session_.Submit(
                SourceCollectionSessionIntent::
                    EditSourceCollection(
                        SourceCollectionIntent::
                            SwitchActive(index)));
        MergeSourceCollectionSessionAction(
            service_result.action,
            result.action);
        service_result.session_changed = true;
        RetireSessionResources(result);
        if (consume_session_action) {
            consume_session_action(result.action);
        }
        QueueSessionFollowUp(result, true);
        return;
    }
}

void SourceCollectionActivationTransaction::
    FinishDeferredRestoreIfReady(
        const ConsumeSessionAction& consume_session_action,
        ServiceResult& service_result)
{
    if (!deferred_restore_active_ ||
        HasPendingDeferredRestore()) {
        return;
    }
    RestoreDeferredActiveSourceIfAvailable(
        consume_session_action,
        service_result);
    session_.FinishDeferredRestore();
    deferred_restore_active_ = false;
    deferred_restore_active_path_.reset();
}

NavigationLatencyTraceHandle
SourceCollectionActivationTransaction::
    StartNavigationTrace(
        const SourceCollectionSessionResult& result,
        std::optional<std::size_t> from_index,
        NavigationLatencyTimePoint requested_at,
        NavigationLatencyTimePoint target_resolved_at,
        NavigationTargetResolutionReport target_resolution,
        std::optional<NavigationIntent> navigation)
{
    if (!latency_tracing_enabled_ || !navigation ||
        !from_index ||
        !result.follow_up_spectrum_index) {
        return {};
    }
    const bool keyboard_origin =
        navigation->kind ==
            NavigationLatencyInputKind::KeyboardPrevious ||
        navigation->kind ==
            NavigationLatencyInputKind::KeyboardNext;
    if (keyboard_origin && !navigation->input_at) {
        return {};
    }
    const std::uint64_t navigation_id =
        next_navigation_trace_id_++;
    const NavigationLatencyTimePoint input_at =
        navigation->input_at.value_or(requested_at);
    auto trace =
        std::make_shared<NavigationLatencyTrace>(
            navigation_id,
            *from_index,
            *result.follow_up_spectrum_index,
            navigation->kind,
            input_at,
            requested_at,
            target_resolved_at,
            std::move(target_resolution));
    navigation_traces_.emplace(navigation_id, trace);
    return trace;
}

SourceLoadLatencyTraceHandle
SourceCollectionActivationTransaction::
    StartSourceLoadTrace(
        std::size_t target_index,
        NavigationLatencyTimePoint accepted_at)
{
    if (!latency_tracing_enabled_) {
        return {};
    }
    const std::uint64_t source_load_id =
        next_source_load_trace_id_++;
    auto trace =
        std::make_shared<SourceLoadLatencyTrace>(
            source_load_id,
            target_index,
            SourceLoadLatencyRequestKind::ExplicitOpen,
            accepted_at);
    source_load_traces_.emplace(
        source_load_id,
        trace);
    return trace;
}

void SourceCollectionActivationTransaction::
    SupersedePresentableNavigationIfSnapshotChanged(
        const SpectrumSnapshotHandle& current_snapshot)
{
    if (!presentable_navigation_trace_ ||
        presentable_navigation_snapshot_ ==
            current_snapshot) {
        return;
    }
    (void)presentable_navigation_trace_->MarkTerminal(
        NavigationLatencyOutcome::Superseded);
    presentable_navigation_trace_.reset();
    presentable_navigation_snapshot_.reset();
}

void SourceCollectionActivationTransaction::
    SupersedePresentableSourceLoadIfSnapshotChanged(
        const SpectrumSnapshotHandle& current_snapshot)
{
    if (!presentable_source_load_trace_ ||
        presentable_source_load_snapshot_ ==
            current_snapshot) {
        return;
    }
    (void)presentable_source_load_trace_->MarkTerminal(
        SourceLoadLatencyOutcome::Superseded);
    presentable_source_load_trace_.reset();
    presentable_source_load_snapshot_.reset();
}

bool SourceCollectionActivationTransaction::
    CancelFailedPendingSampleNavigation(
        SourceCollectionSession& session,
        const Ticket& ticket)
{
    if (ticket.purpose == Purpose::ExplicitOpen) {
        return false;
    }
    return session.CancelPendingSampleNavigation(
        ticket.path,
        ticket.spectrum_index);
}

SourceCollectionActivationTransaction::Ticket
SourceCollectionActivationTransaction::ReserveLoad(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    Purpose purpose,
    NavigationLatencyTraceHandle navigation_trace,
    SourceLoadLatencyTraceHandle source_load_trace,
    std::optional<SampleNavigationDirection>
        prefetch_direction)
{
    const std::string path_key =
        SourcePathIdentityKey(path);
    return Ticket{
        .path = path,
        .path_key = path_key,
        .spectrum_index = spectrum_index,
        .generation = ++generations_[path_key],
        .activation_epoch = activation_epoch_,
        .purpose = purpose,
        .navigation_trace =
            std::move(navigation_trace),
        .source_load_trace =
            std::move(source_load_trace),
        .prefetch_direction = prefetch_direction,
    };
}

void SourceCollectionActivationTransaction::RegisterLoad(
    std::uint64_t task_id,
    Ticket ticket)
{
    const Purpose purpose = ticket.purpose;
    const auto [pending, inserted] =
        pending_loads_.emplace(
            task_id,
            std::move(ticket));
    if (!inserted) {
        throw std::logic_error(
            "source activation task ID was already registered");
    }
    if (purpose == Purpose::DeferredRestore) {
        deferred_restore_task_ids_.insert(pending->first);
    }
}

std::vector<
    SourceCollectionActivationTransaction::PendingTask>
SourceCollectionActivationTransaction::
    RegisterOrReplaceLoad(
        std::uint64_t task_id,
        Ticket ticket)
{
    std::vector<PendingTask> replaced =
        RemoveLoadsForPath(ticket.path_key, false);
    RegisterLoad(task_id, std::move(ticket));
    return replaced;
}

std::vector<
    SourceCollectionActivationTransaction::PendingTask>
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

std::optional<
    SourceCollectionActivationTransaction::
        CompletionAdmission>
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

std::vector<
    SourceCollectionActivationTransaction::PendingTask>
SourceCollectionActivationTransaction::
    CancelNonExplicitFollowUps(
        const std::filesystem::path& path)
{
    return RemoveLoadsForPath(
        SourcePathIdentityKey(path),
        true);
}

bool SourceCollectionActivationTransaction::
    HasMatchingFollowUp(
        const std::filesystem::path& path,
        std::size_t spectrum_index) const
{
    const std::string path_key =
        SourcePathIdentityKey(path);
    for (const auto& [task_id, ticket] :
         pending_loads_) {
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
    const auto generation = generations_.find(
        SourcePathIdentityKey(path));
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

bool SourceCollectionActivationTransaction::
    HasPendingDeferredRestore() const
{
    return !deferred_restore_task_ids_.empty();
}

std::vector<
    SourceCollectionActivationTransaction::PendingTask>
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

void SourceCollectionActivationTransaction::
    EraseDeferredRestoreTask(std::uint64_t task_id)
{
    deferred_restore_task_ids_.erase(task_id);
}

void SourceCollectionActivationTransaction::
    MarkTicketSuperseded(const Ticket& ticket)
{
    if (ticket.navigation_trace) {
        (void)ticket.navigation_trace->MarkTerminal(
            NavigationLatencyOutcome::Superseded);
    }
    if (ticket.source_load_trace) {
        (void)ticket.source_load_trace->MarkTerminal(
            SourceLoadLatencyOutcome::Superseded);
    }
}

}  // namespace specforge
