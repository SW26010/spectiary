#include "ui/sample_navigation_controller.h"

#include "domain/sample_labeling_source_compatibility.h"
#include "domain/source_path_identity.h"
#include "platform/win32_text.h"
#include "profile/navigation_latency_trace.h"
#include "ui/sample_workflow_preparation.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace spectiary {
namespace {

using namespace std::chrono_literals;
using TargetResolutionClock = std::chrono::steady_clock;

constexpr auto kStateCacheSaveDebounce = 500ms;
constexpr auto kStateCacheSaveRetry = 2s;

SampleLabelingSourceCompatibility ReadOnlyAnnotationCompatibilityView(
    const SampleLabelingCanonicalSourceDescriptor& source)
{
    SampleLabelingSourceCompatibility compatibility =
        SampleLabelingCompatibilityView(source);
    if (compatibility.source_kind == "unknown") {
        compatibility.source_kind = {};
    }
    return compatibility;
}

std::int64_t ElapsedNanoseconds(TargetResolutionClock::time_point started_at)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               TargetResolutionClock::now() - started_at)
        .count();
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

bool PathsReferToSameFile(const std::filesystem::path& left, const std::filesystem::path& right)
{
    if (left.empty() || right.empty()) {
        return false;
    }
    return SourcePathIdentityKey(left) == SourcePathIdentityKey(right);
}

bool UnicodeOrdinalCaseEqual(
    std::string_view left,
    std::string_view right)
{
    if (left.empty() || right.empty()) {
        return left.empty() && right.empty();
    }
    const std::wstring wide_left =
        Utf8ToWide(left);
    const std::wstring wide_right =
        Utf8ToWide(right);
    if (wide_left.empty() ||
        wide_right.empty() ||
        wide_left.size() >
            static_cast<std::size_t>(
                (std::numeric_limits<int>::max)()) ||
        wide_right.size() >
            static_cast<std::size_t>(
                (std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(
               wide_left.data(),
               static_cast<int>(
                   wide_left.size()),
               wide_right.data(),
               static_cast<int>(
                   wide_right.size()),
               TRUE) == CSTR_EQUAL;
}

std::vector<std::filesystem::path> AnnotationPaths(const SourceCollectionManifest& manifest)
{
    std::vector<std::filesystem::path> paths;
    paths.reserve(manifest.annotations.size());
    std::unordered_set<std::string> path_keys;
    path_keys.reserve(manifest.annotations.size());
    for (const SampleAnnotationResult& annotation : manifest.annotations) {
        if (annotation.path.empty()) {
            continue;
        }
        std::string path_key = SourcePathIdentityKey(annotation.path);
        if (path_key.empty() || !path_keys.insert(std::move(path_key)).second) {
            continue;
        }
        paths.push_back(annotation.path);
    }
    return paths;
}

std::optional<std::size_t> ResolveNavigationTarget(
    const SampleNavigationRequest& request,
    const SampleNavigationSequence& sequence,
    const SampleNavigationSequenceProjection& projection,
    const SourceCollectionManifest& manifest,
    std::size_t spectrum_count,
    bool filter_active,
    bool& blocked_by_filter)
{
    switch (request.kind) {
    case SampleNavigationRequestKind::Previous:
        return projection.previous_target;
    case SampleNavigationRequestKind::Next:
        return projection.next_target;
    case SampleNavigationRequestKind::LabelAdvance:
        return sequence.LabelAdvanceTarget(projection, request.eligible_samples);
    case SampleNavigationRequestKind::LocateRow: {
        const std::optional<std::size_t> target = sequence.LocateSourceRow(request.row_index);
        if (!target && request.row_index < spectrum_count && !sequence.row_location_available) {
            blocked_by_filter = filter_active;
        }
        return target;
    }
    case SampleNavigationRequestKind::LocateSequencePosition:
        return sequence.LocateSequencePosition(
            request.sequence_position);
    case SampleNavigationRequestKind::LocateSourceRowInSequence: {
        const std::optional<std::size_t> target = sequence.LocateSourceRowInSequence(request.row_index);
        if (!target && request.row_index < spectrum_count && sequence.active) {
            blocked_by_filter = filter_active;
        }
        return target;
    }
    case SampleNavigationRequestKind::LocateSampleName:
        return sequence.LocateSampleName(manifest.sample_names, request.sample_name);
    case SampleNavigationRequestKind::LocateSampleNameMatch:
        return sequence.LocateSampleNameMatch(
            manifest.sample_names,
            request.row_index,
            request.sample_name);
    case SampleNavigationRequestKind::RestoreLabelUndoPosition:
        return request.row_index < spectrum_count
            ? std::optional<std::size_t>{request.row_index}
            : std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

SampleNavigationRequest SampleNavigationRequest::Previous()
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::Previous;
    return request;
}

SampleNavigationRequest SampleNavigationRequest::Next()
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::Next;
    return request;
}

SampleNavigationRequest SampleNavigationRequest::LabelAdvance()
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::LabelAdvance;
    return request;
}

SampleNavigationRequest SampleNavigationRequest::LabelAdvanceToEligible(std::vector<bool> eligible_samples)
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::LabelAdvance;
    request.eligible_samples = std::move(eligible_samples);
    return request;
}

SampleNavigationRequest SampleNavigationRequest::LocateRow(std::size_t row_index)
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::LocateRow;
    request.row_index = row_index;
    return request;
}

SampleNavigationRequest SampleNavigationRequest::LocateSequencePosition(
    std::size_t sequence_position)
{
    SampleNavigationRequest request;
    request.kind =
        SampleNavigationRequestKind::LocateSequencePosition;
    request.sequence_position = sequence_position;
    return request;
}

SampleNavigationRequest SampleNavigationRequest::LocateSourceRowInSequence(std::size_t row_index)
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::LocateSourceRowInSequence;
    request.row_index = row_index;
    return request;
}

SampleNavigationRequest SampleNavigationRequest::LocateSampleName(std::string sample_name)
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::LocateSampleName;
    request.sample_name = std::move(sample_name);
    return request;
}

SampleNavigationRequest SampleNavigationRequest::LocateSampleNameMatch(
    std::size_t row_index,
    std::string sample_name)
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::LocateSampleNameMatch;
    request.row_index = row_index;
    request.sample_name = std::move(sample_name);
    return request;
}

