#pragma once

#include "profile/navigation_latency_trace.h"
#include "ui/sample_navigation_controller.h"
#include "ui/source_collection_session.h"

#include <functional>
#include <optional>

namespace spectiary {

class SourceCollectionActivationTransaction;

// Shell-owned panel seam. Every mutation is followed by a projection read;
// actions and navigation latency semantics stay outside panel implementations.
class PanelSessionInteraction {
public:
    struct Update {
        SourceCollectionSessionResult result;
        // SourceCollectionSession retains invalidated projections through the
        // current presentation lifecycle, so this reference is frame-stable.
        std::reference_wrapper<const SourceCollectionSessionView> view;
    };

    using Submitter = std::function<SourceCollectionSessionResult(
        SourceCollectionSessionIntent,
        std::optional<NavigationLatencyInputKind>)>;
    using ViewReader =
        std::function<const SourceCollectionSessionView&()>;

    PanelSessionInteraction(
        SourceCollectionSession& session,
        SourceCollectionActivationTransaction& activation);
    PanelSessionInteraction(
        Submitter submit,
        ViewReader read_view);

    [[nodiscard]] const SourceCollectionSessionView& View();
    [[nodiscard]] Update Submit(
        SourceCollectionSessionIntent intent);
    [[nodiscard]] Update SubmitNavigation(
        SourceCollectionSessionIntent intent,
        SampleNavigationRequestKind request_kind);
    [[nodiscard]] Update SubmitAutoAdvance(
        SourceCollectionSessionIntent intent);
    [[nodiscard]] const SourceCollectionSessionAction&
        PendingAction() const noexcept;
    [[nodiscard]] SourceCollectionSessionAction TakeAction();

private:
    [[nodiscard]] Update Submit(
        SourceCollectionSessionIntent intent,
        std::optional<NavigationLatencyInputKind> latency_kind);

    Submitter submit_;
    ViewReader read_view_;
    SourceCollectionSessionAction pending_action_;
};

}  // namespace spectiary
