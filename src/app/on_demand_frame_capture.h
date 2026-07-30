#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace specforge {

struct OnDemandFrameCaptureConfiguration {
    std::string requested;
    bool enabled = false;
    bool recognized = true;
};

[[nodiscard]] OnDemandFrameCaptureConfiguration
ResolveOnDemandFrameCapture(
    std::optional<std::string_view> requested);
[[nodiscard]] OnDemandFrameCaptureConfiguration
ResolveOnDemandFrameCaptureEnvironment();

enum class OnDemandFrameCaptureRequestOutcome {
    Accepted,
    Disabled,
    WindowNotRenderable,
    AlreadyPending,
};

enum class OnDemandFrameCaptureStatus {
    None,
    Ready,
    Disabled,
    WindowUnavailable,
    Pending,
    Captured,
    Canceled,
    FailedPreparingOutputDirectory,
    FailedCapture,
    Failed,
};

class OnDemandFrameCapture {
public:
    OnDemandFrameCapture() = default;
    explicit OnDemandFrameCapture(
        OnDemandFrameCaptureConfiguration configuration);

    [[nodiscard]] OnDemandFrameCaptureRequestOutcome Request(
        std::uint64_t current_frame,
        bool window_renderable,
        std::optional<std::filesystem::path>
            output_path = std::nullopt);
    void ObserveWindowRenderable(bool window_renderable);
    [[nodiscard]] bool ShouldCapture(
        std::uint64_t current_frame) const noexcept;
    void Complete(std::filesystem::path output_path);
    void Cancel(
        std::string message =
            "Frame capture was canceled; no image was produced.");
    void Fail(std::string message);
    void FailPreparingOutputDirectory();
    void FailCapture(
        std::string operation,
        std::string result);

    [[nodiscard]] bool enabled() const noexcept
    {
        return configuration_.enabled;
    }
    [[nodiscard]] bool pending() const noexcept
    {
        return requested_after_frame_.has_value();
    }
    [[nodiscard]] const OnDemandFrameCaptureConfiguration&
    configuration() const noexcept
    {
        return configuration_;
    }
    [[nodiscard]] std::string_view status_message() const noexcept
    {
        return status_message_;
    }
    [[nodiscard]] OnDemandFrameCaptureStatus
    status() const noexcept
    {
        return status_;
    }
    [[nodiscard]] std::string_view
    status_operation() const noexcept
    {
        return status_operation_;
    }
    [[nodiscard]] std::string_view
    status_result() const noexcept
    {
        return status_result_;
    }
    [[nodiscard]] const std::optional<std::filesystem::path>&
    last_output_path() const noexcept
    {
        return last_output_path_;
    }
    [[nodiscard]] const std::optional<std::filesystem::path>&
    requested_output_path() const noexcept
    {
        return requested_output_path_;
    }

private:
    OnDemandFrameCaptureConfiguration configuration_;
    std::optional<std::uint64_t> requested_after_frame_;
    std::optional<std::filesystem::path>
        requested_output_path_;
    std::optional<std::filesystem::path> last_output_path_;
    std::string status_message_;
    OnDemandFrameCaptureStatus status_ =
        OnDemandFrameCaptureStatus::None;
    std::string status_operation_;
    std::string status_result_;
};

}  // namespace specforge
