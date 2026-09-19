#pragma once

#include <chrono>
#include <optional>

namespace spectiary {

enum class Win32MessageWaitWake {
    Deadline,
    MessageInput,
};

[[nodiscard]] Win32MessageWaitWake WaitForWin32MessageOrDeadline(
    std::optional<std::chrono::steady_clock::time_point> deadline);

}  // namespace spectiary
