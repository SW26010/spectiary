#include "app/automation_panel_command_coordinator.h"

#include "app/local_user_state_json.h"
#include "automation/automation_protocol.h"

#include <algorithm>
#include <ranges>
#include <sstream>

namespace specforge {

namespace {

[[nodiscard]] std::optional<ApplicationPanel> PanelFromName(
    std::string_view name)
{
    if (name == "files") {
        return ApplicationPanel::Files;
    }
    if (name == "navigation") {
        return ApplicationPanel::Navigation;
    }
    if (name == "annotations") {
        return ApplicationPanel::Annotations;
    }
    if (name == "labeling") {
        return ApplicationPanel::Labeling;
    }
    if (name == "filters") {
        return ApplicationPanel::Filters;
    }
    if (name == "sorting") {
        return ApplicationPanel::Sorting;
    }
    if (name == "smoothing") {
        return ApplicationPanel::Smoothing;
    }
    if (name == "information") {
        return ApplicationPanel::Information;
    }
    if (name == "spectral_lines") {
        return ApplicationPanel::SpectralLines;
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t PanelIndex(ApplicationPanel panel) noexcept
{
    return static_cast<std::size_t>(panel);
}

[[nodiscard]] std::string JsonString(std::string_view value)
{
    return "\"" + JsonEscape(value) + "\"";
}

[[nodiscard]] const char* JsonBool(bool value) noexcept
{
    return value ? "true" : "false";
}

[[nodiscard]] std::string PanelResult(
    std::string_view name,
    bool visible,
    std::optional<bool> changed = std::nullopt,
    std::optional<std::uint64_t> frame_index = std::nullopt)
{
    std::ostringstream body;
    body << "\"result\":{\"name\":"
         << JsonString(name)
         << ",\"visible\":"
         << JsonBool(visible);
    if (changed.has_value()) {
        body << ",\"changed\":"
             << JsonBool(*changed);
    }
    if (frame_index.has_value()) {
        body << ",\"frame_index\":"
             << *frame_index;
    }
    body << '}';
    return body.str();
}

}  // namespace

void AutomationPanelCommandCoordinator::ServicePanelGet(
    const AutomationQueuedCommand& command,
    const Callbacks& callbacks) const
{
    if (!callbacks.is_request_active(command.request_id)) {
        return;
    }
    const auto* parameters =
        std::get_if<AutomationPanelGetParameters>(&command.parameters);
    if (parameters == nullptr) {
        callbacks.fail(
            command,
            "invalid_params",
            "panel.get parameters were not decoded.");
        return;
    }
    const std::optional<ApplicationPanel> panel =
        PanelFromName(parameters->name);
    if (!panel) {
        callbacks.fail(
            command,
            "unsupported_panel",
            "The requested automation panel is not supported.");
        return;
    }

    callbacks.complete(
        command,
        PanelResult(
            parameters->name,
            ApplicationPanelVisible(
                callbacks.current_visibility(),
                *panel)));
}

void AutomationPanelCommandCoordinator::ServicePanelSet(
    const AutomationQueuedCommand& command,
    std::uint64_t accepted_frame,
    const RuntimeState& runtime,
    const Callbacks& callbacks)
{
    const auto* parameters =
        std::get_if<AutomationPanelSetParameters>(&command.parameters);
    if (parameters == nullptr) {
        callbacks.fail(
            command,
            "invalid_params",
            "panel.set parameters were not decoded.");
        return;
    }
    const std::optional<ApplicationPanel> panel =
        PanelFromName(parameters->name);
    if (!panel) {
        callbacks.fail(
            command,
            "unsupported_panel",
            "The requested automation panel is not supported.");
        return;
    }
    if (!runtime.window_renderable) {
        callbacks.fail(
            command,
            "window_not_renderable",
            "The main application window is hidden or minimized; panel visibility was not changed.");
        return;
    }
    if (runtime.immersive_plot_mode) {
        callbacks.fail(
            command,
            "panel_not_renderable",
            "Immersive plot mode does not render product panels; panel visibility was not changed.");
        return;
    }
    if (!callbacks.try_claim_execution(command)) {
        return;
    }

    const bool previous_visible =
        ApplicationPanelVisible(
            callbacks.current_visibility(),
            *panel);
    const ApplyResult result = callbacks.apply_visibility(
        *panel,
        parameters->visible);
    if (result.outcome == ApplyOutcome::Rejected) {
        callbacks.fail(
            command,
            "panel_visibility_rejected",
            result.detail.empty()
                ? "The production settings owner rejected the panel visibility change."
                : result.detail);
        return;
    }
    if (result.outcome == ApplyOutcome::PersistenceFailed) {
        callbacks.fail(
            command,
            "panel_persistence_failed",
            result.detail.empty()
                ? "The panel visibility change could not be persisted."
                : result.detail);
        return;
    }

    std::uint64_t& generation = generations_[PanelIndex(*panel)];
    if (result.outcome == ApplyOutcome::Applied) {
        ++generation;
        mutation_chains_[PanelIndex(*panel)].RecordAppliedMutation(
            previous_visible,
            parameters->visible,
            generation,
            accepted_frame);
    }
    pending_commands_.push_back({
        .command = command,
        .panel = *panel,
        .name = parameters->name,
        .visible = parameters->visible,
        .changed = result.outcome == ApplyOutcome::Applied,
        .generation = generation,
        .accepted_frame = accepted_frame,
    });
    callbacks.request_poll();
}

std::optional<
    AutomationPanelCommandCoordinator::RollbackResolution>
AutomationPanelCommandCoordinator::RollbackMutationChain(
    ApplicationPanel panel,
    RollbackReason reason,
    const Callbacks& callbacks)
{
    AutomationPanelMutationChain& chain =
        mutation_chains_[PanelIndex(panel)];
    if (!chain.active()) {
        return std::nullopt;
    }
    if (ApplicationPanelVisible(
            callbacks.current_visibility(),
            panel) != chain.requested_visible()) {
        chain.Clear();
        return std::nullopt;
    }

    const std::uint64_t generation = chain.generation();
    (void)callbacks.apply_visibility(
        panel,
        chain.baseline_visible());
    const bool restored =
        ApplicationPanelVisible(
            callbacks.current_visibility(),
            panel) == chain.baseline_visible();
    chain.Clear();
    return RollbackResolution{
        .generation = generation,
        .succeeded = restored,
        .reason = reason,
    };
}

AutomationPanelCommandCoordinator::RollbackResolutions
AutomationPanelCommandCoordinator::RollbackActiveMutationChains(
    RollbackReason reason,
    const Callbacks& callbacks)
{
    RollbackResolutions resolutions;
    for (std::size_t panel_index = 0;
         panel_index < kApplicationPanelCount;
         ++panel_index) {
        resolutions[panel_index] = RollbackMutationChain(
            static_cast<ApplicationPanel>(panel_index),
            reason,
            callbacks);
    }
    return resolutions;
}

void AutomationPanelCommandCoordinator::Poll(
    const ShellAutomationPanelPresentation& presented,
    const ShellAutomationPanelPresentationStatus& presentation_status,
    const RuntimeState& runtime,
    const Callbacks& callbacks)
{
    const PanelVisibilityState visibility = callbacks.current_visibility();
    for (std::size_t panel_index = 0;
         panel_index < kApplicationPanelCount;
         ++panel_index) {
        AutomationPanelMutationChain& chain = mutation_chains_[panel_index];
        if (!chain.active()) {
            continue;
        }
        const ApplicationPanel panel =
            static_cast<ApplicationPanel>(panel_index);
        const bool live_visible = ApplicationPanelVisible(visibility, panel);
        if (live_visible != chain.requested_visible()) {
            chain.Clear();
            continue;
        }
        if (presented.FrameIndex(panel) > chain.accepted_frame() &&
            ApplicationPanelVisible(
                presented.visibility,
                panel) == chain.requested_visible()) {
            chain.Clear();
        }
    }

    RollbackResolutions rollback_resolutions;
    if (!runtime.presentation_available) {
        rollback_resolutions = RollbackActiveMutationChains(
            RollbackReason::PresentationUnavailable,
            callbacks);
    } else {
        for (std::size_t panel_index = 0;
             panel_index < kApplicationPanelCount;
             ++panel_index) {
            AutomationPanelMutationChain& chain =
                mutation_chains_[panel_index];
            if (chain.active() &&
                presentation_status.BlockedAfter(
                    static_cast<ApplicationPanel>(panel_index),
                    chain.accepted_frame())) {
                rollback_resolutions[panel_index] = RollbackMutationChain(
                    static_cast<ApplicationPanel>(panel_index),
                    RollbackReason::ViewportBlocked,
                    callbacks);
            }
        }
    }

    pending_commands_.erase(
        std::remove_if(
            pending_commands_.begin(),
            pending_commands_.end(),
            [this,
             &presented,
             &presentation_status,
             &rollback_resolutions,
             runtime,
             &callbacks](const PendingCommand& pending) {
                if (!callbacks.is_request_active(
                        pending.command.request_id)) {
                    return true;
                }
                if (generations_[PanelIndex(pending.panel)] !=
                    pending.generation) {
                    callbacks.fail(
                        pending.command,
                        "operation_canceled",
                        "Panel visibility changed again before the requested state was presented.");
                    return true;
                }
                const bool live_visible = ApplicationPanelVisible(
                    callbacks.current_visibility(),
                    pending.panel);
                const std::uint64_t presented_frame =
                    presented.FrameIndex(pending.panel);
                if (live_visible == pending.visible &&
                    presented_frame > pending.accepted_frame &&
                    ApplicationPanelVisible(
                        presented.visibility,
                        pending.panel) == pending.visible) {
                    callbacks.complete(
                        pending.command,
                        PanelResult(
                            pending.name,
                            pending.visible,
                            pending.changed,
                            presented_frame));
                    return true;
                }

                const std::optional<RollbackResolution>& rollback =
                    rollback_resolutions[PanelIndex(pending.panel)];
                if (rollback && rollback->generation == pending.generation) {
                    if (!rollback->succeeded) {
                        callbacks.fail(
                            pending.command,
                            "panel_rollback_failed",
                            "The unpresented panel mutation chain could not be restored to its pre-chain value through the production settings owner.");
                        return true;
                    }
                    if (rollback->reason == RollbackReason::ViewportBlocked) {
                        callbacks.fail(
                            pending.command,
                            "panel_viewport_not_renderable",
                            "A detached viewport required to present the panel state is minimized; the complete unpresented mutation chain was rolled back.");
                        return true;
                    }
                    callbacks.fail(
                        pending.command,
                        runtime.window_renderable
                            ? "panel_not_renderable"
                            : "window_not_renderable",
                        runtime.window_renderable
                            ? "Immersive plot mode began before the requested panel state was presented; the complete unpresented mutation chain was rolled back."
                            : "The main application window became hidden or minimized before the requested panel state was presented; the complete unpresented mutation chain was rolled back.");
                    return true;
                }
                if (live_visible != pending.visible) {
                    callbacks.fail(
                        pending.command,
                        "operation_canceled",
                        "Panel visibility changed again before the requested state was presented.");
                    return true;
                }
                if (!runtime.presentation_available) {
                    callbacks.fail(
                        pending.command,
                        runtime.window_renderable
                            ? "panel_not_renderable"
                            : "window_not_renderable",
                        runtime.window_renderable
                            ? "Immersive plot mode does not provide a normal Shell panel frame; no requested panel state was presented."
                            : "The main application window became hidden or minimized before a requested panel state was presented.");
                    return true;
                }
                if (presentation_status.BlockedAfter(
                        pending.panel,
                        pending.accepted_frame)) {
                    callbacks.fail(
                        pending.command,
                        "panel_viewport_not_renderable",
                        "A detached viewport required to present the panel state is minimized; no production mutation required rollback.");
                    return true;
                }
                return false;
            }),
        pending_commands_.end());
}

bool AutomationPanelCommandCoordinator::SettleForShutdown(
    const Callbacks& callbacks)
{
    const RollbackResolutions resolutions = RollbackActiveMutationChains(
        RollbackReason::Shutdown,
        callbacks);
    const bool restored_panel_state_requires_flush = std::ranges::any_of(
        resolutions,
        [](const auto& resolution) {
            return resolution && resolution->succeeded;
        });

    for (const PendingCommand& pending : pending_commands_) {
        if (!callbacks.is_request_active(pending.command.request_id)) {
            continue;
        }
        if (generations_[PanelIndex(pending.panel)] != pending.generation) {
            callbacks.fail(
                pending.command,
                "operation_canceled",
                "Panel visibility changed again before the requested state was presented.");
            continue;
        }
        const std::optional<RollbackResolution>& resolution =
            resolutions[PanelIndex(pending.panel)];
        if (resolution && resolution->generation == pending.generation) {
            callbacks.fail(
                pending.command,
                resolution->succeeded
                    ? "app_shutdown"
                    : "panel_rollback_failed",
                resolution->succeeded
                    ? "The application closed before the requested panel state was presented; the complete unpresented mutation chain was restored before shutdown."
                    : "The application closed before presentation, and the unpresented panel mutation chain could not be restored before shutdown.");
            continue;
        }
        if (ApplicationPanelVisible(
                callbacks.current_visibility(),
                pending.panel) != pending.visible) {
            callbacks.fail(
                pending.command,
                "operation_canceled",
                "Panel visibility changed again before the requested state was presented.");
            continue;
        }
        callbacks.fail(
            pending.command,
            "app_shutdown",
            "The application closed before the requested panel state was presented; no production mutation remained to restore.");
    }
    pending_commands_.clear();
    return restored_panel_state_requires_flush;
}

bool AutomationPanelCommandCoordinator::Idle() const noexcept
{
    return pending_commands_.empty() &&
           std::ranges::none_of(
               mutation_chains_,
               [](const auto& chain) {
                   return chain.active();
               });
}

}  // namespace specforge