SampleNavigationRequest SampleNavigationRequest::RestoreLabelUndoPosition(std::size_t row_index)
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::RestoreLabelUndoPosition;
    request.row_index = row_index;
    return request;
}

SampleNavigationController::SampleNavigationController()
    : SampleNavigationController(std::filesystem::path{})
{
}

SampleNavigationController::SampleNavigationController(std::filesystem::path state_cache_path)
    : state_cache_path_(std::move(state_cache_path)),
      state_cache_persistence_(kStateCacheSaveDebounce, kStateCacheSaveRetry)
{
}

void SampleNavigationController::ActivateSource(std::string source_key, const SpectrumSnapshotHandle& snapshot)
{
    if (source_key.empty() || !snapshot) {
        ClearActiveSource();
        return;
    }

    SourceCollectionContext context = LoadSourceCollectionContext(*snapshot);
    ActivateSource(
        std::move(source_key),
        snapshot,
        context.identity,
        std::move(context.manifest));
}

void SampleNavigationController::ActivateSource(
    std::string source_key,
    const SpectrumSnapshotHandle& snapshot,
    const SourceCollectionIdentity& identity,
    SourceCollectionManifest manifest,
    std::optional<std::size_t> prepared_index)
{
    if (source_key.empty() || !snapshot) {
        ClearActiveSource();
        return;
    }

    EnsureStateCacheLoaded();
    SourceSession& session = sessions_[identity.id];
    const bool new_session = session.source_collection_identity.empty();
    const bool active_source_changed =
        !active_source_key_ || *active_source_key_ != identity.id;
    const bool context_changed =
        new_session ||
        session.source_fingerprint != identity.source_fingerprint ||
        session.context_fingerprint != identity.context_fingerprint ||
        session.spectrum_count != identity.spectrum_count;
    const std::string previous_query = std::move(session.sample_name_query);
    const std::optional<std::size_t> previous_index = session.current_index;

    session.canonical_source =
        BuildSampleLabelingCanonicalSourceDescriptor(
            *snapshot,
            identity,
            manifest);

    session.source_collection_identity = identity.id;
    session.source_name = identity.source_name;
    session.source_fingerprint = identity.source_fingerprint;
    session.context_fingerprint = identity.context_fingerprint;
    session.spectrum_count = identity.spectrum_count;
    if (context_changed) {
        std::vector<std::filesystem::path> retained_annotation_paths;
        for (const SampleAnnotationResult& annotation : session.manifest.annotations) {
            if (annotation.path.empty() ||
                std::any_of(
                    retained_annotation_paths.begin(),
                    retained_annotation_paths.end(),
                    [&annotation](const std::filesystem::path& existing) {
                        return PathsReferToSameFile(existing, annotation.path);
                    })) {
                continue;
            }
            retained_annotation_paths.push_back(annotation.path);
        }
        session.manifest = std::move(manifest);
        for (const std::filesystem::path& annotation_path : retained_annotation_paths) {
            if (std::any_of(
                    session.manifest.annotations.begin(),
                    session.manifest.annotations.end(),
                    [&annotation_path](const SampleAnnotationResult& existing) {
                        return PathsReferToSameFile(existing.path, annotation_path);
                    })) {
                continue;
            }
            (void)LoadReadOnlyAnnotationIntoSession(session, annotation_path);
        }
    }
    session.sample_name_query = previous_query;
    if (session.spectrum_count > 0) {
        const std::optional<std::size_t> persisted =
            CachedIndex(identity.id);
        if (prepared_index && *prepared_index < session.spectrum_count) {
            session.current_index = *prepared_index;
        } else if (new_session && persisted &&
                   *persisted < session.spectrum_count) {
            session.current_index = *persisted;
        } else if (new_session) {
            session.current_index = std::min(snapshot->collection.current_index, session.spectrum_count - 1);
        } else if (previous_index) {
            session.current_index = std::min(*previous_index, session.spectrum_count - 1);
        } else {
            session.current_index = std::min(snapshot->collection.current_index, session.spectrum_count - 1);
        }
    } else {
        session.current_index.reset();
    }
    if (session.filter_active && session.filter_included_samples.size() != session.spectrum_count) {
        session.filter_active = false;
        session.filter_included_samples.clear();
        session.filtered_sample_count = 0;
        session.index_before_active_filter.reset();
    }
    if (active_source_changed || context_changed) {
        InvalidateSequenceState(session);
    }
    (void)ReconcileCurrentWithSequence(session);
    RecomputeMatches(session);

    source_key_to_session_key_[source_key] = identity.id;
    active_source_key_ = identity.id;
    if (active_source_changed || context_changed) {
        ++active_context_generation_;
        RefreshSequenceTopologyRevision();
    }
    PersistActiveIndex();
}

BackgroundRetirementHandle SampleNavigationController::ActivatePreparedSource(
    std::string source_key,
    const SpectrumSnapshotHandle& snapshot,
    const SourceCollectionIdentity& identity,
    SourceCollectionManifest manifest,
    PreparedSampleWorkflowState prepared)
{
    if (source_key.empty() || !snapshot) {
        ClearActiveSource();
        return {};
    }

    auto [session_entry, inserted] = sessions_.try_emplace(identity.id);
    BackgroundRetirementHandle retired_session;
    if (!inserted) {
        auto retired = std::make_shared<SourceSession>();
        *retired = std::move(session_entry->second);
        retired_session = std::move(retired);
        session_entry->second = SourceSession{};
    }
    SourceSession& session = session_entry->second;
    session.canonical_source =
        BuildSampleLabelingCanonicalSourceDescriptor(
            *snapshot,
            identity,
            manifest);
    session.source_collection_identity = identity.id;
    session.source_name = identity.source_name;
    session.source_fingerprint = identity.source_fingerprint;
    session.context_fingerprint = identity.context_fingerprint;
    session.spectrum_count = identity.spectrum_count;
    session.current_index = prepared.current_index;
    session.manifest = std::move(manifest);
    // Sample-name search is transient UI state. A prepared load never performs
    // an implicit collection scan to preserve an old query.
    session.sample_name_query.clear();
    session.sample_name_matches = prepared.navigation_sequence.sample_name_matches;
    session.filter_active = prepared.filter_evaluation.active;
    session.filter_included_samples = std::move(prepared.filter_evaluation.included_samples);
    session.filtered_sample_count = prepared.filter_evaluation.active
        ? prepared.filter_evaluation.included_count
        : identity.spectrum_count;
    session.index_before_active_filter = prepared.index_before_active_filter;
    session.sort_choice = prepared.sort_choice ? std::move(*prepared.sort_choice) : SampleNavigationSortChoice{};
    session.sequence_state_cache = std::move(prepared.navigation_sequence);
    session.sequence_state_cache_valid = true;

    source_key_to_session_key_[source_key] = identity.id;
    active_source_key_ = identity.id;
    ++active_context_generation_;
    RefreshSequenceTopologyRevision();
    return retired_session;
}

