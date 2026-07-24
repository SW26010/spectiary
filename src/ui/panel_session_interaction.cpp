#include "ui/panel_session_interaction.h"

#include "ui/source_collection_activation_transaction.h"

#include <stdexcept>
#include <utility>

namespace specforge {

PanelSessionInteraction::PanelSessionInteraction(
    SourceCollectionSession& session,
    SourceCollectionActivationTransaction& activation)
    : PanelSessionInteraction(
          [&activation](
              SourceCollectionSessionIntent intent,
              std::optional<NavigationLatencyInputKind>
                  latency_kind) {
              return activation.Submit(
                  std::move(intent),
                  latency_kind
                      ? std::optional<
                            SourceCollectionActivationTransaction::
                                NavigationIntent>{
                            SourceCollectionActivationTransaction::
                                NavigationIntent{
                                    *latency_kind,
                                    NavigationLatencyTrace::Now()}}
                      : std::nullopt);
          },
          [&session]() -> const SourceCollectionSessionView& {
              return session.View();
          })
{
}

PanelSessionInteraction::PanelSessionInteraction(
    Submitter submit,
    ViewReader read_view)
    : submit_(std::move(submit)),
      read_view_(std::move(read_view))
{
    if (!submit_ || !read_view_) {
        throw std::invalid_argument(
            "Panel session interaction requires submit and view callbacks.");
    }
}

const SourceCollectionSessionView& PanelSessionInteraction::View()
{
    return read_view_();
}

PanelSessionInteraction::Update PanelSessionInteraction::Submit(
    SourceCollectionSessionIntent intent)
{
    return Submit(std::move(intent), std::nullopt);
}

PanelSessionInteraction::Update
PanelSessionInteraction::SubmitNavigation(
    SourceCollectionSessionIntent intent,
    SampleNavigationRequestKind request_kind)
{
    NavigationLatencyInputKind latency_kind;
    switch (request_kind) {
    case SampleNavigationRequestKind::Previous:
        latency_kind = NavigationLatencyInputKind::UiPrevious;
        break;
    case SampleNavigationRequestKind::Next:
        latency_kind = NavigationLatencyInputKind::UiNext;
        break;
    default:
        throw std::invalid_argument(
            "Panel navigation submission requires previous or next intent.");
    }
    return Submit(std::move(intent), latency_kind);
}

PanelSessionInteraction::Update
PanelSessionInteraction::SubmitAutoAdvance(
    SourceCollectionSessionIntent intent)
{
    return Submit(
        std::move(intent),
        NavigationLatencyInputKind::AutoAdvance);
}

const SourceCollectionSessionAction&
PanelSessionInteraction::PendingAction() const noexcept
{
    return pending_action_;
}

SourceCollectionSessionAction
PanelSessionInteraction::TakeAction()
{
    return std::exchange(
        pending_action_,
        SourceCollectionSessionAction{});
}

PanelSessionInteraction::Update PanelSessionInteraction::Submit(
    SourceCollectionSessionIntent intent,
    std::optional<NavigationLatencyInputKind> latency_kind)
{
    SourceCollectionSessionResult result =
        submit_(std::move(intent), latency_kind);
    MergeSourceCollectionSessionAction(
        pending_action_,
        result.action);
    return {
        std::move(result),
        std::cref(read_view_()),
    };
}

}  // namespace specforge
