#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>

namespace specforge {

enum class SourceOpenOrigin {
    InApp,
    ExternalStartup,
    Automation,
};

struct SourceOpenRequest {
    std::filesystem::path source_path;
    SourceOpenOrigin origin = SourceOpenOrigin::InApp;
    bool open_external_fits_as_folder = false;
};

struct SourceOpenFilesystemProbe {
    std::filesystem::path parent_path;
    std::filesystem::file_status parent_status;
    std::error_code parent_error;
    std::filesystem::file_status source_status;
    std::error_code source_error;
};

enum class SourceOpenResolutionKind {
    Direct,
    Folder,
    Failed,
};

enum class SourceOpenResolutionFailure {
    EmptySourcePath,
    SourcePathUnavailable,
    SourcePathNotRegularFile,
    ParentPathUnavailable,
    ParentPathNotDirectory,
};

struct SourceOpenResolution {
    // Direct keeps the requested source path unchanged. Folder is the
    // existing first-level folder-source contract and carries the requested
    // FITS member separately.
    SourceOpenResolutionKind kind =
        SourceOpenResolutionKind::Failed;
    std::filesystem::path source_path;
    std::optional<std::filesystem::path> preferred_member_path;
    std::optional<SourceOpenResolutionFailure> failure;
    std::string diagnostic;

    [[nodiscard]] bool failed() const noexcept
    {
        return kind == SourceOpenResolutionKind::Failed;
    }
};

[[nodiscard]] bool SourceOpenRequestExpandsAsFolder(
    const SourceOpenRequest& request) noexcept;
[[nodiscard]] std::filesystem::path SourceOpenRequestCandidatePath(
    const SourceOpenRequest& request);
[[nodiscard]] SourceOpenFilesystemProbe ProbeSourceOpenRequest(
    const SourceOpenRequest& request,
    const std::function<void()>& cancellation_checkpoint = {});
[[nodiscard]] SourceOpenResolution ResolveSourceOpenRequest(
    const SourceOpenRequest& request,
    const SourceOpenFilesystemProbe& probe);

}  // namespace specforge