BackgroundRetirementHandle
SampleNavigationController::AdoptPreparedStateCache(
    std::shared_ptr<const SampleNavigationStateCacheLoadResult>
        cache_snapshot)
{
    if (!cache_snapshot ||
        cache_snapshot == state_cache_snapshot_) {
        return {};
    }
    const bool first_load = !state_cache_loaded_;
    std::shared_ptr<const SampleNavigationStateCacheLoadResult>
        retired = std::exchange(
            state_cache_snapshot_,
            std::move(cache_snapshot));
    state_cache_loaded_ = true;
    if (first_load) {
        state_cache_persistence_.SetLoadWarning(
            state_cache_snapshot_->warning);
    }
    return retired;
}

BackgroundRetirementHandle
SampleNavigationController::ReleaseBackgroundResourcesForShutdown()
{
    return std::move(state_cache_snapshot_);
}

std::optional<SourceCollectionIdentity> SampleNavigationController::ActivateKnownSource(
    std::string_view source_key)
{
    const std::optional<SourceCollectionIdentity> identity = KnownSourceIdentity(source_key);
    if (!identity) {
        return std::nullopt;
    }
    const auto mapped = source_key_to_session_key_.find(std::string(source_key));
    if (!active_source_key_ || *active_source_key_ != mapped->second) {
        ++active_context_generation_;
        active_source_key_ = mapped->second;
        RefreshSequenceTopologyRevision();
        InvalidateSequenceState(
            sessions_.at(mapped->second));
    } else {
        active_source_key_ = mapped->second;
    }
    PersistActiveIndex();
    return identity;
}

std::optional<SourceCollectionIdentity> SampleNavigationController::KnownSourceIdentity(
    std::string_view source_key) const
{
    const auto mapped = source_key_to_session_key_.find(std::string(source_key));
    if (mapped == source_key_to_session_key_.end()) {
        return std::nullopt;
    }
    const auto session = sessions_.find(mapped->second);
    if (session == sessions_.end()) {
        return std::nullopt;
    }

    return SourceCollectionIdentity{
        .id = session->second.source_collection_identity,
        .source_name = session->second.source_name,
        .source_fingerprint = session->second.source_fingerprint,
        .context_fingerprint = session->second.context_fingerprint,
        .spectrum_count = session->second.spectrum_count,
    };
}

std::optional<std::size_t> SampleNavigationController::KnownSourceCurrentIndex(
    std::string_view source_key) const
{
    const auto mapped = source_key_to_session_key_.find(std::string(source_key));
    if (mapped == source_key_to_session_key_.end()) {
        return std::nullopt;
    }
    const auto session = sessions_.find(mapped->second);
    return session == sessions_.end() ? std::nullopt : session->second.current_index;
}

std::optional<SourceCollectionIdentity> SampleNavigationController::active_source_identity() const
{
    if (!active_source_key_) {
        return std::nullopt;
    }
    const auto session = sessions_.find(*active_source_key_);
    if (session == sessions_.end()) {
        return std::nullopt;
    }
    return SourceCollectionIdentity{
        .id = session->second.source_collection_identity,
        .source_name = session->second.source_name,
        .source_fingerprint = session->second.source_fingerprint,
        .context_fingerprint = session->second.context_fingerprint,
        .spectrum_count = session->second.spectrum_count,
    };
}

std::optional<SampleLabelingSourceCompatibility>
SampleNavigationController::active_source_compatibility() const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return std::nullopt;
    }
    return ReadOnlyAnnotationCompatibilityView(
        session->canonical_source);
}

BackgroundRetirementHandle SampleNavigationController::RemoveSource(std::string_view source_key)
{
    const std::string external_key(source_key);
    std::string session_key = external_key;
    const auto mapped = source_key_to_session_key_.find(external_key);
    if (mapped != source_key_to_session_key_.end()) {
        session_key = mapped->second;
        source_key_to_session_key_.erase(mapped);
    }

    BackgroundRetirementHandle retired;
    auto removed = sessions_.extract(session_key);
    if (!removed.empty()) {
        retired = MakeBackgroundRetirementHandle(std::move(removed.mapped()));
    }
    if (active_source_key_ && *active_source_key_ == session_key) {
        active_source_key_.reset();
        ++active_context_generation_;
        RefreshSequenceTopologyRevision();
    }
    return retired;
}

void SampleNavigationController::ClearActiveSource()
{
    if (active_source_key_) {
        ++active_context_generation_;
        active_source_key_.reset();
        RefreshSequenceTopologyRevision();
        return;
    }
    active_source_key_.reset();
}

bool SampleNavigationController::AddReadOnlyAnnotationToActiveSource(
    const std::filesystem::path& path,
    std::string* message)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr || session->spectrum_count == 0) {
        if (message != nullptr) {
            *message = "No active source can accept sample annotations.";
        }
        return false;
    }

    const bool loaded = LoadReadOnlyAnnotationIntoSession(*session, path, message);
    if (loaded) {
        ++active_context_generation_;
    }
    return loaded;
}

bool SampleNavigationController::
    UpsertAttachedAnnotationForActiveSource(
        SampleAnnotationResult annotation,
        bool* attachment_added)
{
    if (attachment_added != nullptr) {
        *attachment_added = false;
    }
    SourceSession* session = ActiveSession();
    if (session == nullptr ||
        annotation.path.empty() ||
        annotation.values.size() !=
            session->spectrum_count) {
        return false;
    }
    const auto existing = std::find_if(
        session->manifest.annotations.begin(),
        session->manifest.annotations.end(),
        [&annotation](const SampleAnnotationResult& candidate) {
            return PathsReferToSameFile(
                candidate.path,
                annotation.path);
        });
    if (existing ==
        session->manifest.annotations.end()) {
        session->manifest.annotations.push_back(
            std::move(annotation));
        if (attachment_added != nullptr) {
            *attachment_added = true;
        }
    } else {
        *existing = std::move(annotation);
    }
    ++active_context_generation_;
    return true;
}

