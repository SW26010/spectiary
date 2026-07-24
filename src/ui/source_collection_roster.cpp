#include "ui/source_collection_roster.h"

#include "domain/source_path_identity.h"
#include "domain/spectrum_fixture.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace specforge {
namespace {

constexpr std::size_t kMaximumResidentSnapshotCount = 8;
constexpr std::size_t kMaximumResidentSnapshotPayloadBytes =
    128U * 1024U * 1024U;

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string FileNameToUtf8(const std::filesystem::path& path)
{
    const std::filesystem::path filename = path.filename();
    return filename.empty() ? PathToUtf8(path) : PathToUtf8(filename);
}

std::string_view MetadataValue(const std::vector<SpectrumMetadataEntry>& metadata, std::string_view key)
{
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        if (metadata[index].key == key) {
            return metadata[index].value;
        }
    }
    return {};
}

bool HasDiagnosticAtLeast(const SpectrumSnapshotHandle& snapshot, SpectrumDiagnosticSeverity minimum)
{
    if (!snapshot) {
        return false;
    }
    const auto rank = [](SpectrumDiagnosticSeverity severity) {
        switch (severity) {
        case SpectrumDiagnosticSeverity::Error:
            return 2;
        case SpectrumDiagnosticSeverity::Warning:
            return 1;
        case SpectrumDiagnosticSeverity::Info:
        default:
            return 0;
        }
    };
    const int minimum_rank = rank(minimum);
    return std::any_of(snapshot->diagnostics.begin(), snapshot->diagnostics.end(), [rank, minimum_rank](const auto& d) {
        return rank(d.severity) >= minimum_rank;
    });
}

SourceCollectionSourceState SourceState(
    const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        return SourceCollectionSourceState::Unavailable;
    }
    if (HasDiagnosticAtLeast(snapshot, SpectrumDiagnosticSeverity::Error)) {
        return SourceCollectionSourceState::Error;
    }
    if (snapshot->capabilities.can_plot_current_spectrum) {
        return snapshot->diagnostics.empty()
            ? SourceCollectionSourceState::Loaded
            : SourceCollectionSourceState::LoadedWithDiagnostics;
    }
    return SourceCollectionSourceState::NotPlottable;
}

std::string SnapshotDisplayNameText(const SpectrumSnapshotHandle& snapshot, const std::filesystem::path& path)
{
    if (snapshot && !snapshot->source.display_name.empty()) {
        return snapshot->source.display_name;
    }
    return FileNameToUtf8(path);
}

std::optional<std::string> SnapshotType(
    const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        return std::nullopt;
    }

    const std::string_view format = MetadataValue(snapshot->source.metadata, "format");
    if (!format.empty()) {
        return std::string{format};
    }

    const std::string_view source_type = MetadataValue(snapshot->source.metadata, "source_type");
    return source_type.empty()
        ? std::nullopt
        : std::optional<std::string>{source_type};
}

std::size_t EstimatedSnapshotPayloadBytes(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        return 0;
    }
    std::size_t bytes = 0;
    if (snapshot->current_spectrum.x_values) {
        bytes += snapshot->current_spectrum.x_values->size() * sizeof(double);
    }
    if (snapshot->current_spectrum.y_values &&
        snapshot->current_spectrum.y_values !=
            snapshot->current_spectrum.x_values) {
        bytes += snapshot->current_spectrum.y_values->size() * sizeof(double);
    }
    return bytes;
}

bool ResidencyBoundariesMatch(
    const std::optional<SourceCollectionContextReuseProof>& left_proof,
    const SourceCollectionFolderListingGenerationHandle& left_generation,
    const std::optional<SourceCollectionContextReuseProof>& right_proof,
    const SourceCollectionFolderListingGenerationHandle& right_generation)
{
    return left_proof && right_proof &&
           left_proof->identity == right_proof->identity &&
           left_proof->dependency_state == right_proof->dependency_state &&
           left_generation == right_generation;
}

}  // namespace

SourceCollectionRoster::SourceCollectionRoster()
    : snapshot_(MakeSmallSyntheticSpectrumSnapshot())
{
}

const SpectrumSnapshotHandle& SourceCollectionRoster::snapshot() const
{
    return snapshot_;
}

std::optional<std::size_t> SourceCollectionRoster::current_source_index() const
{
    return current_source_index_;
}

bool SourceCollectionRoster::has_source(std::size_t source_index) const
{
    return source_index < sources_.size();
}

