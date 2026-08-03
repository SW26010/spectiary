#pragma once

#include <filesystem>
#include <string>

namespace specforge {

struct ExclusiveFileLeaseAcquireResult;

enum class ExclusiveFileLeaseAcquireStatus {
    Acquired,
    Unavailable,
    Failed,
};

class ExclusiveFileLease {
public:
    ExclusiveFileLease() = default;
    ~ExclusiveFileLease();

    ExclusiveFileLease(ExclusiveFileLease&& other) noexcept;
    ExclusiveFileLease& operator=(ExclusiveFileLease&& other) noexcept;
    ExclusiveFileLease(const ExclusiveFileLease&) = delete;
    ExclusiveFileLease& operator=(const ExclusiveFileLease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    void Reset() noexcept;

private:
    explicit ExclusiveFileLease(void* native_handle) noexcept;

    void* native_handle_ = nullptr;

    friend struct ExclusiveFileLeaseAcquireResult;
    friend ExclusiveFileLeaseAcquireResult TryAcquireExclusiveFileLease(
        const std::filesystem::path& path);
};

struct ExclusiveFileLeaseAcquireResult {
    ExclusiveFileLease lease;
    ExclusiveFileLeaseAcquireStatus status =
        ExclusiveFileLeaseAcquireStatus::Failed;
    std::string error;
};

// Opens a lock-file name with exclusive Windows sharing. Ownership is the live
// operating-system file handle, so process termination releases the lease
// without stale-owner recovery logic. Windows delete-on-close removes the
// directory entry on both clean release and process teardown.
[[nodiscard]] ExclusiveFileLeaseAcquireResult TryAcquireExclusiveFileLease(
    const std::filesystem::path& path);

}  // namespace specforge
