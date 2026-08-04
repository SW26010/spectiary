#pragma once

#include "app/application_settings.h"
#include "automation/automation_named_pipe.h"
#include "automation/automation_panel_mutation_chain.h"
#include "ui/shell_ui.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

class AutomationPanelCommandCoordinator {
public:
    enum class ApplyOutcome {
        Unchanged,
        Applied,
        Rejected,
        PersistenceFailed,
    };

    struct ApplyResult {
        ApplyOutcome outcome = ApplyOutcome::Unchanged;
        std::string detail;
    };

    struct RuntimeState {
        bool window_renderable = true;
        bool immersive_plot_mode = false;
        bool presentation_available = true;
    };

    struct Callbacks {
        std::function<bool(std::string_view)> is_request_active;
        std::function<bool(const AutomationQueuedCommand&)> try_claim_execution;
        std::function<PanelVisibilityState()> current_visibility;
        std::function<ApplyResult(ApplicationPanel, bool)> apply_visibility;
        std::function<void(const AutomationQueuedCommand&, std::string_view)>
            complete;
        std::function<void(
            const AutomationQueuedCommand&,
            std::string_view,
            std::string_view)>
            fail;
        std::function<void()> request_poll;
    };

    void ServicePanelGet(
        const AutomationQueuedCommand& command,
        const Callbacks& callbacks) const;
    void ServicePanelSet(
        const AutomationQueuedCommand& command,
        std::uint64_t accepted_frame,
        const RuntimeState& runtime,
        const Callbacks& callbacks);
    void Poll(
        const ShellAutomationPanelPresentation& presented,
        const ShellAutomationPanelPresentationStatus& presentation_status,
        const RuntimeState& runtime,
        const Callbacks& callbacks);
    [[nodiscard]] bool SettleForShutdown(
        const Callbacks& callbacks);

    [[nodiscard]] bool Idle() const noexcept;

private:
    enum class RollbackReason {
        PresentationUnavailable,
        ViewportBlocked,
        Shutdown,
    };

    struct PendingCommand {
        AutomationQueuedCommand command;
        ApplicationPanel panel = ApplicationPanel::Files;
        std::string name;
        bool visible = true;
        bool changed = false;
        std::uint64_t generation = 0;
        std::uint64_t accepted_frame = 0;
    };

    struct RollbackResolution {
        std::uint64_t generation = 0;
        bool succeeded = false;
        RollbackReason reason = RollbackReason::PresentationUnavailable;
    };

    using RollbackResolutions =
        std::array<std::optional<RollbackResolution>, kApplicationPanelCount>;

    [[nodiscard]] std::optional<RollbackResolution>
    RollbackMutationChain(
        ApplicationPanel panel,
        RollbackReason reason,
        const Callbacks& callbacks);
    [[nodiscard]] RollbackResolutions RollbackActiveMutationChains(
        RollbackReason reason,
        const Callbacks& callbacks);

    std::vector<PendingCommand> pending_commands_;
    std::array<std::uint64_t, kApplicationPanelCount> generations_{};
    std::array<AutomationPanelMutationChain, kApplicationPanelCount>
        mutation_chains_;
};

}  // namespace specforge
