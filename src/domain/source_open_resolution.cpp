#include "domain/source_open_resolution.h"

#include "domain/spectrum_loader_support.h"

#include <system_error>
#include <utility>

namespace specforge {
namespace {

std::string PathText(const std::filesystem::path& path)
{
    return detail::PathToUtf8(path);
}

SourceOpenResolution MakeDirectResolution(
    const SourceOpenRequest& request)
{
    return {
        .kind = SourceOpenResolutionKind::Direct,
        .source_path = request.source_path,
    };
}

SourceOpenResolution MakeFailure(
    const SourceOpenRequest& request,
    SourceOpenResolutionFailure failure,
    std::string diagnostic)
{
    return {
        .kind = SourceOpenResolutionKind::Failed,
        .source_path = request.source_path,
        .failure = failure,
        .diagnostic = std::move(diagnostic),
    };
}

bool IsSupportedFitsSourcePath(
    const std::filesystem::path& path)
{
    const std::string format = detail::SourceFormatLabel(path);
    return format == "fits" || format == "fits.gz";
}

}  // namespace

SourceOpenResolution ResolveSourceOpenRequest(
    const SourceOpenRequest& request)
{
    if (request.source_path.empty()) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::EmptySourcePath,
            "The source path is empty.");
    }

    const bool should_expand_as_folder =
        request.origin == SourceOpenOrigin::ExternalStartup &&
        request.open_external_fits_as_folder &&
        IsSupportedFitsSourcePath(request.source_path);
    if (!should_expand_as_folder) {
        std::error_code source_error;
        const std::filesystem::file_status source_status =
            std::filesystem::status(request.source_path, source_error);
        if (source_error ||
            source_status.type() == std::filesystem::file_type::not_found) {
            return MakeFailure(
                request,
                SourceOpenResolutionFailure::SourcePathUnavailable,
                "The source path does not exist or cannot be accessed: " +
                    PathText(request.source_path));
        }
        return MakeDirectResolution(request);
    }

    std::error_code parent_error;
    std::filesystem::path parent_path =
        request.source_path.parent_path();
    if (parent_path.empty()) {
        parent_path = std::filesystem::current_path(parent_error);
    }
    if (parent_error || parent_path.empty()) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::ParentPathUnavailable,
            "The external FITS source parent folder is unavailable: " +
                PathText(request.source_path));
    }

    const std::filesystem::file_status parent_status =
        std::filesystem::status(parent_path, parent_error);
    if (parent_error ||
        parent_status.type() == std::filesystem::file_type::not_found) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::ParentPathUnavailable,
            "The external FITS source parent folder does not exist or "
            "cannot be accessed: " +
                PathText(parent_path));
    }
    if (parent_status.type() !=
        std::filesystem::file_type::directory) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::ParentPathNotDirectory,
            "The external FITS source parent path is not a folder: " +
                PathText(parent_path));
    }

    std::error_code source_error;
    const std::filesystem::file_status source_status =
        std::filesystem::status(request.source_path, source_error);
    if (source_error ||
        source_status.type() == std::filesystem::file_type::not_found) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::SourcePathUnavailable,
            "The source path does not exist or cannot be accessed: " +
                PathText(request.source_path));
    }
    if (source_status.type() !=
        std::filesystem::file_type::regular) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::SourcePathNotRegularFile,
            "The external FITS source is not a regular file: " +
                PathText(request.source_path));
    }

    SourceOpenResolution resolution =
        MakeDirectResolution(request);
    resolution.kind = SourceOpenResolutionKind::Folder;
    resolution.source_path = parent_path;
    resolution.preferred_member_path = request.source_path;
    return resolution;
}

}  // namespace specforge
