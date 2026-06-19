#include "domain/spectrum_smoothing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace specforge {
namespace {

constexpr double kGaussianTruncate = 4.0;
constexpr int kMaxGaussianRadius = 4096;
constexpr int kMaxMedianKernelSize = 501;

std::size_t ReflectIndex(long long index, std::size_t size)
{
    if (size <= 1) {
        return 0;
    }

    const long long count = static_cast<long long>(size);
    const long long period = count * 2;
    index %= period;
    if (index < 0) {
        index += period;
    }
    if (index >= count) {
        index = period - index - 1;
    }
    return static_cast<std::size_t>(index);
}

std::vector<double> SmoothGaussian(const std::vector<double>& values, double sigma)
{
    if (values.empty() || !(sigma > 0.0) || !std::isfinite(sigma)) {
        return values;
    }

    const double radius_value = kGaussianTruncate * sigma + 0.5;
    if (radius_value > static_cast<double>(std::numeric_limits<int>::max())) {
        return values;
    }

    int radius = static_cast<int>(radius_value);
    radius = std::min(radius, kMaxGaussianRadius);
    if (radius <= 0) {
        return values;
    }

    std::vector<double> weights(static_cast<std::size_t>(radius * 2 + 1));
    double weight_sum = 0.0;
    for (int offset = -radius; offset <= radius; ++offset) {
        const double scaled = static_cast<double>(offset) / sigma;
        const double weight = std::exp(-0.5 * scaled * scaled);
        weights[static_cast<std::size_t>(offset + radius)] = weight;
        weight_sum += weight;
    }
    if (!(weight_sum > 0.0)) {
        return values;
    }

    for (double& weight : weights) {
        weight /= weight_sum;
    }

    std::vector<double> smoothed(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        double sum = 0.0;
        for (int offset = -radius; offset <= radius; ++offset) {
            const std::size_t source_index =
                ReflectIndex(static_cast<long long>(index) + static_cast<long long>(offset), values.size());
            sum += values[source_index] * weights[static_cast<std::size_t>(offset + radius)];
        }
        smoothed[index] = sum;
    }
    return smoothed;
}

std::vector<double> SmoothMedian(const std::vector<double>& values, int kernel_size)
{
    if (values.empty()) {
        return values;
    }

    const int normalized_kernel_size = EffectiveMedianKernelSize(kernel_size, values.size());
    if (normalized_kernel_size <= 1) {
        return values;
    }
    const int half_width = normalized_kernel_size / 2;
    std::vector<double> smoothed(values.size());
    std::vector<double> window;
    window.reserve(static_cast<std::size_t>(normalized_kernel_size));

    for (std::size_t index = 0; index < values.size(); ++index) {
        window.clear();
        for (int offset = -half_width; offset <= half_width; ++offset) {
            // Unlike scipy.signal.medfilt, reflected edges avoid zero-biased display artifacts on short spectra.
            const std::size_t source_index =
                ReflectIndex(static_cast<long long>(index) + static_cast<long long>(offset), values.size());
            window.push_back(values[source_index]);
        }

        const std::size_t middle = window.size() / 2;
        std::nth_element(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(middle), window.end());
        smoothed[index] = window[middle];
    }
    return smoothed;
}

}  // namespace

bool operator==(const SpectrumSmoothingSettings& left, const SpectrumSmoothingSettings& right)
{
    return left.method == right.method && left.gaussian_sigma == right.gaussian_sigma &&
           left.median_kernel_size == right.median_kernel_size;
}

bool operator!=(const SpectrumSmoothingSettings& left, const SpectrumSmoothingSettings& right)
{
    return !(left == right);
}

int NormalizeMedianKernelSize(int kernel_size)
{
    if (kernel_size < 3) {
        return 3;
    }
    if (kernel_size > kMaxMedianKernelSize) {
        return kMaxMedianKernelSize;
    }
    if (kernel_size % 2 == 0) {
        ++kernel_size;
    }
    return kernel_size;
}

int EffectiveMedianKernelSize(int kernel_size, std::size_t value_count)
{
    if (value_count < 3) {
        return 1;
    }

    int normalized = NormalizeMedianKernelSize(kernel_size);
    int max_for_values = static_cast<int>(std::min<std::size_t>(value_count, kMaxMedianKernelSize));
    if (max_for_values % 2 == 0) {
        --max_for_values;
    }
    return std::max(1, std::min(normalized, max_for_values));
}

std::vector<double> SmoothSpectrumValues(const std::vector<double>& values, const SpectrumSmoothingSettings& settings)
{
    switch (settings.method) {
    case SpectrumSmoothingMethod::Gaussian:
        return SmoothGaussian(values, settings.gaussian_sigma);
    case SpectrumSmoothingMethod::Median:
        return SmoothMedian(values, settings.median_kernel_size);
    case SpectrumSmoothingMethod::None:
    default:
        return values;
    }
}

}  // namespace specforge
