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
    output << "},\"window\":{"
           << "\"visible\":"
           << JsonBool(state.window.visible)
           << ",\"minimized\":"
           << JsonBool(state.window.minimized)
           << ",\"client_width\":"
           << state.window.client_width
           << ",\"client_height\":"
           << state.window.client_height
           << "},\"runtime\":{"
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

}  // namespace specforge
