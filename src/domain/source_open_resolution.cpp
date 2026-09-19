#include "domain/source_open_resolution.h"

#include "domain/spectrum_loader_support.h"

#include <system_error>
#include <utility>

namespace spectiary {
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

void Checkpoint(
    const std::function<void()>& cancellation_checkpoint)
{
    if (cancellation_checkpoint) {
        cancellation_checkpoint();
    }
}

}  // namespace

bool SourceOpenRequestExpandsAsFolder(
    const SourceOpenRequest& request) noexcept
{
    return request.origin == SourceOpenOrigin::ExternalStartup &&
           request.open_external_source_as_folder &&
           detail::IsSupportedSingleFileSpectrumPath(request.source_path);
}

std::filesystem::path SourceOpenRequestCandidatePath(
    const SourceOpenRequest& request)
{
    if (!SourceOpenRequestExpandsAsFolder(request)) {
        return request.source_path;
    }
    std::filesystem::path parent_path =
        request.source_path.parent_path();
    if (!parent_path.empty()) {
        return parent_path;
    }
    std::error_code current_path_error;
    parent_path = std::filesystem::current_path(
        current_path_error);
    return parent_path.empty() || current_path_error
        ? std::filesystem::path{"."}
        : parent_path;
}

SourceOpenFilesystemProbe ProbeSourceOpenRequest(
    const SourceOpenRequest& request,
    const std::function<void()>& cancellation_checkpoint)
{
    SourceOpenFilesystemProbe probe;
    if (request.source_path.empty()) {
        return probe;
    }

    if (SourceOpenRequestExpandsAsFolder(request)) {
        Checkpoint(cancellation_checkpoint);
        probe.parent_path = request.source_path.parent_path();
        if (probe.parent_path.empty()) {
            probe.parent_path = std::filesystem::current_path(
                probe.parent_error);
        }
        Checkpoint(cancellation_checkpoint);
        if (!probe.parent_error && !probe.parent_path.empty()) {
            probe.parent_status = std::filesystem::status(
                probe.parent_path,
                probe.parent_error);
        }
        Checkpoint(cancellation_checkpoint);
        if (probe.parent_error || probe.parent_path.empty()) {
            return probe;
        }
    }

    probe.source_status = std::filesystem::status(
        request.source_path,
        probe.source_error);
    Checkpoint(cancellation_checkpoint);
    return probe;
}

SourceOpenResolution ResolveSourceOpenRequest(
    const SourceOpenRequest& request,
    const SourceOpenFilesystemProbe& probe)
{
    if (request.source_path.empty()) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::EmptySourcePath,
            "The source path is empty.");
    }

    const bool should_expand_as_folder =
        SourceOpenRequestExpandsAsFolder(request);
    if (!should_expand_as_folder) {
        if (probe.source_error ||
            probe.source_status.type() ==
                std::filesystem::file_type::not_found) {
            return MakeFailure(
                request,
                SourceOpenResolutionFailure::SourcePathUnavailable,
                "The source path does not exist or cannot be accessed: " +
                    PathText(request.source_path));
        }
        return MakeDirectResolution(request);
    }

    const std::filesystem::path& parent_path =
        probe.parent_path;
    if (probe.parent_error || parent_path.empty()) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::ParentPathUnavailable,
            "The external source parent folder is unavailable: " +
                PathText(request.source_path));
    }

    if (probe.parent_error ||
        probe.parent_status.type() ==
            std::filesystem::file_type::not_found) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::ParentPathUnavailable,
            "The external source parent folder does not exist or "
            "cannot be accessed: " +
                PathText(parent_path));
    }
    if (probe.parent_status.type() !=
        std::filesystem::file_type::directory) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::ParentPathNotDirectory,
            "The external source parent path is not a folder: " +
                PathText(parent_path));
    }

    if (probe.source_error ||
        probe.source_status.type() ==
            std::filesystem::file_type::not_found) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::SourcePathUnavailable,
            "The source path does not exist or cannot be accessed: " +
                PathText(request.source_path));
    }
    if (probe.source_status.type() !=
        std::filesystem::file_type::regular) {
        return MakeFailure(
            request,
            SourceOpenResolutionFailure::SourcePathNotRegularFile,
            "The external source is not a regular file: " +
                PathText(request.source_path));
    }

    SourceOpenResolution resolution =
        MakeDirectResolution(request);
    resolution.kind = SourceOpenResolutionKind::Folder;
    resolution.source_path = parent_path;
    resolution.preferred_member_path = request.source_path;
    return resolution;
}

}  // namespace spectiary
