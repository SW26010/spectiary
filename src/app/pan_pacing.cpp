#include "app/pan_pacing.h"

#include <cstdlib>
#include <memory>

namespace specforge {

PanPacingConfiguration ResolvePanPacing(
    std::optional<std::string_view> requested)
{
    if (!requested) {
        return {};
    }

    PanPacingConfiguration configuration;
    configuration.requested = std::string(*requested);
    if (*requested == "uncapped") {
        configuration.effective = PanPacingMode::Uncapped;
        return configuration;
    }
    if (*requested == "display") {
        return configuration;
    }

    configuration.recognized = false;
    return configuration;
}

PanPacingConfiguration ResolvePanPacingEnvironment()
{
    char* requested_buffer = nullptr;
    std::size_t requested_size = 0;
    if (_dupenv_s(
            &requested_buffer,
            &requested_size,
            "SPECFORGE_PAN_PACING") != 0) {
        return {};
    }
    const std::unique_ptr<char, decltype(&std::free)> requested(
        requested_buffer,
        &std::free);
    return ResolvePanPacing(
        requested
            ? std::optional<std::string_view>(requested.get())
            : std::nullopt);
}

const char* PanPacingModeName(PanPacingMode mode) noexcept
{
    return mode == PanPacingMode::Uncapped ? "uncapped" : "display";
}

}  // namespace specforge