std::optional<std::string> SourceCollectionRoster::current_source_key() const
{
    const SourceListEntry* source = current_source();
    if (source == nullptr) {
        return std::nullopt;
    }
    return source->key;
}

bool SourceCollectionRoster::has_active_source() const
{
    return current_source() != nullptr && snapshot_ && !snapshot_->source.path.empty();
}

std::vector<SourceCollectionSourceView> SourceCollectionRoster::SourceViews() const
{
    std::vector<SourceCollectionSourceView> views;
    views.reserve(sources_.size());
    for (const SourceListEntry& entry : sources_) {
        SourceCollectionSourceView source_view;
        source_view.path = entry.path;
        source_view.display_name = entry.display_name;
        source_view.type = entry.type;
        source_view.state = entry.state;
        views.push_back(std::move(source_view));
    }
    return views;
}

std::vector<SourceCollectionSavedSource> SourceCollectionRoster::SavedSources() const
{
    std::vector<SourceCollectionSavedSource> sources;
    sources.reserve(sources_.size());
    for (const SourceListEntry& entry : sources_) {
        SourceCollectionSavedSource source;
        source.path = entry.path;
        source.last_spectrum_index = entry.last_spectrum_index;
        sources.push_back(std::move(source));
    }
    return sources;
}

std::vector<std::string> SourceCollectionRoster::SavedSourceKeys() const
{
    std::vector<std::string> keys;
    keys.reserve(sources_.size());
    for (const SourceListEntry& entry : sources_) {
        keys.push_back(entry.key);
    }
    return keys;
}

SourceCollectionFolderListingGenerationHandle SourceCollectionRoster::FolderListingGeneration(
    const std::filesystem::path& path) const
{
    const std::string key = SourcePathIdentityKey(path);
    const auto match = std::find_if(
        sources_.begin(),
        sources_.end(),
        [&key](const SourceListEntry& entry) { return entry.key == key; });
    return match == sources_.end()
        ? SourceCollectionFolderListingGenerationHandle{}
        : match->folder_listing_generation;
}

std::optional<SourceCollectionContextReuseProof>
SourceCollectionRoster::ContextReuseProof(
    const std::filesystem::path& path) const
{
    const std::string key = SourcePathIdentityKey(path);
    const auto match = std::find_if(
        sources_.begin(),
        sources_.end(),
        [&key](const SourceListEntry& entry) { return entry.key == key; });
    return match == sources_.end()
        ? std::optional<SourceCollectionContextReuseProof>{}
        : match->context_reuse_proof;
}

std::optional<SourceCollectionResidentSnapshot>
SourceCollectionRoster::ResidentSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SourceCollectionIdentity& identity)
{
    const std::string key = SourcePathIdentityKey(path);
    const auto source = std::find_if(
        sources_.begin(),
        sources_.end(),
        [&key](const SourceListEntry& entry) { return entry.key == key; });
    if (source == sources_.end()) {
        return std::nullopt;
    }

    if (source->cached_snapshot &&
        source->last_spectrum_index == spectrum_index &&
        source->context_reuse_proof &&
        source->context_reuse_proof->identity == identity) {
        return SourceCollectionResidentSnapshot{
            spectrum_index,
            source->cached_snapshot,
            *source->context_reuse_proof,
            source->folder_listing_generation,
            SourceCollectionResidentSnapshotOrigin::History,
        };
    }

    const auto resident = std::find_if(
        source->resident_snapshots.begin(),
        source->resident_snapshots.end(),
        [spectrum_index, &identity](const ResidentSnapshotEntry& entry) {
            return entry.spectrum_index == spectrum_index &&
                   entry.context_reuse_proof.identity == identity;
        });
    if (resident == source->resident_snapshots.end()) {
        return std::nullopt;
    }
    resident->access_epoch = ++resident_access_epoch_;
    return SourceCollectionResidentSnapshot{
        resident->spectrum_index,
        resident->snapshot,
        resident->context_reuse_proof,
        resident->folder_listing_generation,
        resident->origin,
        resident->prefetch_id,
        resident->prefetch_task_id,
        resident->prefetch_scheduled_ns,
        resident->prefetch_direction,
    };
}

