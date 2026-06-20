#include "platform/atomic_file.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace specforge {
namespace {

void SetError(std::string* error_message, std::string message)
{
    if (error_message != nullptr) {
        *error_message = std::move(message);
    }
}

std::string DescriptionText(std::string_view description)
{
    return description.empty() ? std::string("file") : std::string(description);
}

void RemoveTemporaryFile(const std::filesystem::path& temporary_path)
{
    std::error_code remove_error;
    std::filesystem::remove(temporary_path, remove_error);
}

}  // namespace

std::filesystem::path TemporarySiblingPath(const std::filesystem::path& target_path)
{
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path temporary = target_path;
    temporary += ".tmp.";
#ifdef _WIN32
    temporary += std::to_string(GetCurrentProcessId());
#else
    temporary += "pid";
#endif
    temporary += ".";
    temporary += std::to_string(timestamp);
    return temporary;
}

bool ReplaceFileAtomically(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path,
    std::string* error_message,
    std::string_view target_description)
{
#ifdef _WIN32
    if (MoveFileExW(
            temporary_path.c_str(),
            target_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    SetError(
        error_message,
        "could not replace " + DescriptionText(target_description) + ": " +
            std::system_category().message(GetLastError()));
    return false;
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary_path, target_path, rename_error);
    if (!rename_error) {
        return true;
    }
    SetError(
        error_message,
        "could not replace " + DescriptionText(target_description) + ": " + rename_error.message());
    return false;
#endif
}

bool WriteFileAtomically(
    const std::filesystem::path& target_path,
    const AtomicFileWriteOptions& options,
    const AtomicFileWriter& writer,
    std::string* error_message)
{
    if (error_message != nullptr) {
        error_message->clear();
    }

    const std::string description = DescriptionText(options.target_description);
    if (target_path.empty()) {
        SetError(error_message, description + " path is empty");
        return false;
    }

    std::error_code filesystem_error;
    const std::filesystem::path parent = target_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            SetError(error_message, "could not create " + description + " directory: " + filesystem_error.message());
            return false;
        }
    }

    const std::filesystem::path temporary_path = TemporarySiblingPath(target_path);
    std::ofstream stream(temporary_path, options.open_mode);
    if (!stream.good()) {
        SetError(error_message, "could not open temporary " + description + " for writing: " + temporary_path.string());
        return false;
    }

    std::string writer_error;
    if (!writer(stream, writer_error)) {
        stream.close();
        RemoveTemporaryFile(temporary_path);
        SetError(
            error_message,
            writer_error.empty() ? "could not write temporary " + description + ": " + temporary_path.string()
                                 : std::move(writer_error));
        return false;
    }

    if (!stream.good()) {
        stream.close();
        RemoveTemporaryFile(temporary_path);
        SetError(error_message, "could not write temporary " + description + ": " + temporary_path.string());
        return false;
    }
    stream.close();
    if (!stream.good()) {
        RemoveTemporaryFile(temporary_path);
        SetError(error_message, "could not close temporary " + description + ": " + temporary_path.string());
        return false;
    }

    if (!ReplaceFileAtomically(temporary_path, target_path, error_message, description)) {
        RemoveTemporaryFile(temporary_path);
        return false;
    }
    return true;
}

}  // namespace specforge
