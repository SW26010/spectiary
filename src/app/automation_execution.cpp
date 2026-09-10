#include "app/automation_execution.h"
#include "platform/win32_text.h"
#include <algorithm>
#include <utility>

namespace specforge {
namespace {
std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}
std::pair<std::string_view, std::string_view>
AutomationNavigationFailure(
    ShellAutomationNavigationError error)
{
    switch (error) {
    case ShellAutomationNavigationError::NoActiveSource:
        return {
            "no_active_source",
            "No source is active.",
        };
    case ShellAutomationNavigationError::SourceNotReady:
        return {
            "source_not_ready",
            "The active source is not ready for spectrum navigation.",
        };
    case ShellAutomationNavigationError::IndexOutOfRange:
        return {
            "spectrum_index_out_of_range",
            "The requested spectrum index is outside the active source.",
        };
    case ShellAutomationNavigationError::NameUnavailable:
        return {
            "spectrum_name_unavailable",
            "The active source does not provide spectrum names.",
        };
    case ShellAutomationNavigationError::NameNotFound:
        return {
            "spectrum_not_found",
            "No spectrum has the requested exact name.",
        };
    case ShellAutomationNavigationError::NameAmbiguous:
        return {
            "spectrum_ambiguous",
            "More than one spectrum has the requested exact name.",
        };
    case ShellAutomationNavigationError::FilteredOut:
        return {
            "spectrum_filtered_out",
            "The requested spectrum is outside the active navigation sequence.",
        };
    case ShellAutomationNavigationError::Rejected:
        return {
            "spectrum_load_failed",
            "The real session rejected the requested spectrum.",
        };
    case ShellAutomationNavigationError::None:
        break;
    }
    return {
        "spectrum_load_failed",
        "Spectrum navigation failed.",
    };
}

std::pair<std::string_view, std::string_view>
AutomationLabelFailure(
    ShellAutomationLabelError error)
{
    switch (error) {
    case ShellAutomationLabelError::NoCurrentSpectrum:
        return {
            "no_current_spectrum",
            "No current spectrum is ready for labeling.",
        };
    case ShellAutomationLabelError::NoActiveTask:
        return {
            "no_active_label_task",
            "No labeling task is active.",
        };
    case ShellAutomationLabelError::LabelNotFound:
        return {
            "label_not_found",
            "The active labeling task does not contain the requested code.",
        };
    case ShellAutomationLabelError::Rejected:
        return {
            "label_assignment_rejected",
            "The real labeling workflow rejected the assignment.",
        };
    case ShellAutomationLabelError::None:
        break;
    }
    return {
        "label_assignment_rejected",
        "Label assignment failed.",
    };
}


} // namespace
void AutomationExecution::BeginAutomationSourceOpen(
    const AutomationQueuedCommand& command, const Callbacks& callbacks)
{
    const auto* parameters =
        std::get_if<AutomationSourceOpenParameters>(
            &command.parameters);
    if (parameters == nullptr) {
        callbacks.fail(
            command,
            "invalid_params",
            "source.open parameters were not decoded.");
        return;
    }
    const std::filesystem::path path(
        Utf8ToWide(parameters->path));
    if (!path.is_absolute()) {
        callbacks.fail(
            command,
            "path_not_absolute",
            "source.open path must be absolute.");
        return;
    }
    std::error_code path_error;
    const bool supported_path_kind =
        std::filesystem::is_regular_file(
            path,
            path_error) ||
        std::filesystem::is_directory(
            path,
            path_error);
    if (path_error || !supported_path_kind) {
        callbacks.fail(
            command,
            "source_not_found",
            "source.open path must identify an existing file or directory.");
        return;
    }
    if (!callbacks.try_claim_execution(
            command)) {
        return;
    }

    automation_source_commands_.push_back({
        .command = command,
        .operation =
            callbacks.open_source(path),
    });
    RequestFrame(callbacks);
}

