#pragma once

#include "platform/atomic_file.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

namespace specforge {

using AtomicFileReplaceOperation = std::function<std::error_code(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path)>;
using AtomicFileRetryWait =
    std::function<void(std::chrono::milliseconds delay)>;

// Deterministic seam shared by the platform implementation and its regression
// tests. Production callers should use ReplaceFileAtomically.
[[nodiscard]] bool ReplaceFileAtomicallyWithOperation(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path,
    AtomicFileReplaceRetryPolicy retry_policy,
    const AtomicFileReplaceOperation& replace_operation,
    const AtomicFileRetryWait& retry_wait,
    std::string* error_message = nullptr,
    std::string_view target_description = "file");

}  // namespace specforge
