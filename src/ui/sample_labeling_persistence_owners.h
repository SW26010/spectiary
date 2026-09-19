#pragma once

#include "app/runtime_paths.h"
#include "app/local_user_state_json.h"
#include "domain/sample_labeling.h"

#include <unordered_map>

namespace spectiary {

// Deliberately contains no canonical content or save/retry state.
struct SampleLabelingTaskRegistration {
    std::string task_id;
    std::optional<std::filesystem::path> output_path;
    SampleLabelingOutputArtifactFormat output_format =
        SampleLabelingOutputArtifactFormat::None;
    SampleLabelingSessionState session;
    // Only for unopened list presentation; hydration always replaces it from ASDF.
    std::optional<std::string> display_name_hint;
};

struct SampleLabelingSourceRegistration {
    std::size_t sample_count = 0;
    std::string source_name;
    std::string source_fingerprint;
    std::string context_fingerprint;
    std::optional<std::string> active_task_id;
    std::vector<SampleLabelingTaskRegistration> tasks;
};

struct SampleLabelingOrdinaryState {
    std::unordered_map<std::string, SampleLabelingSourceRegistration> sources;
};

// No output locator, workflow preferences, or persistent pending overlay.
struct SampleLabelingDraftCheckpoint {
    std::string task_id;
    std::string task_name;
    SampleLabelingTaskCanonicalMetadata canonical_metadata;
    SampleLabelSet label_set;
    std::vector<int> values;
};

struct SampleLabelingSourceDraftCheckpoint {
    std::size_t sample_count = 0;
    std::string source_name;
    std::string source_fingerprint;
    std::string context_fingerprint;
    SampleLabelingDraftCheckpoint draft;
};

struct SampleLabelingDraftCheckpoints {
    // Exactly one output-free slot per source. No historical generations.
    std::unordered_map<std::string, SampleLabelingSourceDraftCheckpoint> sources;
};

template <typename Owner>
struct SampleLabelingOwnerLoadResult {
    Owner owner;
    VersionedJsonCacheLoadIssueKind issue_kind = VersionedJsonCacheLoadIssueKind::None;
    std::string diagnostic;
};

[[nodiscard]] SampleLabelingOwnerLoadResult<SampleLabelingOrdinaryState>
LoadSampleLabelingOrdinaryState(const RuntimePaths& paths,
    const std::filesystem::path& path,
    const JsonCancellationCheckpoint& cancellation = {});

[[nodiscard]] SampleLabelingOwnerLoadResult<SampleLabelingDraftCheckpoints>
LoadSampleLabelingDraftCheckpoints(const std::filesystem::path& path,
    const JsonCancellationCheckpoint& cancellation = {});

[[nodiscard]] bool SaveSampleLabelingOrdinaryState(const RuntimePaths& paths,
    const std::filesystem::path& path, const SampleLabelingOrdinaryState& owner,
    std::string* error = nullptr);

[[nodiscard]] bool SaveSampleLabelingDraftCheckpoints(
    const std::filesystem::path& path, const SampleLabelingDraftCheckpoints& owner,
    std::string* error = nullptr);

} // namespace spectiary
