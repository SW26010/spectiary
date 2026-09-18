#include "ui/sample_labeling_persistence_owners.h"
#include "domain/uuid_v4.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace specforge;
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
std::string Read(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void TestOwners(const std::filesystem::path& root)
{
    auto paths = DefaultRuntimePaths();
    const auto state_path = root / "state" / "sample-labeling-state.json";
    const auto drafts_path = root / "unsaved" / "sample-labeling-drafts.json";
    const std::string id = "12345678-1234-4234-8234-123456789abc";
    auto task = CreateSampleLabelingTask(id, "Unsaved work", 2);
    Require(UpsertSampleLabel(task.label_set, {1, "Galaxy", 'g'}), "create label");
    task.values.Complete() = {1, -1};
    SampleLabelingDraftCheckpoints drafts;
    drafts.sources["source"] = {.sample_count = 2, .source_name = "source",
        .source_fingerprint = "fingerprint", .context_fingerprint = "roster",
        .draft = {id, task.task_name, task.canonical_metadata, task.label_set, task.values.Complete()}};
    SampleLabelingOrdinaryState state;
    state.sources["source"] = {.sample_count = 2, .source_name = "source",
        .source_fingerprint = "fingerprint", .context_fingerprint = "roster",
        .active_task_id = id, .tasks = {{.task_id = id, .session = {.auto_advance = true}}}};
    std::string error;
    Require(SaveSampleLabelingOrdinaryState(paths, state_path, state, &error), error.c_str());
    Require(SaveSampleLabelingDraftCheckpoints(drafts_path, drafts, &error), error.c_str());
    const auto state_json = nlohmann::json::parse(Read(state_path));
    const auto draft_json = nlohmann::json::parse(Read(drafts_path));
    const auto& registration = state_json["sources"][0]["tasks"][0];
    for (const auto field : {"task_name", "labels", "canonical_metadata", "values", "pending_values", "save_state", "initial_publication_pending"})
        Require(!registration.contains(field), "ordinary state contains content or retry state");
    const auto& checkpoint = draft_json["sources"][0]["draft"];
    Require(!checkpoint.contains("output") && !checkpoint.contains("auto_advance"), "draft contains registration/session");
    auto loaded_state = LoadSampleLabelingOrdinaryState(paths, state_path);
    auto loaded_drafts = LoadSampleLabelingDraftCheckpoints(drafts_path);
    Require(loaded_state.issue_kind == VersionedJsonCacheLoadIssueKind::None &&
        loaded_state.owner.sources.at("source").tasks[0].session.auto_advance, "session roundtrip");
    Require(loaded_drafts.issue_kind == VersionedJsonCacheLoadIssueKind::None &&
        loaded_drafts.owner.sources.at("source").draft.values == std::vector<int>({1, -1}), "draft content roundtrip");

    // Sharing denial forces atomic replacement to fail, preserving the complete previous checkpoint.
    const auto previous = Read(drafts_path);
    HANDLE held = CreateFileW(drafts_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(held != INVALID_HANDLE_VALUE, "open checkpoint sharing denial");
    drafts.sources.at("source").draft.values[1] = 1;
    const bool saved = SaveSampleLabelingDraftCheckpoints(drafts_path, drafts, &error);
    CloseHandle(held);
    Require(!saved && Read(drafts_path) == previous, "failed replacement destroyed checkpoint");
    Require(SaveSampleLabelingDraftCheckpoints(drafts_path, drafts, &error), "retry checkpoint replacement");

    auto invalid = draft_json;
    invalid["sources"].push_back(invalid["sources"][0]);
    { std::ofstream stream(drafts_path); stream << invalid.dump(); }
    loaded_drafts = LoadSampleLabelingDraftCheckpoints(drafts_path);
    Require(loaded_drafts.issue_kind == VersionedJsonCacheLoadIssueKind::InvalidDocument &&
        loaded_drafts.owner.sources.empty(), "conflicting slots must fail closed");
    invalid = state_json;
    invalid["sources"][0]["tasks"][0]["pending_values"] = nlohmann::json::array();
    { std::ofstream stream(state_path); stream << invalid.dump(); }
    loaded_state = LoadSampleLabelingOrdinaryState(paths, state_path);
    Require(loaded_state.issue_kind == VersionedJsonCacheLoadIssueKind::InvalidDocument &&
        loaded_state.owner.sources.empty(), "ordinary codec must reject persistent overlays");
}
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("specforge-labeling-owners-" + std::to_string(GetCurrentProcessId()));
    try {
        TestOwners(root);
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
