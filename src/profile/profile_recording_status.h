#pragma once

#include "profile/profile_sink.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace specforge {

[[nodiscard]] std::string ProfileRecordingStatusMessage(
    ProfileSink::StopReason stop_reason,
    std::uint64_t dropped_events,
    std::string_view error_message);

}  // namespace specforge
