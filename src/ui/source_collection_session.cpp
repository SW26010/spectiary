#include "ui/source_collection_session.h"

#include "domain/sample_annotation_io.h"
#include "domain/spectrum_fixture.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

void MergeAction(SourceCollectionSessionAction& target, const SourceCollectionSessionAction& source)
{
    target.snapshot_changed = target.snapshot_changed || source.snapshot_changed;
    target.workflow_changed = target.workflow_changed || source.workflow_changed;
    target.navigation_inputs_changed = target.navigation_inputs_changed || source.navigation_inputs_changed;
}

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

bool ShouldRememberLabelingPosition(SampleNavigationRequestKind kind)
{
    return kind == SampleNavigationRequestKind::Previous || kind == SampleNavigationRequestKind::Next ||
           kind == SampleNavigationRequestKind::LabelAdvance;
}

}  // namespace

SourceCollectionSession::SourceCollectionSession(SnapshotLoader snapshot_loader)
    : snapshot_loader_(std::move(snapshot_loader)),
      snapshot_(MakeSmallSyntheticSpectrumSnapshot())
{
}

SourceCollectionSession::SourceCollectionSession(
    SnapshotLoader snapshot_loader,
    std::filesystem::path navigation_state_cache_path,
    std::filesystem::path labeling_state_cache_path)
    : navigation_(std::move(navigation_state_cache_path)),
      labeling_(std::move(labeling_state_cache_path)),
      snapshot_loader_(std::move(snapshot_loader)),
      snapshot_(MakeSmallSyntheticSpectrumSnapshot())
{
}