bool SampleNavigationController::RemoveReadOnlyAnnotationFromActiveSource(const std::filesystem::path& path)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr || path.empty()) {
        return false;
    }

    const auto previous_size = session->manifest.annotations.size();
    session->manifest.annotations.erase(
        std::remove_if(
            session->manifest.annotations.begin(),
            session->manifest.annotations.end(),
            [&path](const SampleAnnotationResult& annotation) {
                return PathsReferToSameFile(annotation.path, path);
            }),
        session->manifest.annotations.end());
    if (session->manifest.annotations.size() == previous_size) {
        return false;
    }

    ++active_context_generation_;
    return true;
}

bool SampleNavigationController::
RemoveAnnotationAttachmentDiagnosticsFromActiveSource(
    const std::filesystem::path& path)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr || path.empty()) {
        return false;
    }

    const auto previous_size =
        session->manifest.diagnostics.size();
    session->manifest.diagnostics.erase(
        std::remove_if(
            session->manifest.diagnostics.begin(),
            session->manifest.diagnostics.end(),
            [&path](
                const SourceCollectionManifestDiagnostic&
                    diagnostic) {
                return diagnostic.kind ==
                        SourceCollectionManifestDiagnosticKind::
                            AnnotationIgnored &&
                    PathsReferToSameFile(
                        diagnostic.path,
                        path);
            }),
        session->manifest.diagnostics.end());
    if (session->manifest.diagnostics.size() ==
        previous_size) {
        return false;
    }

    ++active_context_generation_;
    return true;
}

bool SampleNavigationController::RestoreReadOnlyAnnotationsForActiveSource(
    const std::vector<std::filesystem::path>& paths)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr || session->spectrum_count == 0) {
        return false;
    }

    bool restored = false;
    for (const std::filesystem::path& path : paths) {
        if (path.empty() ||
            std::any_of(
                session->manifest.annotations.begin(),
                session->manifest.annotations.end(),
                [&path](const SampleAnnotationResult& existing) {
                    return PathsReferToSameFile(existing.path, path);
                })) {
            continue;
        }
        restored = LoadReadOnlyAnnotationIntoSession(*session, path) || restored;
    }
    if (restored) {
        ++active_context_generation_;
    }
    return restored;
}

SampleNavigationResult SampleNavigationController::Navigate(const SampleNavigationRequest& request)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return {};
    }

    SampleNavigationResult result;
    result.has_active_source = true;
    result.previous_index = session->current_index.value_or(0);
    result.current_index = session->current_index.value_or(0);
    const SampleNavigationSequence& sequence = CachedSequenceState(*session);
    const SampleNavigationSequenceProjection projection =
        ProjectSampleNavigationSequence(sequence, session->current_index);
    PopulateResultFromSequence(result, *session, sequence, projection);
    if (session->spectrum_count == 0) {
        return result;
    }

    const std::optional<std::size_t> target_index = ResolveNavigationTarget(
        request,
        sequence,
        projection,
        session->manifest,
        session->spectrum_count,
        session->filter_active,
        result.blocked_by_filter);

    if (!target_index) {
        return result;
    }

    result.target_found = true;
    session->current_index = *target_index;
    session->pending_index.reset();
    session->pending_navigation_remembers_labeling_position = false;
    result.current_index = *session->current_index;
    result.moved = result.current_index != result.previous_index;
    PopulateResultFromSequence(
        result,
        *session,
        sequence,
        ProjectSampleNavigationSequence(sequence, session->current_index));
    PersistActiveIndex();
    return result;
}

SampleNavigationResult SampleNavigationController::NavigateDeferred(
    const SampleNavigationRequest& request,
    bool remember_labeling_position,
    std::optional<std::size_t> base_index,
    NavigationTargetResolutionReport* target_resolution)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return {};
    }

    const bool sequence_cache_hit = session->sequence_state_cache_valid;
    if (target_resolution != nullptr) {
        target_resolution->row_count = session->spectrum_count;
        target_resolution->filter_active = session->filter_active;
        target_resolution->sort_active =
            session->sort_choice.active &&
            session->sort_choice.values.size() == session->spectrum_count;
        target_resolution->query_active = !session->sample_name_query.empty();
        target_resolution->pending_present = session->pending_index.has_value();
        target_resolution->sequence_cache_hit = sequence_cache_hit;
    }
    if (!base_index) {
        base_index = session->pending_index ? session->pending_index : session->current_index;
    }
    SampleNavigationResult result;
    result.has_active_source = true;
    result.previous_index = base_index.value_or(0);
    result.current_index = base_index.value_or(0);
    const TargetResolutionClock::time_point base_sequence_started_at =
        target_resolution != nullptr ? TargetResolutionClock::now()
                                     : TargetResolutionClock::time_point{};
    const SampleNavigationSequence& sequence = CachedSequenceState(*session);
    const SampleNavigationSequenceProjection base_projection =
        ProjectSampleNavigationSequence(sequence, base_index);
    if (target_resolution != nullptr) {
        target_resolution->base_sequence_ns +=
            ElapsedNanoseconds(base_sequence_started_at);
        target_resolution->sequence_build_count += sequence_cache_hit ? 0U : 1U;
    }
    PopulateResultFromSequence(result, *session, sequence, base_projection);
    if (session->spectrum_count == 0) {
        return result;
    }

    const TargetResolutionClock::time_point target_lookup_started_at =
        target_resolution != nullptr ? TargetResolutionClock::now()
                                     : TargetResolutionClock::time_point{};
    std::optional<std::size_t> target_index = ResolveNavigationTarget(
        request,
        sequence,
        base_projection,
        session->manifest,
        session->spectrum_count,
        session->filter_active,
        result.blocked_by_filter);
    if (!target_index && request.kind == SampleNavigationRequestKind::LabelAdvance &&
        session->pending_index && session->pending_index != base_index &&
        sequence.ContainsSourceRow(*session->pending_index) &&
        (request.eligible_samples.empty() ||
         (*session->pending_index < request.eligible_samples.size() &&
          request.eligible_samples[*session->pending_index]))) {
        target_index = session->pending_index;
    }
    if (target_resolution != nullptr) {
        target_resolution->target_lookup_ns +=
            ElapsedNanoseconds(target_lookup_started_at);
    }
    if (!target_index) {
        return result;
    }

    result.target_found = true;
    result.moved = *target_index != result.previous_index;
    if (session->current_index && *target_index == *session->current_index) {
        session->pending_index.reset();
        session->pending_navigation_remembers_labeling_position = false;
    } else if (session->pending_index != target_index) {
        session->pending_index = *target_index;
        session->pending_navigation_remembers_labeling_position = remember_labeling_position;
    } else {
        session->pending_navigation_remembers_labeling_position =
            session->pending_navigation_remembers_labeling_position || remember_labeling_position;
    }
    const TargetResolutionClock::time_point target_sequence_started_at =
        target_resolution != nullptr ? TargetResolutionClock::now()
                                     : TargetResolutionClock::time_point{};
    const SampleNavigationSequenceProjection target_projection =
        ProjectSampleNavigationSequence(sequence, *target_index);
    if (target_resolution != nullptr) {
        target_resolution->target_sequence_ns +=
            ElapsedNanoseconds(target_sequence_started_at);
    }
    PopulateResultFromSequence(result, *session, sequence, target_projection);
    result.current_index = *target_index;
    return result;
}

