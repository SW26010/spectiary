#pragma once

#include "overlays/spectral_line_user_state.h"

#include <optional>
#include <string>
#include <unordered_set>

namespace specforge {

// Reconciles one controller task's catalog state against the durable state
// observed immediately before that task started.  The result deliberately
// covers only catalog user state and its panel expansion state; labeling
// leases and unrelated local caches have different ownership contracts.
struct CatalogUserStateReconciliationResult {
    CatalogUserState state;
    CatalogPanelState panel_state;
};

// The merge is field/entity based.  Local task changes win on the same scalar
// field, local deletions win over concurrent edits, and additions with an
// already-used id are retained under a deterministic fresh id.  Unchanged
// local fields come from latest, so a stale controller cannot erase unrelated
// durable changes. The optional selection provenance is supplied by the
// controller as task intent provenance, including an explicit round-trip to
// the base value. Automatic fallback is not granted task ownership. The
// ordering-view set similarly records views for which the task explicitly
// reordered groups, including newly added groups.
[[nodiscard]] bool ReconcileCatalogUserStateTask(
    const CatalogUserState& base,
    const CatalogUserState& local,
    const CatalogUserState& latest,
    const CatalogPanelState& base_panel_state,
    const CatalogPanelState& local_panel_state,
    const CatalogPanelState& latest_panel_state,
    CatalogUserStateReconciliationResult& result,
    std::string& diagnostic,
    std::optional<bool> local_selection_is_explicit = std::nullopt,
    std::unordered_set<std::string> local_group_ordering_view_ids = {});

}  // namespace specforge
