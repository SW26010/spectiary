#include "automation/automation_state.h"

#include "app/local_user_state_json.h"

#include <sstream>

namespace specforge {
namespace {

std::string JsonString(std::string_view value)
{
    return "\"" + JsonEscape(value) + "\"";
}

std::string PathToUtf8(
    const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return std::string(
        utf8.begin(),
        utf8.end());
}

const char* JsonBool(bool value)
{
    return value ? "true" : "false";
}

}  // namespace

std::string SerializeAutomationStateBody(
    const AutomationStateSnapshot& state)
{
    std::ostringstream output;
    output << "\"state\":{"
           << "\"protocol\":{"
           << "\"version\":"
           << kAutomationProtocolVersion
           << "},\"instance\":{"
           << "\"id\":" << JsonString(state.instance_id)
           << ",\"client_connected\":"
           << JsonBool(
                  state.control.client_connected)
           << ",\"handshake_complete\":"
           << JsonBool(
                  state.control.handshake_complete)
           << "},\"control\":{"
           << "\"accepting_requests\":"
           << JsonBool(
                  state.control.accepting_requests)
           << ",\"pending_count\":"
           << state.control.pending_count
           << ",\"outstanding_count\":"
           << state.control.outstanding_count
           << ",\"capacity\":"
           << state.control.capacity
           << "},\"shell\":{"
           << "\"idle\":"
           << JsonBool(state.shell.idle)
           << ",\"source_load_idle\":"
           << JsonBool(
                  state.shell.source_load_idle)
           << ",\"pending_completion_idle\":"
           << JsonBool(
                  state.shell
                      .pending_completion_idle)
           << ",\"background_retirement_idle\":"
           << JsonBool(
                  state.shell
                      .background_retirement_idle)
           << ",\"active_load_count\":"
           << state.shell.active_load_count
           << ",\"completed_load_count\":"
           << state.shell.completed_load_count
           << ",\"pending_load_count\":"
           << state.shell.pending_load_count
           << ",\"retirement_queued_count\":"
           << state.shell.retirement_queued_count
           << ",\"retirement_in_flight_count\":"
           << state.shell
                  .retirement_in_flight_count
           << "},\"source\":{";
    if (state.shell.current_source_id.empty() &&
        state.shell.current_source_path.empty()) {
        output << "\"present\":false";
    } else {
        output << "\"present\":true"
               << ",\"id\":"
               << JsonString(
                      state.shell.current_source_id)
               << ",\"path\":"
               << JsonString(
                      PathToUtf8(
                          state.shell
                              .current_source_path));
    }
    output << "},\"presented_source\":{"
           << "\"present\":"
           << JsonBool(
                  state.presented_source.present);
    if (state.presented_source.present) {
        output << ",\"id\":"
               << JsonString(
                      state.presented_source.id)
               << ",\"path\":"
               << JsonString(
                      PathToUtf8(
                          state.presented_source
                              .path));
    }
    output << "},\"settings\":{"
           << "\"language\":"
           << JsonString(state.settings.language)
           << ",\"ui_scale_percentage\":"
           << state.settings.ui_scale_percentage
           << "},\"panels\":{"
           << "\"files\":"
           << JsonBool(state.panels.files)
           << ",\"navigation\":"
           << JsonBool(state.panels.navigation)
           << ",\"annotations\":"
           << JsonBool(state.panels.annotations)
           << ",\"labeling\":"
           << JsonBool(state.panels.labeling)
           << ",\"filters\":"
           << JsonBool(state.panels.filters)
           << ",\"sorting\":"
           << JsonBool(state.panels.sorting)
           << ",\"smoothing\":"
           << JsonBool(state.panels.smoothing)
           << ",\"information\":"
           << JsonBool(state.panels.information)
           << ",\"spectral_lines\":"
           << JsonBool(state.panels.spectral_lines)
           << "},\"window\":{"
           << "\"visible\":"
           << JsonBool(state.window.visible)
           << ",\"minimized\":"
           << JsonBool(state.window.minimized)
           << ",\"client_width\":"
           << state.window.client_width
           << ",\"client_height\":"
           << state.window.client_height
           << "},\"spectrum\":{"
           << "\"present\":"
           << JsonBool(state.spectrum.present)
           << ",\"count\":"
           << state.spectrum.count;
    if (state.spectrum.present) {
        output << ",\"index\":"
               << state.spectrum.index
               << ",\"name\":"
               << JsonString(state.spectrum.name);
    }
    output << "},\"labeling\":{"
           << "\"has_active_task\":"
           << JsonBool(
                  state.labeling.has_active_task);
    if (state.labeling.has_active_task) {
        output << ",\"active_task\":{"
               << "\"id\":"
               << JsonString(state.labeling.task_id)
               << ",\"name\":"
               << JsonString(
                      state.labeling.task_name)
               << '}';
        if (state.spectrum.present) {
            output
                << ",\"current_spectrum_label\":{"
                << "\"code\":"
                << state.labeling
                       .current_spectrum_code
                << '}';
        }
    }
    output << "},\"capture\":{"
           << "\"pending\":"
           << JsonBool(state.capture.pending)
           << ",\"last_result\":"
           << JsonString(
                  state.capture.last_result);
    if (state.capture.pending &&
        !state.capture.current_path.empty()) {
        output << ",\"current_path\":"
               << JsonString(
                      PathToUtf8(
                          state.capture.current_path));
    }
    if (!state.capture.last_path.empty()) {
        output << ",\"last_path\":"
               << JsonString(
                      PathToUtf8(
                          state.capture.last_path));
    }
    output << "},\"profile\":{"
           << "\"status\":"
           << JsonString(state.profile.status)
           << ",\"stop_reason\":"
           << JsonString(
                  state.profile.stop_reason)
           << ",\"dropped_events\":"
           << state.profile.dropped_events;
    if (!state.profile.path.empty()) {
        output << ",\"path\":"
               << JsonString(
                      PathToUtf8(
                          state.profile.path));
    }
    output << "},\"runtime\":{"
           << "\"running\":"
           << JsonBool(state.runtime.running)
           << ",\"shutting_down\":"
           << JsonBool(
                  state.runtime.shutting_down)
           << ",\"frame_index\":"
           << state.runtime.frame_index
           << "}}";
    return output.str();
}

void CompleteAutomationProfileStopTerminal(
    AutomationNamedPipeServer& server,
    const AutomationQueuedCommand& command,
    ProfileSink::StopReason reason,
    const std::filesystem::path& path,
    std::uint64_t dropped_events)
{
    const AutomationProfileStopTerminalPolicy policy =
        AutomationProfileStopPolicy(reason);
    if (!policy.succeeded) {
        server.Fail(
            command,
            policy.error_code,
            reason == ProfileSink::StopReason::WriteFailure
                ? "The performance recording could not be finalized on disk."
                : "The performance recorder stopped without a terminal reason.");
        return;
    }

    std::ostringstream body;
    body << "\"result\":{"
         << "\"status\":\"succeeded\""
         << ",\"path\":"
         << JsonString(PathToUtf8(path))
         << ",\"stop_reason\":"
         << JsonString(
                ProfileSink::StopReasonName(reason))
         << ",\"dropped_events\":"
         << dropped_events
         << '}';
    server.Complete(
        command,
        body.str());
}

}  // namespace specforge