SourceCollectionRosterResidentRetainResult
SourceCollectionRoster::RetainPrefetchedSnapshot(
    const std::filesystem::path& path,
    SourceCollectionResidentSnapshot resident)
{
    SourceCollectionRosterResidentRetainResult result;
    if (!resident.snapshot ||
        resident.origin != SourceCollectionResidentSnapshotOrigin::Prefetch ||
        resident.prefetch_id == 0 || resident.prefetch_task_id == 0 ||
        resident.snapshot->collection.current_index !=
            resident.spectrum_index ||
        SourcePathIdentityKey(resident.snapshot->source.path) !=
            SourcePathIdentityKey(path)) {
        if (resident.snapshot) {
            result.retired_snapshots.push_back(
                std::move(resident.snapshot));
        }
        return result;
    }

    const std::string key = SourcePathIdentityKey(path);
    const auto source = std::find_if(
        sources_.begin(),
        sources_.end(),
        [&key](const SourceListEntry& entry) {
            return entry.key == key;
        });
    if (source == sources_.end() || !current_source_index_ ||
        *current_source_index_ !=
            static_cast<std::size_t>(
                std::distance(sources_.begin(), source)) ||
        !ResidencyBoundariesMatch(
            source->context_reuse_proof,
            source->folder_listing_generation,
            resident.context_reuse_proof,
            resident.folder_listing_generation) ||
        (source->cached_snapshot &&
         source->last_spectrum_index == resident.spectrum_index)) {
        result.retired_snapshots.push_back(
            std::move(resident.snapshot));
        return result;
    }

    const std::size_t spectrum_index = resident.spectrum_index;
    const std::uint64_t prefetch_id = resident.prefetch_id;
    RetainPreviousSnapshot(
        *source,
        std::move(resident.snapshot),
        resident.spectrum_index,
        resident.context_reuse_proof,
        resident.folder_listing_generation,
        resident.origin,
        resident.prefetch_id,
        resident.prefetch_task_id,
        resident.prefetch_scheduled_ns,
        resident.prefetch_direction,
        result.retired_snapshots);
    EvictResidentSnapshots(result.retired_snapshots);
    result.retained = std::any_of(
        source->resident_snapshots.begin(),
        source->resident_snapshots.end(),
        [spectrum_index, prefetch_id](
            const ResidentSnapshotEntry& entry) {
            return entry.spectrum_index == spectrum_index &&
                   entry.prefetch_id == prefetch_id;
        });
    return result;
}

SourceCollectionRosterOpenResult SourceCollectionRoster::OpenPreparedSource(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    SpectrumSnapshotHandle snapshot,
    SourceCollectionFolderListingGenerationHandle folder_listing_generation,
    std::optional<SourceCollectionContextReuseProof> context_reuse_proof)
{
    SourceCollectionRosterOpenResult result;
    AddOrUpdateSourceResult update = AddOrUpdateSource(
        path,
        snapshot,
        spectrum_index,
        std::move(folder_listing_generation),
        std::move(context_reuse_proof));
    current_source_index_ = update.source_index;
    result.retired_snapshots = std::move(update.retired_snapshots);
    result.replaced_folder_listing_generation =
        std::move(update.replaced_folder_listing_generation);
    SetSnapshot(std::move(snapshot), result.action);
    return result;
}

SourceCollectionSessionAction SourceCollectionRoster::ActivateSource(std::size_t source_index)
{
    SourceCollectionSessionAction action;
    if (source_index >= sources_.size()) {
        return action;
    }

    SourceListEntry& entry = sources_[source_index];
    current_source_index_ = source_index;
    SetSnapshot(entry.cached_snapshot, action);
    return action;
}

SourceCollectionRosterRemoveResult SourceCollectionRoster::RemoveSource(std::size_t source_index)
{
    SourceCollectionRosterRemoveResult result;
    if (source_index >= sources_.size()) {
        return result;
    }

    result.removed = true;
    result.removed_current = current_source_index_ && *current_source_index_ == source_index;
    result.removed_path = sources_[source_index].path;
    result.removed_source_key = sources_[source_index].key;
    if (sources_[source_index].cached_snapshot) {
        result.retired_snapshots.push_back(std::move(sources_[source_index].cached_snapshot));
    }
    InvalidateResidentSnapshots(
        sources_[source_index],
        result.retired_snapshots);
    result.retired_folder_listing_generation =
        std::move(sources_[source_index].folder_listing_generation);
    if (result.removed_current && snapshot_) {
        result.retired_snapshots.push_back(std::move(snapshot_));
    }
    std::optional<std::size_t> next_current_index;
    if (result.removed_current && sources_.size() > 1) {
        next_current_index = source_index + 1 < sources_.size() ? source_index : source_index - 1;
    }

    sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(source_index));

    if (result.removed_current) {
        current_source_index_.reset();
        if (next_current_index) {
            MergeSourceCollectionSessionAction(result.action, ActivateSource(*next_current_index));
        } else {
            SetSnapshot(MakeSmallSyntheticSpectrumSnapshot(), result.action);
            result.action.navigation_inputs_changed = true;
        }
        return result;
    }

    if (current_source_index_ && *current_source_index_ > source_index) {
        current_source_index_ = *current_source_index_ - 1;
    }
    return result;
}

