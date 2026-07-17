#include "platform/win32_message_wait.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <system_error>

namespace specforge {

Win32MessageWaitWake WaitForWin32MessageOrDeadline(
    std::optional<std::chrono::steady_clock::time_point> deadline)
{
    DWORD timeout = INFINITE;
    if (deadline) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            *deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            return Win32MessageWaitWake::Deadline;
        }
        constexpr auto kMaximumFiniteWait = std::chrono::milliseconds(INFINITE - 1U);
        timeout = static_cast<DWORD>(std::min(remaining, kMaximumFiniteWait).count());
    }

    const DWORD result = MsgWaitForMultipleObjectsEx(
        0,
        nullptr,
        timeout,
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0) {
        return Win32MessageWaitWake::MessageInput;
    }
    if (result == WAIT_TIMEOUT) {
        return Win32MessageWaitWake::Deadline;
    }
    if (result == WAIT_FAILED) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "MsgWaitForMultipleObjectsEx");
    }
    throw std::runtime_error("MsgWaitForMultipleObjectsEx returned an unexpected wake reason");
}

}  // namespace specforge