SourceCollectionSessionAction SourceCollectionSession::OpenSource(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    SourceCollectionSessionAction action;
    SpectrumSnapshotHandle loaded_snapshot = snapshot_loader_(path, spectrum_index);
    const std::size_t source_index = AddOrUpdateSource(path, loaded_snapshot, spectrum_index);
    current_source_index_ = source_index;
    SetSnapshot(std::move(loaded_snapshot), action);
    MergeAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::ActivateSource(std::size_t source_index)
{
    SourceCollectionSessionAction action;
    if (source_index >= sources_.size()) {
        return action;
    }

    SourceListEntry& entry = sources_[source_index];
    current_source_index_ = source_index;
    SetSnapshot(entry.cached_snapshot, action);
    MergeAction(action, EnsureSnapshotMatchesNavigation());
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::RemoveSource(std::size_t source_index)
{
    SourceCollectionSessionAction action;
    if (source_index >= sources_.size()) {
        return action;
    }

    const bool removed_current = current_source_index_ && *current_source_index_ == source_index;
    navigation_.RemoveSource(sources_[source_index].key);
    std::optional<std::size_t> next_current_index;
    if (removed_current && sources_.size() > 1) {
        next_current_index = source_index + 1 < sources_.size() ? source_index : source_index - 1;
    }

    sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(source_index));

    if (removed_current) {
        current_source_index_.reset();
        ClearSampleWorkflow(action);
        if (next_current_index) {
            MergeAction(action, ActivateSource(*next_current_index));
        } else {
            navigation_.ClearActiveSource();
            SetSnapshot(MakeSmallSyntheticSpectrumSnapshot(), action);
            action.navigation_inputs_changed = true;
        }
        return action;
    }

    if (current_source_index_ && *current_source_index_ > source_index) {
        current_source_index_ = *current_source_index_ - 1;
    }
    return action;
}

SourceCollectionNavigationAction SourceCollectionSession::RequestSampleNavigation(
    const SampleNavigationRequest& request)
{
    SourceCollectionNavigationAction action;
    action.navigation = navigation_.Navigate(request);
    if (action.navigation.has_active_source && action.navigation.target_found &&
        ShouldRememberLabelingPosition(request.kind)) {
        (void)labeling_.RememberActivePosition(action.navigation.current_index);
    }
    if (action.navigation.has_active_source && action.navigation.target_found) {
        if (!snapshot_ || snapshot_->collection.current_index != action.navigation.current_index) {
            MergeAction(action.action, LoadActiveSourceAt(action.navigation.current_index));
        } else {
            action.action.navigation_inputs_changed = true;
        }
    }
    return action;
}

bool SourceCollectionSession::AddReadOnlyAnnotationToActiveSource(
    const std::filesystem::path& path,
    std::string* message)
{
    const bool loaded = navigation_.AddReadOnlyAnnotationToActiveSource(path, message);
    ApplySampleFilters();
    return loaded;
}

void SourceCollectionSession::ApplySampleFilters()
{
    const std::size_t sample_count = ActiveSampleCount();
    if (sample_count == 0) {
        navigation_.ClearSampleFilter();
        return;
    }

    const SampleFilterEvaluation evaluation = filters_.Evaluate(BuildSampleFilterSources(), sample_count);
    if (evaluation.active) {
        navigation_.SetSampleFilter(evaluation.included_samples);
    } else {
        navigation_.ClearSampleFilter();
    }
}

SampleFilterEvaluation SourceCollectionSession::EvaluateSampleFilters() const
{
    const std::size_t sample_count = ActiveSampleCount();
    if (sample_count == 0) {
        return {};
    }
    return filters_.Evaluate(BuildSampleFilterSources(), sample_count);
}

void SourceCollectionSession::ClearFilters()
{
    filters_.Clear();
    ApplySampleFilters();
}

void SourceCollectionSession::SetFilterCondition(
    std::string source_id,
    std::unordered_set<std::string> allowed_value_keys)
{
    filters_.SetCondition(std::move(source_id), std::move(allowed_value_keys));
    ApplySampleFilters();
}

void SourceCollectionSession::SetActiveLabelingFilterSourceSelected(bool selected)
{
    const SampleLabelingTask* task = labeling_.active_task();
    if (task == nullptr) {
        if (selected_labeling_filter_source_id_) {
            filters_.ClearCondition(*selected_labeling_filter_source_id_);
        }
        selected_labeling_filter_source_id_.reset();
        ApplySampleFilters();
        return;
    }

    const SampleFilterSource active_labeling_source = BuildLabelingFilterSource(*task);
    if (selected_labeling_filter_source_id_ && *selected_labeling_filter_source_id_ != active_labeling_source.id) {
        filters_.ClearCondition(*selected_labeling_filter_source_id_);
    }

    if (selected) {
        selected_labeling_filter_source_id_ = active_labeling_source.id;
    } else {
        filters_.ClearCondition(active_labeling_source.id);
        selected_labeling_filter_source_id_.reset();
    }
    ApplySampleFilters();
}

bool SourceCollectionSession::active_labeling_filter_source_selected() const
{
    const SampleLabelingTask* task = labeling_.active_task();
    if (task == nullptr || !selected_labeling_filter_source_id_) {
        return false;
    }
    return *selected_labeling_filter_source_id_ == BuildLabelingFilterSource(*task).id;
}

std::vector<SampleFilterSource> SourceCollectionSession::BuildSampleFilterSources() const
{
    std::vector<SampleFilterSource> filter_sources;
    const SampleCollectionContext* context = navigation_.active_context();
    if (context != nullptr) {
        filter_sources.reserve(context->annotations.size() + 1);
        for (const SampleAnnotationResult& annotation : context->annotations) {
            filter_sources.push_back(BuildAnnotationFilterSource(annotation));
        }
    }

    if (const SampleLabelingTask* task = labeling_.active_task()) {
        SampleFilterSource labeling_source = BuildLabelingFilterSource(*task);
        if (selected_labeling_filter_source_id_ && *selected_labeling_filter_source_id_ == labeling_source.id) {
            filter_sources.push_back(std::move(labeling_source));
        }
    }
    return filter_sources;
}

bool SourceCollectionSession::can_add_read_only_annotation() const
{
    return navigation_.active_context() != nullptr && navigation_.spectrum_count().value_or(0) > 0;
}

void SourceCollectionSession::MaybeSaveStateCaches(std::uint64_t frame_index)
{
    labeling_.MaybeSaveStateCache(frame_index);
}

bool SourceCollectionSession::FlushStateCaches()
{
    return labeling_.FlushStateCache();
}

const SpectrumSnapshotHandle& SourceCollectionSession::snapshot() const
{
    return snapshot_;
}

const std::vector<SourceCollectionSession::SourceListEntry>& SourceCollectionSession::sources() const
{
    return sources_;
}

std::optional<std::size_t> SourceCollectionSession::current_source_index() const
{
    return current_source_index_;
}

const SourceCollectionSession::SourceListEntry* SourceCollectionSession::current_source() const
{
    if (!current_source_index_ || *current_source_index_ >= sources_.size()) {
        return nullptr;
    }
    return &sources_[*current_source_index_];
}

SampleNavigationController& SourceCollectionSession::navigation()
{
    return navigation_;
}

const SampleNavigationController& SourceCollectionSession::navigation() const
{
    return navigation_;
}

SampleLabelingController& SourceCollectionSession::labeling()
{
    return labeling_;
}

const SampleLabelingController& SourceCollectionSession::labeling() const
{
    return labeling_;
}

SampleFilterController& SourceCollectionSession::filters()
{
    return filters_;
}

const SampleFilterController& SourceCollectionSession::filters() const
{
    return filters_;
}

std::size_t SourceCollectionSession::AddOrUpdateSource(
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

SourceCollectionSessionAction SourceCollectionSession::EnsureSnapshotMatchesNavigation()
{
    SourceCollectionSessionAction action;
    SyncSampleNavigationSession(action);
    const std::optional<std::size_t> navigation_index = navigation_.current_index();
    if (navigation_index && snapshot_ && snapshot_->collection.spectrum_count > 0 &&
        snapshot_->collection.current_index != *navigation_index) {
        MergeAction(action, LoadActiveSourceAt(*navigation_index));
        return action;
    }
    action.navigation_inputs_changed = true;
    return action;
}

SourceCollectionSessionAction SourceCollectionSession::LoadActiveSourceAt(std::size_t spectrum_index)
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
    SyncSampleNavigationSession(action);
    action.navigation_inputs_changed = true;
    return action;
}

void SourceCollectionSession::SyncSampleNavigationSession(SourceCollectionSessionAction& action)
{
    const bool has_active_source =
        current_source_index_ && *current_source_index_ < sources_.size() && snapshot_ && !snapshot_->source.path.empty();
    if (!has_active_source) {
        navigation_.ClearActiveSource();
        ClearSampleWorkflow(action);
        return;
    }

    navigation_.ActivateSource(sources_[*current_source_index_].key, snapshot_);
    SyncSampleWorkflowSession(action);
    ApplySampleFilters();
}

void SourceCollectionSession::SyncSampleWorkflowSession(SourceCollectionSessionAction& action)
{
    if (!snapshot_ || snapshot_->source.path.empty() || snapshot_->collection.spectrum_count == 0) {
        ClearSampleWorkflow(action);
        return;
    }

    const SampleCollectionIdentity identity = BuildSampleCollectionIdentity(*snapshot_);
    if (!active_sample_workflow_identity_ || *active_sample_workflow_identity_ != identity.id) {
        filters_.Clear();
        selected_labeling_filter_source_id_.reset();
        active_sample_workflow_identity_ = identity.id;
        action.workflow_changed = true;
    }
    labeling_.ActivateSource(identity.id, identity.spectrum_count);
}

void SourceCollectionSession::ClearSampleWorkflow(SourceCollectionSessionAction& action)
{
    labeling_.ClearActiveSource();
    filters_.Clear();
    active_sample_workflow_identity_.reset();
    selected_labeling_filter_source_id_.reset();
    action.workflow_changed = true;
}

void SourceCollectionSession::SetSnapshot(SpectrumSnapshotHandle snapshot, SourceCollectionSessionAction& action)
{
    snapshot_ = std::move(snapshot);
    action.snapshot_changed = true;
}

std::size_t SourceCollectionSession::ActiveSampleCount() const
{
    return navigation_.spectrum_count().value_or(snapshot_ ? snapshot_->collection.spectrum_count : 0);
}

}  // namespace specforge
