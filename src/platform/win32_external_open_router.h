#pragma once

#include <Windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace spectiary {

struct ExternalOpenRequest {
    std::filesystem::path path;
    bool as_folder = false;
};

[[nodiscard]] bool ActivateExternalOpenWindow(HWND window) noexcept;

// All instance state is transient and local to a Windows session and config
// namespace. Create, drain and destroy the endpoint on the GUI thread.
class Win32ExternalOpenRouter {
public:
    Win32ExternalOpenRouter();
    ~Win32ExternalOpenRouter();
    Win32ExternalOpenRouter(const Win32ExternalOpenRouter&) = delete;
    Win32ExternalOpenRouter& operator=(const Win32ExternalOpenRouter&) = delete;

    bool Start(const std::filesystem::path& config_root,
        std::function<bool()> activate, std::function<void()> wake) noexcept;
    void Stop() noexcept;
    void MarkUsed() noexcept;
    [[nodiscard]] std::vector<ExternalOpenRequest> TakeRequests();

    // False leaves ownership with the caller, which continues normal startup.
    // A timed-out sender atomically cancels the request before falling back.
    [[nodiscard]] static bool Forward(const std::filesystem::path& config_root,
        const ExternalOpenRequest& request, unsigned timeout_ms = 1500) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace spectiary