void SourceCollectionRoster::RememberActiveSourceIndex(std::size_t spectrum_index)
{
    if (!current_source_index_ || *current_source_index_ >= sources_.size()) {
        return;
    }
    sources_[*current_source_index_].last_spectrum_index = spectrum_index;
}

const SourceCollectionRoster::SourceListEntry* SourceCollectionRoster::current_source() const
{
    if (!current_source_index_ || *current_source_index_ >= sources_.size()) {
        return nullptr;
    }
    return &sources_[*current_source_index_];
}

SourceCollectionRoster::AddOrUpdateSourceResult SourceCollectionRoster::AddOrUpdateSource(
    const std::filesystem::path& path,
    SpectrumSnapshotHandle snapshot,
    std::size_t spectrum_index,
    SourceCollectionFolderListingGenerationHandle folder_listing_generation,
    std::optional<SourceCollectionContextReuseProof> context_reuse_proof)
{
    AddOrUpdateSourceResult result;
    const std::string key = SourcePathIdentityKey(path);
    const auto match = std::find_if(sources_.begin(), sources_.end(), [&key](const SourceListEntry& entry) {
        return entry.key == key;
    });
    if (match != sources_.end()) {
        match->path = path;
        match->display_name = SnapshotDisplayNameText(snapshot, path);
        match->type = SnapshotType(snapshot);
        match->state = SourceState(snapshot);
        const bool same_residency_boundary = ResidencyBoundariesMatch(
            match->context_reuse_proof,
            match->folder_listing_generation,
            context_reuse_proof,
            folder_listing_generation);
        if (!same_residency_boundary) {
            InvalidateResidentSnapshots(
                *match,
                result.retired_snapshots);
        } else {
            const auto resident = std::find_if(
                match->resident_snapshots.begin(),
                match->resident_snapshots.end(),
                [spectrum_index](const ResidentSnapshotEntry& entry) {
                    return entry.spectrum_index == spectrum_index;
                });
            if (resident != match->resident_snapshots.end()) {
                --resident_snapshot_count_;
                resident_snapshot_payload_bytes_ -=
                    resident->estimated_payload_bytes;
                if (resident->snapshot != snapshot) {
                    result.retired_snapshots.push_back(
                        std::move(resident->snapshot));
                }
                match->resident_snapshots.erase(resident);
            }
            if (match->cached_snapshot &&
                match->last_spectrum_index != spectrum_index) {
                RetainPreviousSnapshot(
                    *match,
                    std::move(match->cached_snapshot),
                    match->last_spectrum_index,
                    match->context_reuse_proof,
                    match->folder_listing_generation,
                    SourceCollectionResidentSnapshotOrigin::History,
                    0,
                    0,
                    0,
                    SampleNavigationDirection::Next,
                    result.retired_snapshots);
            }
        }
        if (match->cached_snapshot && match->cached_snapshot != snapshot) {
            result.retired_snapshots.push_back(
                std::move(match->cached_snapshot));
        }
        match->cached_snapshot = std::move(snapshot);
        result.replaced_folder_listing_generation =
            std::move(match->folder_listing_generation);
        match->folder_listing_generation = std::move(folder_listing_generation);
        match->context_reuse_proof = std::move(context_reuse_proof);
        match->last_spectrum_index = spectrum_index;
        EvictResidentSnapshots(result.retired_snapshots);
        result.source_index = static_cast<std::size_t>(std::distance(sources_.begin(), match));
        return result;
    }

    SourceListEntry entry;
    entry.path = path;
    entry.key = key;
    entry.display_name = SnapshotDisplayNameText(snapshot, path);
    entry.type = SnapshotType(snapshot);
    entry.state = SourceState(snapshot);
    entry.cached_snapshot = std::move(snapshot);
    entry.folder_listing_generation = std::move(folder_listing_generation);
    entry.context_reuse_proof = std::move(context_reuse_proof);
    entry.last_spectrum_index = spectrum_index;
    sources_.push_back(std::move(entry));
    result.source_index = sources_.size() - 1;
    return result;
}