void AutomationExecution::BeginAutomationSpectrumGoto(
    const AutomationQueuedCommand& command, const Callbacks& callbacks)
{
    if (automation_goto_command_ ||
        automation_label_command_) {
        callbacks.fail(
            command,
            "operation_busy",
            "Another automation navigation or label operation is active.");
        return;
    }
    const auto* parameters =
        std::get_if<AutomationSpectrumGotoParameters>(
            &command.parameters);
    if (parameters == nullptr) {
        callbacks.fail(
            command,
            "invalid_params",
            "spectrum.goto parameters were not decoded.");
        return;
    }
    if (!callbacks.try_claim_execution(
            command)) {
        return;
    }
    ShellAutomationNavigationResult navigation =
        callbacks.goto_spectrum(
            parameters->target.index,
            parameters->target.name
                ? std::optional<std::string_view>(
                      *parameters->target.name)
                : std::nullopt);
    if (navigation.error !=
        ShellAutomationNavigationError::None) {
        const auto [code, message] =
            AutomationNavigationFailure(
                navigation.error);
        callbacks.fail(
            command,
            code,
            message);
        return;
    }
    automation_goto_command_ =
        AutomationGotoCommand{
            .command = command,
            .source_id =
                std::move(navigation.source_id),
            .target_index =
                navigation.target.index,
            .activation_generation =
                callbacks.activation_generation(),
            .presented_sequence_before =
                callbacks.presented_spectrum()
                    .sequence,
            .changed = navigation.changed,
        };
    RequestFrame(callbacks);
}

void AutomationExecution::BeginAutomationLabelAssign(
    const AutomationQueuedCommand& command, const Callbacks& callbacks)
{
    if (automation_goto_command_ ||
        automation_label_command_) {
        callbacks.fail(
            command,
            "operation_busy",
            "Another automation navigation or label operation is active.");
        return;
    }
    const auto* parameters =
        std::get_if<AutomationLabelAssignParameters>(
            &command.parameters);
    if (parameters == nullptr) {
        callbacks.fail(
            command,
            "invalid_params",
            "label.assign parameters were not decoded.");
        return;
    }
    if (!callbacks.try_claim_execution(
            command)) {
        return;
    }

    automation_label_command_ =
        AutomationLabelCommand{
            .command = command,
            .phase =
                AutomationLabelCommand::Phase::
                    NavigatingToTarget,
            .activation_generation =
                callbacks.activation_generation(),
            .presented_sequence_before =
                callbacks.presented_spectrum()
                    .sequence,
        };
    AutomationLabelCommand& operation =
        *automation_label_command_;
    if (parameters->target) {
        ShellAutomationNavigationResult navigation =
            callbacks.goto_spectrum(
                parameters->target->index,
                parameters->target->name
                    ? std::optional<std::string_view>(
                          *parameters->target->name)
                    : std::nullopt);
        if (navigation.error !=
            ShellAutomationNavigationError::None) {
            const auto [code, message] =
                AutomationNavigationFailure(
                    navigation.error);
            callbacks.fail(
                command,
                code,
                message);
            automation_label_command_.reset();
            return;
        }
        operation.source_id =
            std::move(navigation.source_id);
        operation.target_index =
            navigation.target.index;
        operation.activation_generation =
            callbacks.activation_generation();
        const auto& presented =
            callbacks.presented_spectrum();
        operation.presented_sequence_before =
            presented.sequence;
        RequestFrame(callbacks);
        return;
    }
    ContinueAutomationLabelAssign(operation, callbacks);
    if (!callbacks.is_request_active(
            command.request_id)) {
        automation_label_command_.reset();
    }
    RequestFrame(callbacks);
}

