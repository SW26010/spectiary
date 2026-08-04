#pragma once

#include <filesystem>
#include <optional>
#include <string>

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

[[nodiscard]] SourceOpenResolution ResolveSourceOpenRequest(
    const SourceOpenRequest& request);

}  // namespace specforge