bool SampleNavigationController::RetargetDeferredNavigation(std::size_t spectrum_index)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr || !session->pending_index) {
        return false;
    }
    session->pending_index = spectrum_index;
    return true;
}

bool SampleNavigationController::PresentExplicitSample(std::size_t spectrum_index)
{
    SourceSession* session = ActiveSession();
    if (!session || spectrum_index >= session->spectrum_count) return false;
    session->current_index = spectrum_index;
    session->pending_index.reset();
    session->pending_navigation_remembers_labeling_position = false;
    if (session->filter_active) session->index_before_active_filter = spectrum_index;
    PersistActiveIndex();
    return true;
}

bool SampleNavigationController::CommitDeferredNavigation(std::size_t spectrum_index)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr || session->pending_index != spectrum_index) {
        return false;
    }

    session->current_index = spectrum_index;
    session->pending_index.reset();
    session->pending_navigation_remembers_labeling_position = false;
    PersistActiveIndex();
    return true;
}

void SampleNavigationController::CancelDeferredNavigation()
{
    if (SourceSession* session = ActiveSession()) {
        session->pending_index.reset();
        session->pending_navigation_remembers_labeling_position = false;
    }
}

std::optional<std::size_t> SampleNavigationController::current_index() const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || session->spectrum_count == 0 || !session->current_index) {
        return std::nullopt;
    }
    return session->current_index;
}

std::optional<std::size_t> SampleNavigationController::pending_index() const
{
    const SourceSession* session = ActiveSession();
    return session == nullptr ? std::nullopt : session->pending_index;
}

bool SampleNavigationController::pending_navigation_remembers_labeling_position() const
{
    const SourceSession* session = ActiveSession();
    return session != nullptr && session->pending_index &&
           session->pending_navigation_remembers_labeling_position;
}

std::optional<std::size_t> SampleNavigationController::spectrum_count() const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return std::nullopt;
    }
    return session->spectrum_count;
}

bool SampleNavigationController::can_move_previous() const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || session->spectrum_count == 0) {
        return false;
    }
    const SampleNavigationSequence& sequence = CachedSequenceState(*session);
    const SampleNavigationSequenceProjection projection = ProjectSampleNavigationSequence(
        sequence,
        session->pending_index ? session->pending_index : session->current_index);
    return projection.previous_target && projection.current_source_row &&
           *projection.previous_target != *projection.current_source_row;
}

bool SampleNavigationController::can_move_next() const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || session->spectrum_count == 0) {
        return false;
    }
    const SampleNavigationSequence& sequence = CachedSequenceState(*session);
    const SampleNavigationSequenceProjection projection = ProjectSampleNavigationSequence(
        sequence,
        session->pending_index ? session->pending_index : session->current_index);
    return projection.next_target && projection.current_source_row &&
           *projection.next_target != *projection.current_source_row;
}

std::optional<std::size_t> SampleNavigationController::SetSampleFilter(
    std::vector<bool> included_samples,
    bool defer_navigation)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return std::nullopt;
    }
    if (included_samples.size() != session->spectrum_count) {
        return ClearSampleFilter(defer_navigation);
    }

    const std::optional<std::size_t> previous_index = session->current_index;
    if (!session->filter_active) {
        session->index_before_active_filter = session->current_index;
    }
    session->filter_active = true;
    session->filter_included_samples = std::move(included_samples);
    session->filtered_sample_count = static_cast<std::size_t>(std::count(
        session->filter_included_samples.begin(),
        session->filter_included_samples.end(),
        true));
    InvalidateSequenceState(*session);
    const std::optional<std::size_t> deferred_target = defer_navigation
        ? ReconcileDeferredWithSequence(*session)
        : ReconcileCurrentWithSequence(*session);
    RecomputeMatches(*session);
    RefreshSequenceTopologyRevision();
    if (defer_navigation) {
        return deferred_target;
    }
    if (session->current_index != previous_index) {
        PersistActiveIndex();
        return session->current_index;
    }
    return std::nullopt;
}

std::optional<std::size_t> SampleNavigationController::ClearSampleFilter(bool defer_navigation)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::size_t> previous_index = session->current_index;
    const std::optional<std::size_t> restored_index =
        session->index_before_active_filter &&
            *session->index_before_active_filter < session->spectrum_count
        ? session->index_before_active_filter
        : std::nullopt;
    session->filter_active = false;
    session->filter_included_samples.clear();
    session->filtered_sample_count = 0;
    if (!defer_navigation && restored_index) {
        session->current_index = *restored_index;
    } else if (!defer_navigation && !session->current_index && session->spectrum_count > 0) {
        session->current_index = 0;
    }
    session->index_before_active_filter.reset();
    InvalidateSequenceState(*session);
    const std::optional<std::size_t> deferred_target = defer_navigation
        ? ReconcileDeferredWithSequence(*session, restored_index)
        : ReconcileCurrentWithSequence(*session);
    RecomputeMatches(*session);
    RefreshSequenceTopologyRevision();
    if (defer_navigation) {
        return deferred_target;
    }
    if (session->current_index != previous_index) {
        PersistActiveIndex();
        return session->current_index;
    }
    return std::nullopt;
}

