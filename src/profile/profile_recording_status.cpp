#include "profile/profile_recording_status.h"

namespace spectiary {

ProfileRecordingStatus DescribeProfileRecordingStop(
    ProfileSink::StopReason stop_reason,
    std::uint64_t dropped_events,
    std::string_view error_message)
{
    ProfileRecordingStatus status = {
        .dropped_events = dropped_events,
    };
    switch (stop_reason) {
    case ProfileSink::StopReason::WriteFailure:
        status.kind = error_message.empty()
            ? ProfileRecordingStatusKind::FailedWhileWriting
            : ProfileRecordingStatusKind::Failed;
        status.detail = error_message;
        break;
    case ProfileSink::StopReason::DurationLimit:
        status.kind =
            ProfileRecordingStatusKind::SavedAfterDurationLimit;
        break;
    case ProfileSink::StopReason::FileSizeLimit:
        status.kind =
            ProfileRecordingStatusKind::SavedAfterFileSizeLimit;
        break;
    case ProfileSink::StopReason::Explicit:
        status.kind = ProfileRecordingStatusKind::Saved;
        break;
    case ProfileSink::StopReason::None:
        status.kind = ProfileRecordingStatusKind::Stopped;
        break;
    }
    return status;
}

}  // namespace spectiary
