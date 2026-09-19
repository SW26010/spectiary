#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace spectiary::test_support {

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        static std::atomic_uint64_t next_id = 0;
        const auto parent = std::filesystem::temp_directory_path();
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::size_t attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = parent /
                ("spectiary-test-" + std::to_string(timestamp) + "-" +
                 std::to_string(next_id.fetch_add(1)));
            std::error_code error;
            // Creation claims ownership; an existing directory is never reused.
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error(
                    "could not create test temporary directory", candidate, error);
            }
        }
        throw std::runtime_error("could not claim a unique test temporary directory");
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

}  // namespace spectiary::test_support