bool SampleNavigationController::filter_active() const
{
    const SourceSession* session = ActiveSession();
    return session != nullptr && session->filter_active;
}

std::size_t SampleNavigationController::filtered_sample_count() const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return 0;
    }
    const SampleNavigationSequence& sequence = CachedSequenceState(*session);
    return sequence.active ? sequence.ordered_rows.size() : session->spectrum_count;
}

bool SampleNavigationController::current_sample_in_filter() const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || !session->filter_active) {
        return true;
    }
    return session->current_index && IsSampleInFilter(*session, *session->current_index);
}

std::optional<std::size_t> SampleNavigationController::SetSampleSorting(
    SampleNavigationSortChoice sort_choice,
    bool defer_navigation)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr || sort_choice.values.size() != session->spectrum_count) {
        return ClearSampleSorting(defer_navigation);
    }

    const std::optional<std::size_t> previous_index = session->current_index;
    session->sort_choice = std::move(sort_choice);
    session->sort_choice.active = true;
    InvalidateSequenceState(*session);
    const std::optional<std::size_t> deferred_target = defer_navigation
        ? ReconcileDeferredWithSequence(*session)
        : ReconcileCurrentWithSequence(*session);
    RecomputeMatches(*session);
    RefreshSequenceTopologyRevision();
    if (defer_navigation) {
        return deferred_target;
    }
    if (session->current_index != previous_index) {
        PersistActiveIndex();
        return session->current_index;
    }
    return std::nullopt;
}

std::optional<std::size_t> SampleNavigationController::ClearSampleSorting(bool defer_navigation)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return std::nullopt;
    }

    const std::optional<std::size_t> previous_index = session->current_index;
    session->sort_choice = {};
    InvalidateSequenceState(*session);
    const std::optional<std::size_t> deferred_target = defer_navigation
        ? ReconcileDeferredWithSequence(*session)
        : ReconcileCurrentWithSequence(*session);
    RecomputeMatches(*session);
    RefreshSequenceTopologyRevision();
    if (defer_navigation) {
        return deferred_target;
    }
    if (session->current_index != previous_index) {
        PersistActiveIndex();
        return session->current_index;
    }
    return std::nullopt;
}

bool SampleNavigationController::sorting_active() const
{
    const SourceSession* session = ActiveSession();
    return session != nullptr && session->sort_choice.active &&
           session->sort_choice.values.size() == session->spectrum_count;
}

const SampleNavigationSequence& SampleNavigationController::current_sequence() const
{
    static const SampleNavigationSequence kEmptySequence;
    const SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return kEmptySequence;
    }
    return CachedSequence(*session);
}

std::vector<std::size_t> SampleNavigationController::AdjacentRows(
    SampleNavigationDirection direction,
    SampleNavigationPrefetchPolicy policy) const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || !session->current_index) {
        return {};
    }

    const SampleNavigationSequence& sequence = CachedSequence(*session);
    const SampleNavigationSequenceProjection projection =
        ProjectSampleNavigationSequence(sequence, session->current_index);
    if (!projection.current_sequence_position) {
        return {};
    }

    const bool implicit_source_order =
        !sequence.active && sequence.ordered_rows.empty();
    const std::size_t sequence_count = implicit_source_order
        ? sequence.source_row_count
        : sequence.ordered_rows.size();
    if (sequence_count == 0 ||
        *projection.current_sequence_position >= sequence_count) {
        return {};
    }
    const auto row_at = [&sequence, implicit_source_order](
                            std::size_t position) {
        return implicit_source_order ? position
                                     : sequence.ordered_rows[position];
    };
    const auto append = [&row_at,
                         sequence_count,
                         current_position =
                             *projection.current_sequence_position](
                            std::vector<std::size_t>& rows,
                            bool forward,
                            std::size_t count) {
        for (std::size_t offset = 1; offset <= count; ++offset) {
            if (forward) {
                if (offset > sequence_count - current_position - 1) {
                    break;
                }
                rows.push_back(row_at(current_position + offset));
            } else {
                if (offset > current_position) {
                    break;
                }
                rows.push_back(row_at(current_position - offset));
            }
        }
    };

    std::vector<std::size_t> rows;
    rows.reserve(policy.ahead + policy.behind);
    const bool ahead_is_forward =
        direction == SampleNavigationDirection::Next;
    append(rows, ahead_is_forward, policy.ahead);
    append(rows, !ahead_is_forward, policy.behind);
    return rows;
}

bool SampleNavigationController::SetSampleNameQuery(
    std::string query)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr || session->sample_name_query == query) {
        return false;
    }
    session->sample_name_query = std::move(query);
    RecomputeMatches(*session);
    return true;
}

std::string_view SampleNavigationController::sample_name_query() const
{
    const SourceSession* session = ActiveSession();
    return session == nullptr ? std::string_view{} : std::string_view(session->sample_name_query);
}

const std::vector<std::size_t>& SampleNavigationController::sample_name_matches() const
{
    static const std::vector<std::size_t> kEmptyMatches;
    const SourceSession* session = ActiveSession();
    return session == nullptr ? kEmptyMatches : session->sample_name_matches;
}

const SourceCollectionManifest* SampleNavigationController::active_context() const
{
    const SourceSession* session = ActiveSession();
    return session == nullptr ? nullptr : &session->manifest;
}

ExactSampleNameResolution
SampleNavigationController::ResolveExactSampleName(
    std::string_view name) const
{
    ExactSampleNameResolution resolution;
    const SourceSession* session = ActiveSession();
    if (session == nullptr ||
        session->manifest.sample_names.empty()) {
        return resolution;
    }

    resolution.names_available = true;
    for (std::size_t row = 0;
         row < session->manifest.sample_names.size();
         ++row) {
        if (UnicodeOrdinalCaseEqual(
                session->manifest.sample_names[row],
                name)) {
            resolution.matching_rows.push_back(row);
        }
    }
    if (!resolution.matching_rows.empty()) {
        resolution.first_match_in_active_sequence =
            CachedSequenceState(*session).ContainsSourceRow(
                resolution.matching_rows.front());
    }
    return resolution;
}

