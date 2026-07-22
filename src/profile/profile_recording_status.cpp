#include "profile/profile_recording_status.h"

namespace specforge {

std::string ProfileRecordingStatusMessage(
    ProfileSink::StopReason stop_reason,
    std::uint64_t dropped_events,
    std::string_view error_message)
{
    if (stop_reason == ProfileSink::StopReason::WriteFailure) {
        if (!error_message.empty()) {
            return "Recording failed: " + std::string(error_message);
        }
        return "Recording failed while writing the log.";
    }

    std::string message;
    switch (stop_reason) {
    case ProfileSink::StopReason::DurationLimit:
        message = "Recording saved after reaching the 5-minute limit.";
        break;
    case ProfileSink::StopReason::FileSizeLimit:
        message = "Recording saved after reaching the 100 MiB limit.";
        break;
    case ProfileSink::StopReason::Explicit:
        message = "Recording saved.";
        break;
    case ProfileSink::StopReason::None:
        message = "Recording stopped.";
        break;
    case ProfileSink::StopReason::WriteFailure:
        break;
    }

    if (dropped_events != 0) {
        message += " " + std::to_string(dropped_events) + " events were dropped.";
    }
    return message;
}

}  // namespace specforge
