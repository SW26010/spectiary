#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>

namespace spectiary {

class DirectoryChangeGenerationMonitor;

// A one-shot observation boundary for a directory. Once any relevant
// first-level entry changes, this generation remains invalid forever.
class DirectoryChangeGeneration {
public:
    virtual ~DirectoryChangeGeneration() = default;

    [[nodiscard]] virtual bool IsCurrent() const noexcept = 0;

private:
    // Only the monitor closes a registration. Published handles are const, so
    // callers can observe but cannot race the owner-controlled shutdown.
    virtual void Close() noexcept = 0;

    friend class DirectoryChangeGenerationMonitor;
};

using DirectoryChangeGenerationHandle = std::shared_ptr<const DirectoryChangeGeneration>;

// Exclusively owns the thread on which platform change-notification
// registrations are created and closed. Published generations retain only a
// registration lease; destroying the monitor safely invalidates those leases
// before joining its worker. The registration thread must outlive the leases:
// exiting that thread can signal an unchanged directory's native notification.
class DirectoryChangeGenerationMonitor {
public:
    using RegistrationFactory = std::function<std::shared_ptr<DirectoryChangeGeneration>(
        const std::filesystem::path&,
        std::stop_token)>;

    DirectoryChangeGenerationMonitor();
    explicit DirectoryChangeGenerationMonitor(RegistrationFactory registration_factory);
    ~DirectoryChangeGenerationMonitor();

    DirectoryChangeGenerationMonitor(const DirectoryChangeGenerationMonitor&) = delete;
    DirectoryChangeGenerationMonitor& operator=(const DirectoryChangeGenerationMonitor&) = delete;
    DirectoryChangeGenerationMonitor(DirectoryChangeGenerationMonitor&&) = delete;
    DirectoryChangeGenerationMonitor& operator=(DirectoryChangeGenerationMonitor&&) = delete;

    // The token wakes the wait; the checkpoint translates cancellation into
    // the caller's exception. Cancellable callers must supply both. Timeout
    // returns an unavailable generation without canceling an in-flight OS call.
    [[nodiscard]] DirectoryChangeGenerationHandle Begin(
        const std::filesystem::path& path,
        const std::function<void()>& cancellation_checkpoint,
        std::stop_token cancellation_token = {});

private:
    static void CloseRegistration(DirectoryChangeGeneration& registration) noexcept;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spectiary