std::uint64_t
SampleNavigationController::active_context_generation() const
{
    return active_context_generation_;
}

std::uint64_t
SampleNavigationController::sequence_topology_revision() const
{
    return sequence_topology_revision_;
}

void SampleNavigationController::RunMaintenance(LocalUserStateSaveScheduler::TimePoint now)
{
    (void)state_cache_persistence_.RunMaintenance(
        now,
        [this] {
            return SaveStateCache();
        });
}

std::optional<LocalUserStateSaveScheduler::TimePoint>
SampleNavigationController::NextMaintenanceDeadline() const
{
    return state_cache_persistence_.NextMaintenanceDeadline();
}

bool SampleNavigationController::FlushStateCache()
{
    return state_cache_persistence_.Flush(
               [this] {
                   return SaveStateCache();
               }) !=
        LocalUserStatePersistenceLifecycle::FlushOutcome::Failed;
}

LocalUserStatePersistenceStatus
SampleNavigationController::PersistenceStatus() const
{
    return state_cache_persistence_.PersistenceStatus();
}

std::unordered_map<std::string, std::vector<std::filesystem::path>>
SampleNavigationController::AnnotationPathsBySourceKey() const
{
    std::unordered_map<std::string, std::vector<std::filesystem::path>> paths_by_source_key;
    for (const auto& [source_key, session_key] : source_key_to_session_key_) {
        const auto session = sessions_.find(session_key);
        if (session == sessions_.end()) {
            continue;
        }

        std::vector<std::filesystem::path> paths = AnnotationPaths(session->second.manifest);
        if (!paths.empty()) {
            paths_by_source_key.emplace(source_key, std::move(paths));
        }
    }
    return paths_by_source_key;
}

std::vector<std::filesystem::path> SampleNavigationController::AnnotationPathsForSourceKey(
    std::string_view source_key) const
{
    const auto session_key = source_key_to_session_key_.find(std::string(source_key));
    if (session_key == source_key_to_session_key_.end()) {
        return {};
    }
    const auto session = sessions_.find(session_key->second);
    return session == sessions_.end() ? std::vector<std::filesystem::path>{}
                                      : AnnotationPaths(session->second.manifest);
}

SampleNavigationController::SourceSession* SampleNavigationController::ActiveSession()
{
    if (!active_source_key_) {
        return nullptr;
    }
    const auto match = sessions_.find(*active_source_key_);
    return match == sessions_.end() ? nullptr : &match->second;
}

const SampleNavigationController::SourceSession* SampleNavigationController::ActiveSession() const
{
    if (!active_source_key_) {
        return nullptr;
    }
    const auto match = sessions_.find(*active_source_key_);
    return match == sessions_.end() ? nullptr : &match->second;
}

SampleNavigationSequence SampleNavigationController::BuildSequenceState(
    const SourceSession& session)
{
    SampleNavigationSequenceInput input;
    input.source_row_count = session.spectrum_count;
    input.filter_active = session.filter_active;
    input.materialize_source_order = false;
    input.included_samples = &session.filter_included_samples;
    input.sort_choice = &session.sort_choice;
    return BuildSampleNavigationSequence(input);
}

SampleNavigationController::PublishedSequenceTopology
SampleNavigationController::CapturePublishedSequenceTopology(
    const SourceSession& session,
    const SampleNavigationSequence& sequence)
{
    return PublishedSequenceTopology{
        .source_collection_identity =
            session.source_collection_identity,
        .source_fingerprint = session.source_fingerprint,
        .context_fingerprint = session.context_fingerprint,
        .active = sequence.active,
        .source_row_count = sequence.source_row_count,
        .ordered_rows = sequence.ordered_rows,
    };
}

bool SampleNavigationController::MatchesPublishedSequenceTopology(
    const PublishedSequenceTopology& published,
    const SourceSession& session,
    const SampleNavigationSequence& sequence)
{
    return published.source_collection_identity ==
               session.source_collection_identity &&
           published.source_fingerprint ==
               session.source_fingerprint &&
           published.context_fingerprint ==
               session.context_fingerprint &&
           published.active == sequence.active &&
           published.source_row_count ==
               sequence.source_row_count &&
           published.ordered_rows == sequence.ordered_rows;
}

const SampleNavigationSequence& SampleNavigationController::CachedSequenceState(
    const SourceSession& session)
{
    if (!session.sequence_state_cache_valid) {
        session.sequence_state_cache = BuildSequenceState(session);
        session.sequence_state_cache_valid = true;
    }
    return session.sequence_state_cache;
}

const SampleNavigationSequence& SampleNavigationController::CachedSequence(const SourceSession& session)
{
    const SampleNavigationSequence& state = CachedSequenceState(session);
    ApplySampleNavigationSequenceProjection(
        session.sequence_state_cache,
        ProjectSampleNavigationSequence(state, session.current_index));
    return session.sequence_state_cache;
}

void SampleNavigationController::InvalidateSequenceState(SourceSession& session)
{
    // Cache invalidation is not itself an observable topology change. Callers
    // that mutate filtering or sorting rebuild the sequence before publishing
    // its effective topology through RefreshSequenceTopologyRevision().
    session.sequence_state_cache_valid = false;
}

void SampleNavigationController::RefreshSequenceTopologyRevision()
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr) {
        if (!published_sequence_topology_) {
            return;
        }
        published_sequence_topology_.reset();
        ++sequence_topology_revision_;
        return;
    }

    // Topology mutations rebuild this cache before publication. A source
    // switch may deliberately mark it cold afterward, but leaves its last
    // effective topology resident so publication never warms the source.
    if (published_sequence_topology_ &&
        MatchesPublishedSequenceTopology(
            *published_sequence_topology_,
            *session,
            session->sequence_state_cache)) {
        return;
    }
    published_sequence_topology_ =
        CapturePublishedSequenceTopology(
            *session,
            session->sequence_state_cache);
    ++sequence_topology_revision_;
}

