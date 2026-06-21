#include "ui/sample_navigation_controller.h"

#include "domain/sample_annotation_io.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace specforge {
namespace {

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
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

SampleNavigationRequest SampleNavigationRequest::LocateSampleName(std::string sample_name)
{
    SampleNavigationRequest request;
    request.kind = SampleNavigationRequestKind::LocateSampleName;
    request.sample_name = std::move(sample_name);
    return request;
}

SampleNavigationController::SampleNavigationController()
    : SampleNavigationController(DefaultSampleNavigationStateCachePath())
{
}

SampleNavigationController::SampleNavigationController(std::filesystem::path state_cache_path)
    : state_cache_path_(std::move(state_cache_path))
{
}

void SampleNavigationController::ActivateSource(std::string source_key, const SpectrumSnapshotHandle& snapshot)
{
    if (source_key.empty() || !snapshot) {
        ClearActiveSource();
        return;
    }

    EnsureStateCacheLoaded();
    const SourceCollectionIdentity identity = BuildSourceCollectionIdentity(*snapshot);
    SourceSession& session = sessions_[identity.id];
    const bool new_session = session.source_collection_identity.empty();
    const bool context_changed = session.context_fingerprint != identity.context_fingerprint;
    const std::string previous_query = std::move(session.sample_name_query);
    const std::size_t previous_index = session.current_index;

    session.source_collection_identity = identity.id;
    session.source_name = identity.source_name;
    session.source_fingerprint = identity.source_fingerprint;
    session.context_fingerprint = identity.context_fingerprint;
    session.spectrum_count = identity.spectrum_count;
    if (context_changed) {
        session.manifest = LoadSourceCollectionManifest(*snapshot);
    }
    session.sample_name_query = previous_query;
    if (session.spectrum_count > 0) {
        const auto persisted = state_cache_.last_indices_by_source_identity.find(identity.id);
        if (new_session && persisted != state_cache_.last_indices_by_source_identity.end() &&
            persisted->second < session.spectrum_count) {
            session.current_index = persisted->second;
        } else if (new_session) {
            session.current_index = std::min(snapshot->collection.current_index, session.spectrum_count - 1);
        } else {
            session.current_index = std::min(previous_index, session.spectrum_count - 1);
        }
    } else {
        session.current_index = 0;
    }
    if (session.filter_active && session.filter_included_samples.size() != session.spectrum_count) {
        session.filter_active = false;
        session.filter_included_samples.clear();
        session.filtered_sample_count = 0;
    }
    RecomputeMatches(session);

    source_key_to_session_key_[source_key] = identity.id;
    active_source_key_ = identity.id;
    PersistActiveIndex();
}

void SampleNavigationController::RemoveSource(std::string_view source_key)
{
    const std::string external_key(source_key);
    std::string session_key = external_key;
    const auto mapped = source_key_to_session_key_.find(external_key);
    if (mapped != source_key_to_session_key_.end()) {
        session_key = mapped->second;
        source_key_to_session_key_.erase(mapped);
    }

    sessions_.erase(session_key);
    if (active_source_key_ && *active_source_key_ == session_key) {
        active_source_key_.reset();
    }
}

void SampleNavigationController::ClearActiveSource()
{
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

    std::string load_error;
    std::optional<SampleAnnotationResult> annotation =
        LoadSampleAnnotationResultFromPath(path, session->spectrum_count, &load_error);
    if (!annotation) {
        std::string ignored_message = "Ignored " + PathToUtf8(path.filename()) + ": " + load_error + ".";
        session->manifest.messages.push_back(ignored_message);
        if (message != nullptr) {
            *message = std::move(ignored_message);
        }
        return false;
    }

    const auto same_path = [&path](const SampleAnnotationResult& existing) {
        return existing.path == path;
    };
    auto existing = std::find_if(session->manifest.annotations.begin(), session->manifest.annotations.end(), same_path);
    if (existing != session->manifest.annotations.end()) {
        *existing = std::move(*annotation);
    } else {
        session->manifest.annotations.push_back(std::move(*annotation));
    }
    if (message != nullptr) {
        *message = {};
    }
    return true;
}

SampleNavigationResult SampleNavigationController::Navigate(const SampleNavigationRequest& request)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return {};
    }

    SampleNavigationResult result;
    result.has_active_source = true;
    result.previous_index = session->current_index;
    result.current_index = session->current_index;
    result.current_sample_in_filter = IsSampleInFilter(*session, session->current_index);
    result.filtered_sample_count =
        session->filter_active ? session->filtered_sample_count : session->spectrum_count;
    if (session->spectrum_count == 0) {
        return result;
    }

    std::optional<std::size_t> target_index;
    switch (request.kind) {
    case SampleNavigationRequestKind::Previous:
        target_index = FindSequentialTarget(*session, false, result.blocked_by_filter);
        break;
    case SampleNavigationRequestKind::Next:
        target_index = FindSequentialTarget(*session, true, result.blocked_by_filter);
        break;
    case SampleNavigationRequestKind::LabelAdvance:
        target_index = FindLabelAdvanceTarget(*session, request.eligible_samples, result.blocked_by_filter);
        break;
    case SampleNavigationRequestKind::LocateRow:
        if (request.row_index < session->spectrum_count) {
            target_index = request.row_index;
        }
        break;
    case SampleNavigationRequestKind::LocateSampleName:
        target_index = FindSampleNameIndex(*session, request.sample_name);
        break;
    default:
        break;
    }

    if (!target_index) {
        return result;
    }

    result.target_found = true;
    session->current_index = *target_index;
    result.current_index = session->current_index;
    result.moved = result.current_index != result.previous_index;
    result.current_sample_in_filter = IsSampleInFilter(*session, session->current_index);
    result.filtered_sample_count =
        session->filter_active ? session->filtered_sample_count : session->spectrum_count;
    PersistActiveIndex();
    return result;
}

