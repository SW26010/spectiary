#include "ui/sample_navigation_controller.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace specforge {
namespace {

constexpr const char* kStateFormatKind = "specforge.sample_navigation_state.cache";
constexpr int kStateSchemaVersion = 1;

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string JsonEscape(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char character : value) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

std::string JsonUnescape(std::string_view value)
{
    std::string unescaped;
    unescaped.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (character != '\\' || index + 1 >= value.size()) {
            unescaped.push_back(character);
            continue;
        }
        const char escaped = value[++index];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            unescaped.push_back(escaped);
            break;
        case 'n':
            unescaped.push_back('\n');
            break;
        case 'r':
            unescaped.push_back('\r');
            break;
        case 't':
            unescaped.push_back('\t');
            break;
        default:
            unescaped.push_back(escaped);
            break;
        }
    }
    return unescaped;
}

void WriteJsonString(std::ostream& stream, std::string_view value)
{
    stream << '"' << JsonEscape(value) << '"';
}

std::unordered_map<std::string, std::size_t> LoadStateCache(const std::filesystem::path& path)
{
    std::unordered_map<std::string, std::size_t> indices;
    if (path.empty()) {
        return indices;
    }

    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        return indices;
    }

    std::ifstream stream(path);
    if (!stream.good()) {
        return indices;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string contents = buffer.str();
    if (contents.find(kStateFormatKind) == std::string::npos) {
        return indices;
    }

    const std::regex item_expression(
        "\\{\\s*\"identity\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"\\s*,\\s*\"last_index\"\\s*:\\s*([0-9]+)\\s*\\}");
    for (std::sregex_iterator it(contents.begin(), contents.end(), item_expression), end; it != end; ++it) {
        const std::string identity = JsonUnescape((*it)[1].str());
        const std::size_t index = static_cast<std::size_t>(std::stoull((*it)[2].str()));
        if (!identity.empty()) {
            indices[identity] = index;
        }
    }
    return indices;
}

std::filesystem::path TemporaryCachePath(const std::filesystem::path& path)
{
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path temporary = path;
    temporary += ".tmp.";
#ifdef _WIN32
    temporary += std::to_string(GetCurrentProcessId());
#else
    temporary += "pid";
#endif
    temporary += ".";
    temporary += std::to_string(timestamp);
    return temporary;
}

bool ReplaceFileAtomically(const std::filesystem::path& temporary_path, const std::filesystem::path& target_path)
{
#ifdef _WIN32
    return MoveFileExW(
               temporary_path.c_str(),
               target_path.c_str(),
               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary_path, target_path, rename_error);
    return !rename_error;
#endif
}

bool SaveStateCacheFile(
    const std::filesystem::path& path,
    const std::unordered_map<std::string, std::size_t>& indices)
{
    if (path.empty()) {
        return false;
    }

    std::error_code filesystem_error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            return false;
        }
    }

    std::vector<std::string> keys;
    keys.reserve(indices.size());
    for (const auto& [key, value] : indices) {
        (void)value;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());

    const std::filesystem::path temporary_path = TemporaryCachePath(path);
    std::ofstream stream(temporary_path, std::ios::trunc);
    if (!stream.good()) {
        return false;
    }

    stream << "{\n";
    stream << "  \"format_kind\": ";
    WriteJsonString(stream, kStateFormatKind);
    stream << ",\n";
    stream << "  \"schema_version\": " << kStateSchemaVersion << ",\n";
    stream << "  \"sources\": [";
    if (!keys.empty()) {
        stream << "\n";
    }
    for (std::size_t index = 0; index < keys.size(); ++index) {
        stream << "    { \"identity\": ";
        WriteJsonString(stream, keys[index]);
        stream << ", \"last_index\": " << indices.at(keys[index]) << " }";
        stream << (index + 1 == keys.size() ? "\n" : ",\n");
    }
    if (!keys.empty()) {
        stream << "  ";
    }
    stream << "]\n";
    stream << "}\n";
    if (!stream.good()) {
        stream.close();
        std::filesystem::remove(temporary_path, filesystem_error);
        return false;
    }
    stream.close();
    if (!stream.good()) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return false;
    }
    if (!ReplaceFileAtomically(temporary_path, path)) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return false;
    }
    return true;
}

std::filesystem::path DefaultSampleNavigationStatePath()
{
#ifdef _WIN32
    DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 0) {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
        if (written > 0 && written < required) {
            value.resize(written);
            return std::filesystem::path(value) / L"SpecForge" / L"sample-navigation-state.json";
        }
    }
#endif
    return std::filesystem::temp_directory_path() / "SpecForge" / "sample-navigation-state.json";
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
    : SampleNavigationController(DefaultSampleNavigationStatePath())
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
    const SampleCollectionIdentity identity = BuildSampleCollectionIdentity(*snapshot);
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
        session.context = LoadSampleCollectionContext(*snapshot);
    }
    session.sample_name_query = previous_query;
    if (session.spectrum_count > 0) {
        const auto persisted = persisted_indices_.find(identity.id);
        if (new_session && persisted != persisted_indices_.end() && persisted->second < session.spectrum_count) {
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

const SampleCollectionContext* SampleNavigationController::active_context() const
{
    const SourceSession* session = ActiveSession();
    return session == nullptr ? nullptr : &session->context;
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
    if (sample_name.empty() || session.context.sample_names.empty()) {
        return std::nullopt;
    }

    const std::string target = LowerAscii(std::string(sample_name));
    for (std::size_t index = 0; index < session.context.sample_names.size(); ++index) {
        if (!IsSampleInFilter(session, index)) {
            continue;
        }
        if (LowerAscii(session.context.sample_names[index]) == target) {
            return index;
        }
    }
    for (std::size_t index = 0; index < session.context.sample_names.size(); ++index) {
        if (!IsSampleInFilter(session, index)) {
            continue;
        }
        if (LowerAscii(session.context.sample_names[index]).find(target) != std::string::npos) {
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
    persisted_indices_ = LoadStateCache(state_cache_path_);
}

void SampleNavigationController::PersistActiveIndex()
{
    const SourceSession* session = ActiveSession();
    if (session == nullptr || session->source_collection_identity.empty() || session->spectrum_count == 0) {
        return;
    }
    persisted_indices_[session->source_collection_identity] = session->current_index;
    SaveStateCache();
}

bool SampleNavigationController::SaveStateCache()
{
    return SaveStateCacheFile(state_cache_path_, persisted_indices_);
}

void SampleNavigationController::RecomputeMatches(SourceSession& session)
{
    session.sample_name_matches.clear();
    if (session.sample_name_query.empty() || session.context.sample_names.empty()) {
        return;
    }

    const std::string query = LowerAscii(session.sample_name_query);
    for (std::size_t index = 0; index < session.context.sample_names.size(); ++index) {
        if (!IsSampleInFilter(session, index)) {
            continue;
        }
        if (LowerAscii(session.context.sample_names[index]).find(query) != std::string::npos) {
            session.sample_name_matches.push_back(index);
        }
    }
}

}  // namespace specforge
