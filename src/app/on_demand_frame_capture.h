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

class OnDemandFrameCapture {
public:
    OnDemandFrameCapture() = default;
    explicit OnDemandFrameCapture(
        OnDemandFrameCaptureConfiguration configuration);

    [[nodiscard]] OnDemandFrameCaptureRequestOutcome Request(
        std::uint64_t current_frame,
        bool window_renderable);
    void ObserveWindowRenderable(bool window_renderable);
    [[nodiscard]] bool ShouldCapture(
        std::uint64_t current_frame) const noexcept;
    void Complete(std::filesystem::path output_path);
    void Fail(std::string message);

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
    [[nodiscard]] const std::optional<std::filesystem::path>&
    last_output_path() const noexcept
    {
        return last_output_path_;
    }

private:
    OnDemandFrameCaptureConfiguration configuration_;
    std::optional<std::uint64_t> requested_after_frame_;
    std::optional<std::filesystem::path> last_output_path_;
    std::string status_message_;
};

}  // namespace specforge