std::optional<std::size_t> SampleNavigationController::current_index() const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || session->spectrum_count == 0) {
        return std::nullopt;
    }
    return session->current_index;
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
    bool blocked_by_filter = false;
    const std::optional<std::size_t> target = FindSequentialTarget(*session, false, blocked_by_filter);
    return target && *target != session->current_index;
}

bool SampleNavigationController::can_move_next() const
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || session->spectrum_count == 0) {
        return false;
    }
    bool blocked_by_filter = false;
    const std::optional<std::size_t> target = FindSequentialTarget(*session, true, blocked_by_filter);
    return target && *target != session->current_index;
}

void SampleNavigationController::SetSampleFilter(std::vector<bool> included_samples)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return;
    }
    if (included_samples.size() != session->spectrum_count) {
        ClearSampleFilter();
        return;
    }

    session->filter_active = true;
    session->filter_included_samples = std::move(included_samples);
    session->filtered_sample_count = static_cast<std::size_t>(std::count(
        session->filter_included_samples.begin(),
        session->filter_included_samples.end(),
        true));
    RecomputeMatches(*session);
}

void SampleNavigationController::ClearSampleFilter()
{
    SourceSession* session = ActiveSession();
    if (session == nullptr) {
        return;
    }
    session->filter_active = false;
    session->filter_included_samples.clear();
    session->filtered_sample_count = 0;
    RecomputeMatches(*session);
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
    return session->filter_active ? session->filtered_sample_count : session->spectrum_count;
}

bool SampleNavigationController::current_sample_in_filter() const
{
    const SourceSession* session = ActiveSession();
    return session == nullptr || IsSampleInFilter(*session, session->current_index);
}

