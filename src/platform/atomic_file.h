#pragma once

#include <filesystem>
#include <functional>
#include <ios>
#include <ostream>
#include <string>
#include <string_view>

namespace specforge {

struct AtomicFileWriteOptions {
    std::ios::openmode open_mode = std::ios::trunc;
    std::string_view target_description = "file";
};

using AtomicFileWriter = std::function<bool(std::ostream& stream, std::string& error)>;

[[nodiscard]] std::filesystem::path TemporarySiblingPath(const std::filesystem::path& target_path);
[[nodiscard]] bool ReplaceFileAtomically(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path,
    std::string* error_message = nullptr,
    std::string_view target_description = "file");
[[nodiscard]] bool WriteFileAtomically(
    const std::filesystem::path& target_path,
    const AtomicFileWriteOptions& options,
    const AtomicFileWriter& writer,
    std::string* error_message = nullptr);

}  // namespace specforge
