#pragma once

#include "domain/spectrum_snapshot.h"
#include "ui/source_collection_session_types.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace specforge {

struct SourceCollectionRosterRemoveResult {
    SourceCollectionSessionAction action;
    bool removed = false;
    bool removed_current = false;
    std::string removed_source_key;
};

struct SourceCollectionRosterPreparedOpenResult {
    SourceCollectionSessionAction action;
    SpectrumSnapshotHandle replaced_cached_snapshot;
};

class SourceCollectionRoster {
public:
    using SnapshotLoader = std::function<SpectrumSnapshotHandle(const std::filesystem::path&, std::size_t)>;

    explicit SourceCollectionRoster(SnapshotLoader snapshot_loader);

    [[nodiscard]] const SpectrumSnapshotHandle& snapshot() const;
    [[nodiscard]] std::optional<std::size_t> current_source_index() const;
    [[nodiscard]] bool has_source(std::size_t source_index) const;
    [[nodiscard]] std::optional<std::string> current_source_key() const;
    [[nodiscard]] bool has_active_source() const;
    [[nodiscard]] std::vector<SourceCollectionSourceView> SourceViews() const;
    [[nodiscard]] std::vector<SourceCollectionSavedSource> SavedSources() const;
    [[nodiscard]] std::vector<std::string> SavedSourceKeys() const;

    [[nodiscard]] SourceCollectionSessionAction OpenSource(
        const std::filesystem::path& path,
        std::size_t spectrum_index);
    [[nodiscard]] SourceCollectionRosterPreparedOpenResult OpenPreparedSource(
        const std::filesystem::path& path,
        std::size_t spectrum_index,
        SpectrumSnapshotHandle snapshot);
    [[nodiscard]] SourceCollectionSessionAction ActivateSource(std::size_t source_index);
    [[nodiscard]] SourceCollectionRosterRemoveResult RemoveSource(std::size_t source_index);
    [[nodiscard]] SourceCollectionSessionAction LoadActiveSourceAt(std::size_t spectrum_index);
    void RememberActiveSourceIndex(std::size_t spectrum_index);

private:
    struct AddOrUpdateSourceResult {
        std::size_t source_index = 0;
        SpectrumSnapshotHandle replaced_cached_snapshot;
    };

    struct SourceListEntry {
        std::filesystem::path path;
        std::string key;
        std::string display_name;
        std::string type_label;
        std::string state_label;
        // Stores the last domain snapshot for this source so reactivation can use
        // an explicit cache instead of reloading. Do not remove as a summary-only
        // optimization without retesting CSV/folder error snapshots: that change
        // reproduced 0xc0000005 shared_ptr refcount crashes.
        SpectrumSnapshotHandle cached_snapshot;
        std::size_t last_spectrum_index = 0;
    };

    [[nodiscard]] const SourceListEntry* current_source() const;
    [[nodiscard]] AddOrUpdateSourceResult AddOrUpdateSource(
        const std::filesystem::path& path,
        SpectrumSnapshotHandle snapshot,
        std::size_t spectrum_index);
    void SetSnapshot(SpectrumSnapshotHandle snapshot, SourceCollectionSessionAction& action);

    SnapshotLoader snapshot_loader_;
    SpectrumSnapshotHandle snapshot_;
    std::vector<SourceListEntry> sources_;
    std::optional<std::size_t> current_source_index_;
};

}  // namespace specforge