void AutomationExecution::ContinueAutomationLabelAssign(
    AutomationLabelCommand& operation, const Callbacks& callbacks)
{
    const auto* parameters =
        std::get_if<AutomationLabelAssignParameters>(
            &operation.command.parameters);
    if (parameters == nullptr) {
        callbacks.fail(
            operation.command,
            "invalid_params",
            "label.assign parameters were not decoded.");
        return;
    }
    ShellAutomationLabelAssignmentResult assignment =
        callbacks.assign_label(
            parameters->code);
    if (assignment.error !=
        ShellAutomationLabelError::None) {
        const auto [code, message] =
            AutomationLabelFailure(
                assignment.error);
        callbacks.fail(
            operation.command,
            code,
            message);
        return;
    }
    operation.source_id =
        assignment.source_id;
    operation.target_index =
        assignment.spectrum.index;
    operation.assignment =
        std::move(assignment);
    operation.phase =
        AutomationLabelCommand::Phase::
            WaitingForAutoAdvance;
    operation.presented_sequence_before =
        callbacks.presented_spectrum()
            .sequence;
}


void AutomationExecution::Poll(const Callbacks& callbacks)
{
    poll_deadline_ = Clock::now() + std::chrono::milliseconds(25);
    automation_source_commands_.erase(
        std::remove_if(
            automation_source_commands_.begin(),
            automation_source_commands_.end(),
            [&](AutomationSourceCommand& pending) {
                if (!callbacks.is_request_active(
                        pending.command.request_id)) {
                    return true;
                }
                const auto outcome =
                    pending.operation();
                using State =
                    ShellAutomationSourceOutcome::State;
                if (outcome.state == State::Pending) {
                    return false;
                }
                if (outcome.state == State::Succeeded) {
                    callbacks.complete(pending.command,
                        AutomationSourceOpenResult{outcome.source_id, PathToUtf8(outcome.source_path),
                            outcome.spectrum_count, {outcome.spectrum_index, outcome.spectrum_name}});
                } else if (
                    outcome.state == State::Failed) {
                    callbacks.fail(
                        pending.command,
                        "source_load_failed",
                        "The real source loader could not activate the requested source.");
                } else {
                    callbacks.fail(
                        pending.command,
                        "operation_canceled",
                        "The source open operation was superseded or canceled.");
                }
                return true;
            }),
        automation_source_commands_.end());

    if (automation_goto_command_) {
        AutomationGotoCommand& pending =
            *automation_goto_command_;
        if (!callbacks.is_request_active(
                pending.command.request_id)) {
            automation_goto_command_.reset();
        } else {
            const ShellAutomationView view =
                callbacks.view();
            const auto& presented =
                callbacks.presented_spectrum();
            const bool shell_idle =
                callbacks.shell_idle();
            if (callbacks.activation_generation() !=
                pending.activation_generation) {
                callbacks.fail(
                    pending.command,
                    "operation_canceled",
                    "The source activation changed before spectrum navigation completed.");
                automation_goto_command_.reset();
            } else if (!view.source_id.empty() &&
                view.source_id != pending.source_id) {
                callbacks.fail(
                    pending.command,
                    "operation_canceled",
                    "The active source changed before spectrum navigation completed.");
                automation_goto_command_.reset();
            } else if (
                shell_idle &&
                view.spectrum.present &&
                view.spectrum.index ==
                    pending.target_index &&
                presented.sequence >
                    pending
                        .presented_sequence_before &&
                presented.activation_generation ==
                    pending.activation_generation &&
                presented.source_id ==
                    pending.source_id &&
                presented.spectrum_index ==
                    pending.target_index) {
                callbacks.complete(pending.command,
                    AutomationSpectrumGotoResult{pending.source_id,
                        {view.spectrum.index, view.spectrum.name}, pending.changed});
                automation_goto_command_.reset();
            } else if (
                shell_idle &&
                (!view.spectrum.present ||
                 view.spectrum.index !=
                     pending.target_index)) {
                callbacks.fail(
                    pending.command,
                    "spectrum_load_failed",
                    "The requested spectrum did not become the current rendered spectrum.");
                automation_goto_command_.reset();
            }
        }
    }

    if (automation_label_command_) {
        AutomationLabelCommand& pending =
            *automation_label_command_;
        if (!callbacks.is_request_active(
                pending.command.request_id)) {
            automation_label_command_.reset();
        } else {
            const ShellAutomationView view =
                callbacks.view();
            const auto& presented =
                callbacks.presented_spectrum();
            const bool shell_idle =
                callbacks.shell_idle();
            if (pending.phase ==
                AutomationLabelCommand::Phase::
                    NavigatingToTarget) {
                if (callbacks.activation_generation() !=
                    pending.activation_generation) {
                    callbacks.fail(
                        pending.command,
                        "operation_canceled",
                        "The source activation changed before the label target was ready.");
                } else if (!view.source_id.empty() &&
                    view.source_id !=
                        pending.source_id) {
                    callbacks.fail(
                        pending.command,
                        "operation_canceled",
                        "The active source changed before the label target was ready.");
                } else if (
                    shell_idle &&
                    view.spectrum.present &&
                    view.spectrum.index ==
                        pending.target_index &&
                    presented.sequence >
                        pending
                            .presented_sequence_before &&
                    presented.activation_generation ==
                        pending.activation_generation &&
                    presented.source_id ==
                        pending.source_id &&
                    presented.spectrum_index ==
                        pending.target_index) {
                    ContinueAutomationLabelAssign(pending, callbacks);
                    RequestFrame(callbacks);
                } else if (
                    shell_idle &&
                    (!view.spectrum.present ||
                     view.spectrum.index !=
                         pending.target_index)) {
                    callbacks.fail(
                        pending.command,
                        "spectrum_load_failed",
                        "The label target did not become the current rendered spectrum.");
                }
            } else if (pending.assignment) {
                const bool needs_render =
                    pending.assignment->changed ||
                    pending.assignment->
                        navigation_pending;
                const bool activation_replaced =
                    callbacks.activation_generation() !=
                    pending.activation_generation;
                if (activation_replaced ||
                    (shell_idle &&
                     (!needs_render ||
                      (view.spectrum.present &&
                       presented.sequence >
                           pending
                               .presented_sequence_before &&
                       presented.source_id ==
                           pending.source_id &&
                       presented.spectrum_index ==
                           view.spectrum.index)))) {
                    const auto& assignment =
                        *pending.assignment;
                    callbacks.complete(pending.command,
                        AutomationLabelAssignResult{
                            assignment.source_id, assignment.task_id,
                            {assignment.spectrum.index, assignment.spectrum.name},
                            assignment.previous_code, assignment.new_code, assignment.changed,
                            {assignment.state_save_scheduled, assignment.state_save_attempted,
                                assignment.state_saved, assignment.output_save_attempted,
                                assignment.output_saved, assignment.output_retry_scheduled},
                            view.spectrum.present
                                ? std::optional<AutomationResultSpectrum>{{view.spectrum.index, view.spectrum.name}}
                                : std::nullopt});
                }
            }
            if (!callbacks.is_request_active(
                    pending.command.request_id)) {
                automation_label_command_.reset();
            }
        }
    }

}

bool AutomationExecution::Idle() const noexcept
{
    return automation_source_commands_.empty() && !automation_goto_command_ && !automation_label_command_;
}

void AutomationExecution::RequestFrame(const Callbacks& callbacks)
{
    poll_deadline_ = Clock::now() + std::chrono::milliseconds(25);
    callbacks.request_frame();
}

void AutomationExecution::SettleForShutdown()
{
    automation_source_commands_.clear();
    automation_goto_command_.reset();
    automation_label_command_.reset();
    poll_deadline_.reset();
}

std::optional<AutomationExecution::Clock::time_point>
AutomationExecution::NextDeadline() const
{
    return Idle() ? std::nullopt : poll_deadline_;
}
} // namespace specforge