void SampleNavigationController::SetSampleNameQuery(std::string query)
{
    SourceSession* session = ActiveSession();
    if (session == nullptr || session->sample_name_query == query) {
        return;
    }
    session->sample_name_query = std::move(query);
    RecomputeMatches(*session);
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

std::optional<std::size_t> SampleNavigationController::FindSampleNameIndex(
    const SourceSession& session,
    std::string_view sample_name)
{
    if (sample_name.empty() || session.manifest.sample_names.empty()) {
        return std::nullopt;
    }

    const std::string target = LowerAscii(std::string(sample_name));
    for (std::size_t index = 0; index < session.manifest.sample_names.size(); ++index) {
        if (!IsSampleInFilter(session, index)) {
            continue;
        }
        if (LowerAscii(session.manifest.sample_names[index]) == target) {
            return index;
        }
    }
    for (std::size_t index = 0; index < session.manifest.sample_names.size(); ++index) {
        if (!IsSampleInFilter(session, index)) {
            continue;
        }
        if (LowerAscii(session.manifest.sample_names[index]).find(target) != std::string::npos) {
            return index;
        }
    }
    return std::nullopt;
}

bool SampleNavigationController::IsSampleInFilter(const SourceSession& session, std::size_t sample_index)
{
    if (!session.filter_active) {
        return true;
    }
    return sample_index < session.filter_included_samples.size() && session.filter_included_samples[sample_index];
}

std::optional<std::size_t> SampleNavigationController::FindSequentialTarget(
    const SourceSession& session,
    bool forward,
    bool& blocked_by_filter)
{
    blocked_by_filter = false;
    if (session.spectrum_count == 0) {
        return std::nullopt;
    }

    if (!session.filter_active) {
        if (forward) {
            return session.current_index + 1 < session.spectrum_count ? session.current_index + 1 : session.current_index;
        }
        return session.current_index > 0 ? session.current_index - 1 : session.current_index;
    }

    if (!IsSampleInFilter(session, session.current_index)) {
        blocked_by_filter = true;
        return std::nullopt;
    }

    if (forward) {
        for (std::size_t index = session.current_index + 1; index < session.spectrum_count; ++index) {
            if (IsSampleInFilter(session, index)) {
                return index;
            }
        }
        return session.current_index;
    }

    for (std::size_t index = session.current_index; index > 0; --index) {
        const std::size_t candidate = index - 1;
        if (IsSampleInFilter(session, candidate)) {
            return candidate;
        }
    }
    return session.current_index;
}

std::optional<std::size_t> SampleNavigationController::FindLabelAdvanceTarget(
    const SourceSession& session,
    const std::vector<bool>& eligible_samples,
    bool& blocked_by_filter)
{
    blocked_by_filter = false;
    if (session.spectrum_count == 0) {
        return std::nullopt;
    }
    if (session.filter_active && !IsSampleInFilter(session, session.current_index)) {
        blocked_by_filter = true;
        return std::nullopt;
    }

    const auto eligible = [&eligible_samples](std::size_t index) {
        return eligible_samples.empty() || (index < eligible_samples.size() && eligible_samples[index]);
    };
    for (std::size_t index = session.current_index + 1; index < session.spectrum_count; ++index) {
        if (IsSampleInFilter(session, index) && eligible(index)) {
            return index;
        }
    }
    return session.current_index;
}

void SampleNavigationController::EnsureStateCacheLoaded()
{
    if (state_cache_loaded_) {
        return;
    }
    state_cache_loaded_ = true;
    state_cache_ = LoadSampleNavigationStateCache(state_cache_path_);
}

void SampleNavigationController::PersistActiveIndex()
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || session->source_collection_identity.empty() || session->spectrum_count == 0) {
        return;
    }
    state_cache_.last_indices_by_source_identity[session->source_collection_identity] = session->current_index;
    SaveStateCache();
}

bool SampleNavigationController::SaveStateCache()
{
    return SaveSampleNavigationStateCache(state_cache_path_, state_cache_);
}

void SampleNavigationController::RecomputeMatches(SourceSession& session)
{
    session.sample_name_matches.clear();
    if (session.sample_name_query.empty() || session.manifest.sample_names.empty()) {
        return;
    }

    const std::string query = LowerAscii(session.sample_name_query);
    for (std::size_t index = 0; index < session.manifest.sample_names.size(); ++index) {
        if (!IsSampleInFilter(session, index)) {
            continue;
        }
        if (LowerAscii(session.manifest.sample_names[index]).find(query) != std::string::npos) {
            session.sample_name_matches.push_back(index);
        }
    }
}

}  // namespace specforge