void SourceCollectionRoster::RetainPreviousSnapshot(
    SourceListEntry& source,
    SpectrumSnapshotHandle snapshot,
    std::size_t spectrum_index,
    const std::optional<SourceCollectionContextReuseProof>& context_reuse_proof,
    const SourceCollectionFolderListingGenerationHandle&
        folder_listing_generation,
    SourceCollectionResidentSnapshotOrigin origin,
    std::uint64_t prefetch_id,
    std::uint64_t prefetch_task_id,
    std::int64_t prefetch_scheduled_ns,
    SampleNavigationDirection prefetch_direction,
    std::vector<SpectrumSnapshotHandle>& retired_snapshots)
{
    if (!snapshot || !context_reuse_proof) {
        if (snapshot) {
            retired_snapshots.push_back(std::move(snapshot));
        }
        return;
    }

    const auto existing = std::find_if(
        source.resident_snapshots.begin(),
        source.resident_snapshots.end(),
        [spectrum_index](const ResidentSnapshotEntry& entry) {
            return entry.spectrum_index == spectrum_index;
        });
    if (existing != source.resident_snapshots.end()) {
        --resident_snapshot_count_;
        resident_snapshot_payload_bytes_ -=
            existing->estimated_payload_bytes;
        if (existing->snapshot != snapshot) {
            retired_snapshots.push_back(std::move(existing->snapshot));
        }
        source.resident_snapshots.erase(existing);
    }

    ResidentSnapshotEntry resident;
    resident.spectrum_index = spectrum_index;
    resident.estimated_payload_bytes =
        EstimatedSnapshotPayloadBytes(snapshot);
    resident.snapshot = std::move(snapshot);
    resident.context_reuse_proof = *context_reuse_proof;
    resident.folder_listing_generation = folder_listing_generation;
    resident.origin = origin;
    resident.prefetch_id = prefetch_id;
    resident.prefetch_task_id = prefetch_task_id;
    resident.prefetch_scheduled_ns = prefetch_scheduled_ns;
    resident.prefetch_direction = prefetch_direction;
    resident.access_epoch = ++resident_access_epoch_;
    resident_snapshot_payload_bytes_ +=
        resident.estimated_payload_bytes;
    ++resident_snapshot_count_;
    source.resident_snapshots.push_back(std::move(resident));
}

void SourceCollectionRoster::InvalidateResidentSnapshots(
    SourceListEntry& source,
    std::vector<SpectrumSnapshotHandle>& retired_snapshots)
{
    for (ResidentSnapshotEntry& resident : source.resident_snapshots) {
        --resident_snapshot_count_;
        resident_snapshot_payload_bytes_ -=
            resident.estimated_payload_bytes;
        if (resident.snapshot) {
            retired_snapshots.push_back(std::move(resident.snapshot));
        }
    }
    source.resident_snapshots.clear();
}

void SourceCollectionRoster::EvictResidentSnapshots(
    std::vector<SpectrumSnapshotHandle>& retired_snapshots)
{
    while (resident_snapshot_count_ > kMaximumResidentSnapshotCount ||
           resident_snapshot_payload_bytes_ >
               kMaximumResidentSnapshotPayloadBytes) {
        SourceListEntry* oldest_source = nullptr;
        std::vector<ResidentSnapshotEntry>::iterator oldest;
        std::uint64_t oldest_epoch =
            std::numeric_limits<std::uint64_t>::max();
        for (SourceListEntry& source : sources_) {
            const auto candidate = std::min_element(
                source.resident_snapshots.begin(),
                source.resident_snapshots.end(),
                [](const ResidentSnapshotEntry& left,
                   const ResidentSnapshotEntry& right) {
                    return left.access_epoch < right.access_epoch;
                });
            if (candidate != source.resident_snapshots.end() &&
                candidate->access_epoch < oldest_epoch) {
                oldest_source = &source;
                oldest = candidate;
                oldest_epoch = candidate->access_epoch;
            }
        }
        if (oldest_source == nullptr) {
            resident_snapshot_count_ = 0;
            resident_snapshot_payload_bytes_ = 0;
            return;
        }

        --resident_snapshot_count_;
        resident_snapshot_payload_bytes_ -=
            oldest->estimated_payload_bytes;
        if (oldest->snapshot) {
            retired_snapshots.push_back(
                std::move(oldest->snapshot));
        }
        oldest_source->resident_snapshots.erase(oldest);
    }
}

void SourceCollectionRoster::SetSnapshot(SpectrumSnapshotHandle snapshot, SourceCollectionSessionAction& action)
{
    snapshot_ = std::move(snapshot);
    action.snapshot_changed = true;
}

}  // namespace specforge
