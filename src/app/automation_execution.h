#pragma once

#include "automation/automation_named_pipe.h"
#include "ui/shell_ui.h"
#include <chrono>
#include <functional>
#include <optional>
#include <vector>

namespace spectiary {

// Owns accepted source/navigation/label commands, never source or session data.
// Callbacks run synchronously on the application thread. Terminal callbacks retire
// the request before returning; presentation observations are successful Presents.
class AutomationExecution {
public:
    using Clock = std::chrono::steady_clock;
    using SourceOperation = ShellAutomationSourceOperation;
    using Presentation =
        SourceCollectionActivationTransaction::PresentedSpectrumObservation;
    struct Callbacks {
        std::function<bool(std::string_view)> is_request_active;
        std::function<bool(const AutomationQueuedCommand&)> try_claim_execution;
        std::function<void(const AutomationQueuedCommand&, const AutomationCommandResult&)> complete;
        std::function<void(const AutomationQueuedCommand&, std::string_view, std::string_view)> fail;
        std::function<SourceOperation(const std::filesystem::path&)> open_source;
        std::function<ShellAutomationNavigationResult(std::optional<std::size_t>, std::optional<std::string_view>)> goto_spectrum;
        std::function<ShellAutomationLabelAssignmentResult(int)> assign_label;
        std::function<std::uint64_t()> activation_generation;
        std::function<Presentation()> presented_spectrum;
        std::function<ShellAutomationView()> view;
        std::function<bool()> shell_idle;
        std::function<void()> request_frame;
    };
    void BeginAutomationSourceOpen(
        const AutomationQueuedCommand& command, const Callbacks& callbacks);
    void BeginAutomationSpectrumGoto(
        const AutomationQueuedCommand& command, const Callbacks& callbacks);
    void BeginAutomationLabelAssign(
        const AutomationQueuedCommand& command, const Callbacks& callbacks);

    void Poll(const Callbacks& callbacks);
    // Transport owns shutdown settlement; release operations after it retires
    // requests, without canceling Shell-owned work or undoing committed labels.
    void SettleForShutdown();
    [[nodiscard]] bool Idle() const noexcept;
    [[nodiscard]] std::optional<Clock::time_point> NextDeadline() const;
private:
    void RequestFrame(const Callbacks& callbacks);
    std::optional<Clock::time_point> poll_deadline_;
    struct AutomationSourceCommand {
        AutomationQueuedCommand command;
        SourceOperation operation;
    };

    struct AutomationGotoCommand {
        AutomationQueuedCommand command;
        std::string source_id;
        std::size_t target_index = 0;
        std::uint64_t activation_generation = 0;
        std::uint64_t presented_sequence_before = 0;
        bool changed = false;
    };

    struct AutomationLabelCommand {
        enum class Phase {
            NavigatingToTarget,
            WaitingForAutoAdvance,
        };

        AutomationQueuedCommand command;
        Phase phase = Phase::NavigatingToTarget;
        std::string source_id;
        std::size_t target_index = 0;
        std::uint64_t activation_generation = 0;
        std::uint64_t presented_sequence_before = 0;
        std::optional<
            ShellAutomationLabelAssignmentResult>
            assignment;
    };
    void ContinueAutomationLabelAssign(AutomationLabelCommand& operation, const Callbacks& callbacks);
    std::vector<AutomationSourceCommand>
        automation_source_commands_;
    std::optional<AutomationGotoCommand>
        automation_goto_command_;
    std::optional<AutomationLabelCommand>
        automation_label_command_;

};
} // namespace spectiary
