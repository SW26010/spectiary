#include "ui/source_collection_roster.h"

#include "domain/spectrum_fixture.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

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

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string SourceKey(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(path, error);
    return LowerAscii(PathToUtf8(error ? path : absolute_path));
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

std::string_view SourceStateLabel(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        return "none";
    }
    if (HasDiagnosticAtLeast(snapshot, SpectrumDiagnosticSeverity::Error)) {
        return "error";
    }
    if (snapshot->capabilities.can_plot_current_spectrum) {
        return snapshot->diagnostics.empty() ? "loaded" : "loaded with diagnostics";
    }
    return "not plottable";
}

std::string SnapshotDisplayNameText(const SpectrumSnapshotHandle& snapshot, const std::filesystem::path& path)
{
    if (snapshot && !snapshot->source.display_name.empty()) {
        return snapshot->source.display_name;
    }
    return FileNameToUtf8(path);
}

std::string SnapshotTypeLabelText(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        return "unknown";
    }

    const std::string_view format = MetadataValue(snapshot->source.metadata, "format");
    if (!format.empty()) {
        return std::string{format};
    }

    const std::string_view source_type = MetadataValue(snapshot->source.metadata, "source_type");
    return source_type.empty() ? std::string{"unknown"} : std::string{source_type};
}

std::string SnapshotStateLabelText(const SpectrumSnapshotHandle& snapshot)
{
    return std::string{SourceStateLabel(snapshot)};
}

}  // namespace

SourceCollectionRoster::SourceCollectionRoster(SnapshotLoader snapshot_loader)
    : snapshot_loader_(std::move(snapshot_loader)),
      snapshot_(MakeSmallSyntheticSpectrumSnapshot())
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
        source_view.type_label = entry.type_label;
        source_view.state_label = entry.state_label;
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

SourceCollectionSessionAction SourceCollectionRoster::OpenSource(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    SourceCollectionSessionAction action;
    SpectrumSnapshotHandle loaded_snapshot = snapshot_loader_(path, spectrum_index);
    const std::size_t source_index = AddOrUpdateSource(path, loaded_snapshot, spectrum_index);
    current_source_index_ = source_index;
    SetSnapshot(std::move(loaded_snapshot), action);
    return action;
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
    result.removed_source_key = sources_[source_index].key;
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

SourceCollectionSessionAction SourceCollectionRoster::LoadActiveSourceAt(std::size_t spectrum_index)
{
    SourceCollectionSessionAction action;
    const SourceListEntry* source = current_source();
    if (source == nullptr || source->path.empty()) {
        return action;
    }

    const std::filesystem::path path = source->path;
    SpectrumSnapshotHandle loaded_snapshot = snapshot_loader_(path, spectrum_index);
    const std::size_t source_index = AddOrUpdateSource(path, loaded_snapshot, spectrum_index);
    current_source_index_ = source_index;
    SetSnapshot(std::move(loaded_snapshot), action);
    return action;
}

const SourceCollectionRoster::SourceListEntry* SourceCollectionRoster::current_source() const
{
    if (!current_source_index_ || *current_source_index_ >= sources_.size()) {
        return nullptr;
    }
    return &sources_[*current_source_index_];
}

std::size_t SourceCollectionRoster::AddOrUpdateSource(
    const std::filesystem::path& path,
    SpectrumSnapshotHandle snapshot,
    std::size_t spectrum_index)
{
    const std::string key = SourceKey(path);
    const auto match = std::find_if(sources_.begin(), sources_.end(), [&key](const SourceListEntry& entry) {
        return entry.key == key;
    });
    if (match != sources_.end()) {
        match->path = path;
        match->display_name = SnapshotDisplayNameText(snapshot, path);
        match->type_label = SnapshotTypeLabelText(snapshot);
        match->state_label = SnapshotStateLabelText(snapshot);
        match->cached_snapshot = std::move(snapshot);
        match->last_spectrum_index = spectrum_index;
        return static_cast<std::size_t>(std::distance(sources_.begin(), match));
    }

    SourceListEntry entry;
    entry.path = path;
    entry.key = key;
    entry.display_name = SnapshotDisplayNameText(snapshot, path);
    entry.type_label = SnapshotTypeLabelText(snapshot);
    entry.state_label = SnapshotStateLabelText(snapshot);
    entry.cached_snapshot = std::move(snapshot);
    entry.last_spectrum_index = spectrum_index;
    sources_.push_back(std::move(entry));
    return sources_.size() - 1;
}

void SourceCollectionRoster::SetSnapshot(SpectrumSnapshotHandle snapshot, SourceCollectionSessionAction& action)
{
    snapshot_ = std::move(snapshot);
    action.snapshot_changed = true;
}

}  // namespace specforge
