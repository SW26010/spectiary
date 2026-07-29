#include "app/on_demand_frame_capture.h"

#include <cstdlib>
#include <memory>
#include <utility>

namespace specforge {
namespace {

constexpr std::string_view kUnavailableMessage =
    "Capture unavailable while the main window is minimized or hidden.";

}  // namespace

OnDemandFrameCaptureConfiguration ResolveOnDemandFrameCapture(
    std::optional<std::string_view> requested)
{
    if (!requested) {
        return {};
    }

    OnDemandFrameCaptureConfiguration configuration;
    configuration.requested = std::string(*requested);
    if (*requested == "1") {
        configuration.enabled = true;
        return configuration;
    }

    configuration.recognized = false;
    return configuration;
}

OnDemandFrameCaptureConfiguration
ResolveOnDemandFrameCaptureEnvironment()
{
    char* requested_buffer = nullptr;
    std::size_t requested_size = 0;
    if (_dupenv_s(
            &requested_buffer,
            &requested_size,
            "SPECFORGE_FRAME_CAPTURE") != 0) {
        return {};
    }
    const std::unique_ptr<char, decltype(&std::free)> requested(
        requested_buffer,
        &std::free);
    return ResolveOnDemandFrameCapture(
        requested
            ? std::optional<std::string_view>(requested.get())
            : std::nullopt);
}

OnDemandFrameCapture::OnDemandFrameCapture(
    OnDemandFrameCaptureConfiguration configuration)
    : configuration_(std::move(configuration))
{
    if (configuration_.enabled) {
        status_ = OnDemandFrameCaptureStatus::Ready;
        status_message_ =
            "Ready. Captures are synchronous and may disturb performance measurements.";
    }
}

OnDemandFrameCaptureRequestOutcome
OnDemandFrameCapture::Request(
    std::uint64_t current_frame,
    bool window_renderable)
{
    if (!configuration_.enabled) {
        status_ = OnDemandFrameCaptureStatus::Disabled;
        status_operation_.clear();
        status_result_.clear();
        status_message_ =
            "Experimental frame capture is not enabled.";
        return OnDemandFrameCaptureRequestOutcome::Disabled;
    }
    if (!window_renderable) {
        requested_after_frame_.reset();
        last_output_path_.reset();
        status_ =
            OnDemandFrameCaptureStatus::
                WindowUnavailable;
        status_operation_.clear();
        status_result_.clear();
        status_message_ = kUnavailableMessage;
        return OnDemandFrameCaptureRequestOutcome::WindowNotRenderable;
    }
    if (requested_after_frame_) {
        return OnDemandFrameCaptureRequestOutcome::AlreadyPending;
    }

    requested_after_frame_ = current_frame;
    last_output_path_.reset();
    status_ = OnDemandFrameCaptureStatus::Pending;
    status_operation_.clear();
    status_result_.clear();
    status_message_ =
        "Capture requested. Waiting for the next successfully drawn main frame.";
    return OnDemandFrameCaptureRequestOutcome::Accepted;
}

void OnDemandFrameCapture::ObserveWindowRenderable(
    bool window_renderable)
{
    if (window_renderable || !requested_after_frame_) {
        return;
    }

    requested_after_frame_.reset();
    last_output_path_.reset();
    status_ =
        OnDemandFrameCaptureStatus::
            WindowUnavailable;
    status_operation_.clear();
    status_result_.clear();
    status_message_ = kUnavailableMessage;
}

bool OnDemandFrameCapture::ShouldCapture(
    std::uint64_t current_frame) const noexcept
{
    return requested_after_frame_ &&
           current_frame > *requested_after_frame_;
}

void OnDemandFrameCapture::Complete(
    std::filesystem::path output_path)
{
    requested_after_frame_.reset();
    last_output_path_ = std::move(output_path);
    status_ = OnDemandFrameCaptureStatus::Captured;
    status_operation_.clear();
    status_result_.clear();
    status_message_ = "Captured the requested main application frame.";
}

void OnDemandFrameCapture::Fail(std::string message)
{
    requested_after_frame_.reset();
    last_output_path_.reset();
    status_ = OnDemandFrameCaptureStatus::Failed;
    status_operation_.clear();
    status_result_.clear();
    status_message_ =
        message.empty()
            ? "Frame capture failed; no image was produced."
            : std::move(message);
}

void OnDemandFrameCapture::FailPreparingOutputDirectory()
{
    requested_after_frame_.reset();
    last_output_path_.reset();
    status_ =
        OnDemandFrameCaptureStatus::
            FailedPreparingOutputDirectory;
    status_operation_.clear();
    status_result_.clear();
    status_message_ =
        "Frame capture failed while preparing the output directory; no image was produced.";
}

void OnDemandFrameCapture::FailCapture(
    std::string operation,
    std::string result)
{
    requested_after_frame_.reset();
    last_output_path_.reset();
    status_ =
        OnDemandFrameCaptureStatus::FailedCapture;
    status_operation_ = std::move(operation);
    status_result_ = std::move(result);
    status_message_ =
        "Frame capture failed at " +
        status_operation_ + " (" +
        status_result_ +
        "); no image was produced.";
}

}  // namespace specforge
