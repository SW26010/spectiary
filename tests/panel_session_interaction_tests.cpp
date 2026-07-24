#include "ui/panel_session_interaction.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void TestSubmissionRefreshesViewAndAggregatesActions()
{
    specforge::SourceCollectionSessionView view;
    int view_reads = 0;
    int submissions = 0;
    std::vector<std::optional<specforge::NavigationLatencyInputKind>>
        latency_kinds;

    specforge::PanelSessionInteraction interaction(
        [&](
            specforge::SourceCollectionSessionIntent,
            std::optional<specforge::NavigationLatencyInputKind>
                latency_kind) {
            ++submissions;
            latency_kinds.push_back(latency_kind);
            specforge::SourceCollectionSessionResult result;
            if (submissions == 1) {
                result.action.workflow_changed = true;
            } else {
                result.action.navigation_inputs_changed = true;
            }
            return result;
        },
        [&]() -> const specforge::SourceCollectionSessionView& {
            ++view_reads;
            return view;
        });

    Require(
        &interaction.View() == &view && view_reads == 1,
        "the interaction should expose the current session view");

    const auto standard = interaction.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleFiltering(
                specforge::SampleFilteringIntent::Clear()));
    Require(
        &standard.view.get() == &view && view_reads == 2,
        "a standard mutation should return a freshly read view");
    Require(
        !latency_kinds[0].has_value(),
        "a standard mutation should not request latency tracing");

    const auto next = interaction.SubmitNavigation(
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::SampleNavigationRequest::Next())),
        specforge::SampleNavigationRequestKind::Next);
    Require(
        &next.view.get() == &view && view_reads == 3,
        "a navigation mutation should return a freshly read view");
    Require(
        latency_kinds[1] ==
            specforge::NavigationLatencyInputKind::UiNext,
        "next navigation should be classified centrally");

    const specforge::SourceCollectionSessionAction action =
        interaction.TakeAction();
    Require(
        action.workflow_changed &&
            action.navigation_inputs_changed,
        "all panel mutations should contribute to one action");
    const specforge::SourceCollectionSessionAction cleared =
        interaction.TakeAction();
    Require(
        !cleared.workflow_changed &&
            !cleared.navigation_inputs_changed,
        "taking the aggregate should reset it");
}

void TestNavigationClassifications()
{
    specforge::SourceCollectionSessionView view;
    std::vector<std::optional<specforge::NavigationLatencyInputKind>>
        latency_kinds;
    specforge::PanelSessionInteraction interaction(
        [&](
            specforge::SourceCollectionSessionIntent,
            std::optional<specforge::NavigationLatencyInputKind>
                latency_kind) {
            latency_kinds.push_back(latency_kind);
            return specforge::SourceCollectionSessionResult{};
        },
        [&]() -> const specforge::SourceCollectionSessionView& {
            return view;
        });

    (void)interaction.SubmitNavigation(
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::SampleNavigationRequest::Previous())),
        specforge::SampleNavigationRequestKind::Previous);
    (void)interaction.SubmitAutoAdvance(
        specforge::SourceCollectionSessionIntent::
            ChangeActiveSampleWorkflow(
                specforge::ActiveSampleWorkflowIntent::
                    ClearActiveLabelForCurrentSample()));

    Require(
        latency_kinds.size() == 2 &&
            latency_kinds[0] ==
                specforge::NavigationLatencyInputKind::UiPrevious &&
            latency_kinds[1] ==
                specforge::NavigationLatencyInputKind::AutoAdvance,
        "previous and auto-advance classifications should remain distinct");
}

}  // namespace

int main()
{
    TestSubmissionRefreshesViewAndAggregatesActions();
    TestNavigationClassifications();
    return 0;
}
