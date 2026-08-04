#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <ios>
#include <ostream>
#include <string>
#include <string_view>

namespace specforge {

struct AtomicFileReplaceRetryPolicy {
    std::size_t maximum_attempts = 1;
    std::chrono::milliseconds initial_retry_delay{0};
    std::chrono::milliseconds maximum_retry_delay{0};
};

using AtomicFileWriteCheckpoint = std::function<void(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path)>;

struct AtomicFileWriteOptions {
    std::ios::openmode open_mode = std::ios::trunc;
    std::string_view target_description = "file";
    AtomicFileReplaceRetryPolicy replace_retry_policy;
    // Test-only interruption seam.  It runs after the temporary file is
    // closed and before the target is replaced; production callers leave it
    // empty.
    AtomicFileWriteCheckpoint before_replace;
};

using AtomicFileWriter = std::function<bool(std::ostream& stream, std::string& error)>;

[[nodiscard]] std::filesystem::path TemporarySiblingPath(const std::filesystem::path& target_path);
[[nodiscard]] bool ReplaceFileAtomically(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path,
    std::string* error_message = nullptr,
    std::string_view target_description = "file",
    AtomicFileReplaceRetryPolicy retry_policy = {});
[[nodiscard]] bool WriteFileAtomically(
    const std::filesystem::path& target_path,
    const AtomicFileWriteOptions& options,
    const AtomicFileWriter& writer,
    std::string* error_message = nullptr);

}  // namespace specforge
