#pragma once

#include "domain/sample_annotation_io.h"
#include "domain/spectrum_snapshot.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace specforge {

enum class SampleNavigationRequestKind {
    Previous,
    Next,
    LabelAdvance,
    LocateRow,
    LocateSampleName,
};

struct SampleNavigationRequest {
    SampleNavigationRequestKind kind = SampleNavigationRequestKind::LocateRow;
    std::size_t row_index = 0;
    std::string sample_name;
    std::vector<bool> eligible_samples;

    [[nodiscard]] static SampleNavigationRequest Previous();
    [[nodiscard]] static SampleNavigationRequest Next();
    [[nodiscard]] static SampleNavigationRequest LabelAdvance();
    [[nodiscard]] static SampleNavigationRequest LabelAdvanceToEligible(std::vector<bool> eligible_samples);
    [[nodiscard]] static SampleNavigationRequest LocateRow(std::size_t row_index);
    [[nodiscard]] static SampleNavigationRequest LocateSampleName(std::string sample_name);
};

struct SampleNavigationResult {
    bool has_active_source = false;
    bool target_found = false;
    bool moved = false;
    bool blocked_by_filter = false;
    bool current_sample_in_filter = true;
    std::size_t previous_index = 0;
    std::size_t current_index = 0;
    std::size_t filtered_sample_count = 0;
};

class SampleNavigationController {
public:
    SampleNavigationController();
    explicit SampleNavigationController(std::filesystem::path state_cache_path);

    void ActivateSource(std::string source_key, const SpectrumSnapshotHandle& snapshot);
    void RemoveSource(std::string_view source_key);
    void ClearActiveSource();
    [[nodiscard]] bool AddReadOnlyAnnotationToActiveSource(
        const std::filesystem::path& path,
        std::string* message = nullptr);

    [[nodiscard]] SampleNavigationResult Navigate(const SampleNavigationRequest& request);
    [[nodiscard]] std::optional<std::size_t> current_index() const;
    [[nodiscard]] std::optional<std::size_t> spectrum_count() const;
    [[nodiscard]] bool can_move_previous() const;
    [[nodiscard]] bool can_move_next() const;
    void SetSampleFilter(std::vector<bool> included_samples);
    void ClearSampleFilter();
    [[nodiscard]] bool filter_active() const;
    [[nodiscard]] std::size_t filtered_sample_count() const;
    [[nodiscard]] bool current_sample_in_filter() const;
    void SetSampleNameQuery(std::string query);
    [[nodiscard]] std::string_view sample_name_query() const;
    [[nodiscard]] const std::vector<std::size_t>& sample_name_matches() const;
    [[nodiscard]] const SampleCollectionContext* active_context() const;

private:
    struct SourceSession {
        std::string source_collection_identity;
        std::string source_name;
        std::string source_fingerprint;
        std::string context_fingerprint;
        std::size_t spectrum_count = 0;
        std::size_t current_index = 0;
        SampleCollectionContext context;
        std::string sample_name_query;
        std::vector<std::size_t> sample_name_matches;
        bool filter_active = false;
        std::vector<bool> filter_included_samples;
        std::size_t filtered_sample_count = 0;
    };

    [[nodiscard]] SourceSession* ActiveSession();
    [[nodiscard]] const SourceSession* ActiveSession() const;
    [[nodiscard]] static std::optional<std::size_t> FindSampleNameIndex(
        const SourceSession& session,
        std::string_view sample_name);
    [[nodiscard]] static bool IsSampleInFilter(const SourceSession& session, std::size_t sample_index);
    [[nodiscard]] static std::optional<std::size_t> FindSequentialTarget(
        const SourceSession& session,
        bool forward,
        bool& blocked_by_filter);
    [[nodiscard]] static std::optional<std::size_t> FindLabelAdvanceTarget(
        const SourceSession& session,
        const std::vector<bool>& eligible_samples,
        bool& blocked_by_filter);
    void EnsureStateCacheLoaded();
    void PersistActiveIndex();
    bool SaveStateCache();
    static void RecomputeMatches(SourceSession& session);

    std::unordered_map<std::string, SourceSession> sessions_;
    std::unordered_map<std::string, std::string> source_key_to_session_key_;
    std::unordered_map<std::string, std::size_t> persisted_indices_;
    std::filesystem::path state_cache_path_;
    std::optional<std::string> active_source_key_;
    bool state_cache_loaded_ = false;
};

}  // namespace specforge