std::optional<std::size_t> SampleNavigationController::ReconcileCurrentWithSequence(SourceSession& session)
{
    const std::optional<std::size_t> previous_index = session.current_index;
    const SampleNavigationSequence& sequence = CachedSequenceState(session);
    const SampleNavigationSequenceProjection projection =
        ProjectSampleNavigationSequence(sequence, session.current_index);
    if (projection.current_source_row) {
        session.current_index = projection.current_source_row;
    } else if (!sequence.active && session.spectrum_count > 0) {
        session.current_index = 0;
    } else if (!sequence.ordered_rows.empty()) {
        session.current_index = sequence.ordered_rows.front();
    } else {
        session.current_index.reset();
    }

    if (session.current_index == previous_index) {
        return std::nullopt;
    }
    return session.current_index;
}

std::optional<std::size_t> SampleNavigationController::ReconcileDeferredWithSequence(
    SourceSession& session,
    std::optional<std::size_t> preferred_index)
{
    const std::optional<std::size_t> previous_pending_index = session.pending_index;
    if (!preferred_index) {
        preferred_index = session.pending_index ? session.pending_index : session.current_index;
    }

    const SampleNavigationSequence& sequence = CachedSequenceState(session);
    const SampleNavigationSequenceProjection projection =
        ProjectSampleNavigationSequence(sequence, preferred_index);
    std::optional<std::size_t> target_index;
    if (projection.current_source_row) {
        target_index = projection.current_source_row;
    } else if (!sequence.active && session.spectrum_count > 0) {
        target_index = 0;
    } else if (!sequence.ordered_rows.empty()) {
        target_index = sequence.ordered_rows.front();
    }

    if (!target_index) {
        session.current_index.reset();
        session.pending_index.reset();
        session.pending_navigation_remembers_labeling_position = false;
        return std::nullopt;
    }
    if (session.current_index == target_index) {
        session.pending_index.reset();
        session.pending_navigation_remembers_labeling_position = false;
        return std::nullopt;
    }
    if (previous_pending_index == target_index) {
        return std::nullopt;
    }

    session.pending_index = target_index;
    if (!previous_pending_index) {
        session.pending_navigation_remembers_labeling_position = false;
    }
    return target_index;
}

bool SampleNavigationController::IsSampleInFilter(const SourceSession& session, std::size_t sample_index)
{
    if (!session.filter_active) {
        return true;
    }
    return sample_index < session.filter_included_samples.size() && session.filter_included_samples[sample_index];
}

void SampleNavigationController::PopulateResultFromSequence(
    SampleNavigationResult& result,
    const SourceSession& session,
    const SampleNavigationSequence& sequence,
    const SampleNavigationSequenceProjection& projection)
{
    result.current_source_row = projection.current_source_row
        ? projection.current_source_row : session.current_index;
    result.has_current_sample = result.current_source_row.has_value();
    result.current_sequence_position = projection.current_sequence_position;
    result.sequence_active = sequence.active;
    result.sequence_empty = sequence.empty;
    const std::size_t sequence_count = sequence.active ? sequence.ordered_rows.size() : session.spectrum_count;
    result.sequence_count = sequence_count;
    result.filtered_sample_count = sequence_count;
    result.row_location_available = sequence.row_location_available;
    result.current_sample_in_filter = !session.filter_active ||
        (result.current_source_row && IsSampleInFilter(session, *result.current_source_row));
    if (projection.current_source_row) {
        result.current_index = *projection.current_source_row;
    } else {
        result.current_index = session.current_index.value_or(0);
    }
}

bool SampleNavigationController::LoadReadOnlyAnnotationIntoSession(
    SourceSession& session,
    const std::filesystem::path& path,
    std::string* message)
{
    return IngestReadOnlySampleAnnotation(
        session.manifest,
        path,
        ReadOnlyAnnotationCompatibilityView(
            session.canonical_source),
        message);
}

void SampleNavigationController::EnsureStateCacheLoaded()
{
    if (state_cache_loaded_) {
        return;
    }
    state_cache_loaded_ = true;
    SampleNavigationStateCacheLoadResult result =
        LoadSampleNavigationStateCache(state_cache_path_);
    state_cache_ = std::move(result.cache);
    state_cache_persistence_.SetLoadWarning(std::move(result.warning));
}

std::optional<std::size_t>
SampleNavigationController::CachedIndex(
    std::string_view source_identity) const
{
    const auto live =
        state_cache_.last_indices_by_source_identity.find(
            std::string{source_identity});
    if (live !=
        state_cache_.last_indices_by_source_identity.end()) {
        return live->second;
    }
    if (!state_cache_snapshot_) {
        return std::nullopt;
    }
    const auto persisted =
        state_cache_snapshot_->cache
            .last_indices_by_source_identity.find(
                std::string{source_identity});
    return persisted ==
            state_cache_snapshot_->cache
                .last_indices_by_source_identity.end()
        ? std::nullopt
        : std::optional<std::size_t>{persisted->second};
}

void SampleNavigationController::PersistActiveIndex()
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || session->source_collection_identity.empty() || session->spectrum_count == 0 ||
        !session->current_index) {
        return;
    }
    state_cache_.last_indices_by_source_identity[session->source_collection_identity] = *session->current_index;
    if (!state_cache_path_.empty()) {
        state_cache_persistence_.MarkDirty();
    }
}

LocalUserStatePersistenceLifecycle::SaveResult
SampleNavigationController::SaveStateCache()
{
    SampleNavigationStateCache merged =
        state_cache_snapshot_
        ? state_cache_snapshot_->cache
        : SampleNavigationStateCache{};
    for (const auto& [identity, index] :
         state_cache_.last_indices_by_source_identity) {
        merged.last_indices_by_source_identity[identity] = index;
    }
    if (SaveSampleNavigationStateCache(state_cache_path_, merged)) {
        return {.saved = true};
    }
    return {
        .saved = false,
        .error = "Could not save sample navigation state.",
    };
}

void SampleNavigationController::RecomputeMatches(SourceSession& session)
{
    session.sample_name_matches.clear();
    if (session.sample_name_query.empty() || session.manifest.sample_names.empty()) {
        return;
    }
    session.sample_name_matches = FindSampleNameMatches(
        CachedSequenceState(session),
        session.manifest.sample_names,
        session.sample_name_query);
}

}  // namespace spectiary
