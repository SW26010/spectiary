#pragma once

#include <cstddef>
#include <vector>

namespace specforge {

enum class SpectrumSmoothingMethod {
    None,
    Gaussian,
    Median,
};

struct SpectrumSmoothingSettings {
    SpectrumSmoothingMethod method = SpectrumSmoothingMethod::None;
    double gaussian_sigma = 0.25;
    int median_kernel_size = 7;
};

[[nodiscard]] bool operator==(const SpectrumSmoothingSettings& left, const SpectrumSmoothingSettings& right);
[[nodiscard]] bool operator!=(const SpectrumSmoothingSettings& left, const SpectrumSmoothingSettings& right);

[[nodiscard]] int NormalizeMedianKernelSize(int kernel_size);
[[nodiscard]] int EffectiveMedianKernelSize(int kernel_size, std::size_t value_count);
[[nodiscard]] std::vector<double> SmoothSpectrumValues(
    const std::vector<double>& values,
    const SpectrumSmoothingSettings& settings);

}  // namespace specforge
