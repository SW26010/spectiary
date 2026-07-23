#include "platform/directory_change_generation.h"

#if defined(_WIN32)

#include <windows.h>

namespace specforge {
namespace {

class Win32DirectoryChangeGeneration final : public DirectoryChangeGeneration {
public:
    explicit Win32DirectoryChangeGeneration(HANDLE change_notification) noexcept
        : change_notification_(change_notification)
    {
    }

    ~Win32DirectoryChangeGeneration() override
    {
        if (change_notification_ != INVALID_HANDLE_VALUE) {
            (void)FindCloseChangeNotification(change_notification_);
        }
    }

    [[nodiscard]] bool IsCurrent() const noexcept override
    {
        return WaitForSingleObject(change_notification_, 0) == WAIT_TIMEOUT;
    }

private:
    HANDLE change_notification_ = INVALID_HANDLE_VALUE;
};

}  // namespace

DirectoryChangeGenerationHandle BeginDirectoryChangeGeneration(
    const std::filesystem::path& path)
{
    constexpr DWORD kChangeFilter =
        FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES |
        FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_CREATION |
        FILE_NOTIFY_CHANGE_SECURITY;
    const HANDLE change_notification =
        FindFirstChangeNotificationW(path.c_str(), FALSE, kChangeFilter);
    if (change_notification == INVALID_HANDLE_VALUE) {
        return {};
    }
    return std::make_shared<Win32DirectoryChangeGeneration>(change_notification);
}

}  // namespace specforge

#else

namespace specforge {

DirectoryChangeGenerationHandle BeginDirectoryChangeGeneration(
    const std::filesystem::path&)
{
    return {};
}

}  // namespace specforge

#endif
