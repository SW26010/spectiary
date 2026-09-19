#include "app/imgui_layout_persistence.h"

#include "platform/atomic_file.h"

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace spectiary {
namespace {

constexpr std::size_t kMaximumLayoutBytes = 16U * 1024U * 1024U;

std::string_view Trim(std::string_view value) noexcept
{
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

bool IsSectionLine(std::string_view line) noexcept
{
    // Dear ImGui parses the first "[Type]" delimiter, then the first "["
    // after it, and treats the remainder up to the final "]" as the name.
    // In particular, a window name is allowed to contain additional ']'
    // characters (see LoadIniSettingsFromMemory()).
    if (line.size() < 5 || line.front() != '[' || line.back() != ']') {
        return false;
    }

    const std::size_t type_end = line.find(']', 1);
    if (type_end == std::string_view::npos || type_end == 1) {
        return false;
    }
    const std::size_t name_start = line.find('[', type_end + 1);
    return name_start != std::string_view::npos &&
           name_start < line.size() - 1;
}

bool IsInvalidControlByte(unsigned char byte) noexcept
{
    return byte == 0 || (byte < 0x20U && byte != '\t');
}

AtomicFileWriteOptions LayoutWriteOptions()
{
    AtomicFileWriteOptions options;
    options.open_mode = std::ios::binary | std::ios::trunc;
    options.target_description = "ImGui layout";
    options.replace_retry_policy.maximum_attempts = 7;
    options.replace_retry_policy.initial_retry_delay =
        std::chrono::milliseconds(1);
    options.replace_retry_policy.maximum_retry_delay =
        std::chrono::milliseconds(32);
    return options;
}

}  // namespace

ImGuiLayoutPersistence::ImGuiLayoutPersistence(
    std::filesystem::path target_path)
    : target_path_(std::move(target_path))
{
}

const std::filesystem::path& ImGuiLayoutPersistence::target_path()
    const noexcept
{
    return target_path_;
}

bool ImGuiLayoutPersistence::IsWellFormedSnapshot(
    std::string_view snapshot) noexcept
{
    if (snapshot.size() > kMaximumLayoutBytes) {
        return false;
    }

    bool saw_section = false;
    std::size_t line_start = 0;
    while (line_start <= snapshot.size()) {
        const std::size_t newline =
            snapshot.find('\n', line_start);
        const std::size_t line_end =
            newline == std::string_view::npos
                ? snapshot.size()
                : newline;
        std::string_view line =
            snapshot.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        for (const unsigned char byte : line) {
            if (IsInvalidControlByte(byte)) {
                return false;
            }
        }

        line = Trim(line);
        if (!line.empty() &&
            line.front() != ';' && line.front() != '#') {
            if (line.front() == '[') {
                if (!IsSectionLine(line)) {
                    return false;
                }
                saw_section = true;
            } else {
                if (!saw_section) {
                    return false;
                }
                const std::size_t equals = line.find('=');
                if (equals == std::string_view::npos ||
                    Trim(line.substr(0, equals)).empty() ||
                    Trim(line.substr(equals + 1)).empty()) {
                    return false;
                }
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
        line_start = newline + 1;
    }
    return true;
}

ImGuiLayoutLoadResult ImGuiLayoutPersistence::Load() const
{
    ImGuiLayoutLoadResult result;
    if (target_path_.empty()) {
        result.error = "ImGui layout path is empty";
        return result;
    }
    if (ImGui::GetCurrentContext() == nullptr) {
        result.error = "ImGui context is not available";
        return result;
    }

    std::error_code size_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(target_path_, size_error);
    if (!size_error && file_size > kMaximumLayoutBytes) {
        result.status = ImGuiLayoutLoadStatus::Malformed;
        result.error = "ImGui layout is too large";
        return result;
    }

    std::ifstream stream(target_path_, std::ios::binary);
    if (!stream.is_open()) {
        std::error_code exists_error;
        const bool exists =
            std::filesystem::exists(target_path_, exists_error);
        if (!exists && !exists_error) {
            result.status = ImGuiLayoutLoadStatus::Missing;
            return result;
        }
        result.error = "could not read ImGui layout: " +
                       target_path_.string();
        return result;
    }

    std::ostringstream contents;
    contents << stream.rdbuf();
    if (stream.bad() || (stream.fail() && !stream.eof())) {
        result.error = "could not read ImGui layout: " +
                       target_path_.string();
        return result;
    }
    const std::string snapshot = contents.str();
    if (!IsWellFormedSnapshot(snapshot)) {
        result.status = ImGuiLayoutLoadStatus::Malformed;
        result.error = "ImGui layout is malformed";
        return result;
    }

    if (!snapshot.empty()) {
        ImGui::LoadIniSettingsFromMemory(
            snapshot.data(),
            snapshot.size());
    }
    result.status = ImGuiLayoutLoadStatus::Loaded;
    return result;
}

bool ImGuiLayoutPersistence::SaveSnapshot(
    std::string_view snapshot,
    std::string* error) const
{
    if (error != nullptr) {
        error->clear();
    }
    if (target_path_.empty()) {
        if (error != nullptr) {
            *error = "ImGui layout path is empty";
        }
        return false;
    }
    if (!IsWellFormedSnapshot(snapshot)) {
        if (error != nullptr) {
            *error = "refusing to replace ImGui layout with malformed data";
        }
        return false;
    }
    if (snapshot.size() >
        static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max())) {
        if (error != nullptr) {
            *error = "ImGui layout is too large to write";
        }
        return false;
    }

    const AtomicFileWriteOptions options = LayoutWriteOptions();
    return WriteFileAtomically(
        target_path_,
        options,
        [snapshot](std::ostream& stream, std::string&) {
            stream.write(
                snapshot.data(),
                static_cast<std::streamsize>(snapshot.size()));
            return stream.good();
        },
        error);
}

bool ImGuiLayoutPersistence::SaveIfRequested(
    std::string* error) const
{
    if (ImGui::GetCurrentContext() == nullptr) {
        if (error != nullptr) {
            *error = "ImGui context is not available";
        }
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantSaveIniSettings) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    std::size_t snapshot_size = 0;
    const char* snapshot =
        ImGui::SaveIniSettingsToMemory(&snapshot_size);
    if (!SaveSnapshot(
            std::string_view(snapshot, snapshot_size),
            error)) {
        return false;
    }
    io.WantSaveIniSettings = false;
    return true;
}

bool ImGuiLayoutPersistence::SaveNow(std::string* error) const
{
    if (ImGui::GetCurrentContext() == nullptr) {
        if (error != nullptr) {
            *error = "ImGui context is not available";
        }
        return false;
    }

    std::size_t snapshot_size = 0;
    const char* snapshot =
        ImGui::SaveIniSettingsToMemory(&snapshot_size);
    if (!SaveSnapshot(
            std::string_view(snapshot, snapshot_size),
            error)) {
        return false;
    }
    ImGui::GetIO().WantSaveIniSettings = false;
    return true;
}

}  // namespace spectiary
