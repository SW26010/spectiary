#pragma once
#include "overlays/built_in_spectral_line_state_io.h"
#include "overlays/built_in_spectral_line_reconciliation.h"

namespace spectiary {
// Owns the built-in persistence boundary. Consumers see complete v1 content.
class BuiltInSpectralLineAdapter {
public:
    BuiltInSpectralLineAdapter(SpectralLineListParseResult base, std::filesystem::path state_path);
    [[nodiscard]] const SpectralLineList& effective() const { return effective_; }
    [[nodiscard]] const std::string& base_error() const { return base_error_; }
    [[nodiscard]] const std::string& load_error() const { return load_error_; }
    [[nodiscard]] VersionedJsonCacheLoadIssueKind load_issue() const { return load_issue_; }
    [[nodiscard]] bool requires_save() const { return requires_save_; }
    [[nodiscard]] bool CanEditView(std::string_view id) const;
    [[nodiscard]] bool IsBaseView(std::string_view id) const;
    [[nodiscard]] SpectralLineSessionState& session() { return state_.session; }
    [[nodiscard]] const SpectralLineSessionState& session() const { return state_.session; }
    [[nodiscard]] BuiltInSpectralLineTaskIntent& intent() { return intent_; }
    [[nodiscard]] bool PutView(line_list::GroupingView view, std::string& error);
    [[nodiscard]] bool DeleteView(std::string_view id, std::string& error);
    [[nodiscard]] bool SetColor(std::string_view marker_id, PlotSeriesColor color, std::string& error);
    [[nodiscard]] bool ReplaceColors(std::optional<std::vector<line_list::ColorScheme>> schemes, std::string& error);
    [[nodiscard]] PlotSeriesColor Color(std::string_view marker_id) const;
    [[nodiscard]] bool Save(std::string& error);
    void NormalizeSession();
private:
    void Refresh();
    SpectralLineList base_;
    SpectralLineList effective_;
    BuiltInSpectralLineState state_;
    BuiltInSpectralLineState reconciliation_base_;
    BuiltInSpectralLineTaskIntent intent_;
    std::filesystem::path state_path_;
    std::string base_error_, load_error_;
    VersionedJsonCacheLoadIssueKind load_issue_ = VersionedJsonCacheLoadIssueKind::None;
    bool requires_save_ = false;
};
} // namespace spectiary
