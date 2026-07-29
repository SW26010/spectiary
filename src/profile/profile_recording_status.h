#pragma once

#include "profile/profile_sink.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace specforge {

enum class ProfileRecordingStatusKind {
    UseDiagnosticsToRecord,
    StartedByEnvironment,
    StartFailed,
    Recording,
    EventsDroppedUnderPressure,
    Finishing,
    Failed,
    FailedWhileWriting,
    SavedAfterDurationLimit,
    SavedAfterFileSizeLimit,
    Saved,
    Stopped,
};

struct ProfileRecordingStatus {
    ProfileRecordingStatusKind kind =
        ProfileRecordingStatusKind::UseDiagnosticsToRecord;
    std::uint64_t dropped_events = 0;
    std::string detail;
};

[[nodiscard]] ProfileRecordingStatus
DescribeProfileRecordingStop(
    ProfileSink::StopReason stop_reason,
    std::uint64_t dropped_events,
    std::string_view error_message);

}  // namespace specforge
