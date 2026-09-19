#include "overlays/built_in_spectral_line_adapter.h"
#include "platform/exclusive_file_lease.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace spectiary {
BuiltInSpectralLineAdapter::BuiltInSpectralLineAdapter(SpectralLineListParseResult parsed, std::filesystem::path state_path)
    : state_path_(std::move(state_path)), base_error_(std::move(parsed.error))
{
    if (!parsed.list) return;
    base_ = std::move(*parsed.list);
    if (!ValidateSpectralLineList(base_, base_error_)) return;
    if (!state_path_.empty()) {
        auto loaded = LoadBuiltInSpectralLineState(state_path_, base_);
        state_ = std::move(loaded.state);
        load_error_ = std::move(loaded.error); load_issue_ = loaded.issue_kind;
        requires_save_ = loaded.requires_save;
    }
    Refresh();
    reconciliation_base_ = state_;
}
void BuiltInSpectralLineAdapter::Refresh()
{
    auto composed = ComposeBuiltInSpectralLineList(base_, state_.overlay);
    if (composed.list) effective_ = std::move(*composed.list);
    NormalizeSession();
}
void BuiltInSpectralLineAdapter::NormalizeSession() { NormalizeSpectralLineSession(state_.session, effective_); }
bool BuiltInSpectralLineAdapter::IsBaseView(std::string_view id) const
{
    return std::any_of(base_.grouping_views.begin(), base_.grouping_views.end(), [&](const auto& v) { return v.id == id; });
}
bool BuiltInSpectralLineAdapter::CanEditView(std::string_view id) const
{
    return base_error_.empty() && !IsBaseView(id) &&
        std::any_of(effective_.grouping_views.begin(), effective_.grouping_views.end(), [&](const auto& v) { return v.id == id; });
}
bool BuiltInSpectralLineAdapter::PutView(line_list::GroupingView view, std::string& error)
{
    if (!base_error_.empty()) { error = base_error_; return false; }
    if (!ReplaceBuiltInGroupingView(base_, state_.overlay, std::move(view), error)) return false;
    Refresh(); return true;
}
bool BuiltInSpectralLineAdapter::DeleteView(std::string_view id, std::string& error)
{
    if (!RemoveBuiltInGroupingView(base_, state_.overlay, id, error)) return false;
    Refresh(); return true;
}
PlotSeriesColor BuiltInSpectralLineAdapter::Color(std::string_view id) const
{
    for (const auto& scheme : effective_.color_schemes) {
        if (scheme.id != state_.session.active_color_scheme_id) continue;
        const auto found = scheme.colors.find(std::string(id));
        if (found != scheme.colors.end()) return DecodeLineListColor(found->second);
    }
    return PlotSeriesColor::Auto();
}
bool BuiltInSpectralLineAdapter::SetColor(std::string_view id, PlotSeriesColor color, std::string& error)
{
    if (!base_error_.empty()) { error = base_error_; return false; }
    if (color.explicit_color() && !IsValidRgbaColor(*color.explicit_color())) { error = "invalid marker color"; return false; }
    auto candidate = state_.overlay;
    std::string scheme_id = state_.session.active_color_scheme_id;
    if (scheme_id.empty()) {
        if (!color.explicit_color()) return true;
        scheme_id = "builtin.user-colors";
        candidate.color_schemes = std::vector<line_list::ColorScheme>{{scheme_id, "Custom colors", {}}};
    }
    const auto rgba = color.explicit_color() ? std::optional<std::string>(EncodeLineListColor(*color.explicit_color())) : std::nullopt;
    if (!SetBuiltInMarkerColor(base_, candidate, scheme_id, id, rgba, error)) return false;
    if (candidate == state_.overlay) return true;
    state_.overlay = std::move(candidate);
    intent_.marker_colors.emplace(scheme_id, id);
    state_.session.active_color_scheme_id = scheme_id;
    intent_.color_selection = true;
    Refresh(); return true;
}
bool BuiltInSpectralLineAdapter::ReplaceColors(std::optional<std::vector<line_list::ColorScheme>> schemes, std::string& error)
{
    if (!base_error_.empty()) { error = base_error_; return false; }
    if (!ReplaceBuiltInColorSchemes(base_, state_.overlay, std::move(schemes), error)) return false;
    intent_.whole_color_collection = true;
    intent_.marker_colors.clear();
    Refresh();
    return true;
}
bool BuiltInSpectralLineAdapter::Save(std::string& error)
{
    if (!base_error_.empty()) { error = base_error_; return false; }
    if (state_path_.empty()) { error = "spectral-line state path is empty"; return false; }
    auto lock_path = state_path_; lock_path += ".commit.lock";
    std::optional<ExclusiveFileLease> lease;
    for (int attempt = 0; attempt < 200; ++attempt) {
        auto acquired = TryAcquireExclusiveFileLease(lock_path);
        if (acquired.status == ExclusiveFileLeaseAcquireStatus::Acquired) { lease = std::move(acquired.lease); break; }
        if (acquired.status == ExclusiveFileLeaseAcquireStatus::Failed) { error = acquired.error; return false; }
        if (attempt < 199) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!lease) { error = "spectral-line state commit lease remained busy"; return false; }
    auto latest = LoadBuiltInSpectralLineState(state_path_, base_);
    if (latest.issue_kind != VersionedJsonCacheLoadIssueKind::None) { error = "latest durable state is not trusted: " + latest.error; return false; }
    BuiltInSpectralLineState merged;
    if (!ReconcileBuiltInSpectralLineTask(base_, reconciliation_base_, state_, latest.state, intent_, merged, error)) return false;
    if (!SaveBuiltInSpectralLineState(state_path_, base_, merged, error)) return false;
    state_ = std::move(merged); Refresh(); reconciliation_base_ = state_; intent_ = {};
    requires_save_ = false; load_issue_ = VersionedJsonCacheLoadIssueKind::None; load_error_.clear();
    return true;
}
} // namespace spectiary
