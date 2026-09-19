#include "automation/automation_state.h"



namespace spectiary {
namespace {

std::string PathToUtf8(
    const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return std::string(
        utf8.begin(),
        utf8.end());
}

}  // namespace

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

    server.Complete(command,
        AutomationProfileStopResult{PathToUtf8(path), reason, dropped_events});

}

}  // namespace spectiary
