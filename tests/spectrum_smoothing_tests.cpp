#include "domain/spectrum_smoothing.h"

#include <cstddef>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void RequireNear(double actual, double expected, double tolerance, std::string_view message)
{
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string(message));
    }
}

void RequireVectorNear(
    const std::vector<double>& actual,
    const std::vector<double>& expected,
    double tolerance,
    std::string_view message)
{
    Require(actual.size() == expected.size(), message);
    for (std::size_t index = 0; index < actual.size(); ++index) {
        RequireNear(actual[index], expected[index], tolerance, message);
    }
}

void TestNoneReturnsInput()
{
    const std::vector<double> values = {1.0, 4.0, 9.0};
    const std::vector<double> smoothed = spectiary::SmoothSpectrumValues(values, {});
    Require(smoothed == values, "none smoothing should return the input values");
}

void TestGaussianKeepsConstantSignal()
{
    spectiary::SpectrumSmoothingSettings settings;
    settings.method = spectiary::SpectrumSmoothingMethod::Gaussian;
    settings.parameters.gaussian_sigma = 1.0;

    const std::vector<double> values = {5.0, 5.0, 5.0, 5.0};
    const std::vector<double> smoothed = spectiary::SmoothSpectrumValues(values, settings);
    RequireVectorNear(smoothed, values, 1.0e-12, "gaussian smoothing should keep a constant signal constant");
}

void TestMedianUsesReflectedEdges()
{
    spectiary::SpectrumSmoothingSettings settings;
    settings.method = spectiary::SpectrumSmoothingMethod::Median;
    settings.parameters.median_kernel_size = 3;

    const std::vector<double> values = {9.0, 1.0, 8.0, 2.0, 7.0};
    const std::vector<double> smoothed = spectiary::SmoothSpectrumValues(values, settings);
    const std::vector<double> expected = {9.0, 8.0, 2.0, 7.0, 7.0};
    Require(smoothed == expected, "median smoothing should use reflected edge samples");
}

void TestMedianKernelLargerThanSignalDoesNotIntroduceZeroes()
{
    spectiary::SpectrumSmoothingSettings settings;
    settings.method = spectiary::SpectrumSmoothingMethod::Median;
    settings.parameters.median_kernel_size = 99;

    const std::vector<double> values = {2.0, 10.0, 20.0};
    const std::vector<double> smoothed = spectiary::SmoothSpectrumValues(values, settings);
    Require(smoothed == values, "median smoothing should clamp oversized kernels to the signal length");
}

void TestMedianKernelNormalization()
{
    Require(spectiary::NormalizeMedianKernelSize(-1) == 3, "median kernel should have a minimum size");
    Require(spectiary::NormalizeMedianKernelSize(2) == 3, "median kernel should round small even values to 3");
    Require(spectiary::NormalizeMedianKernelSize(4) == 5, "median kernel should be odd");
    Require(spectiary::NormalizeMedianKernelSize(7) == 7, "median kernel should preserve valid odd values");
    Require(spectiary::NormalizeMedianKernelSize(10001) == 501, "median kernel should have a conservative cap");
    Require(spectiary::EffectiveMedianKernelSize(99, 5) == 5, "effective kernel should not exceed odd signal length");
    Require(spectiary::EffectiveMedianKernelSize(99, 4) == 3, "effective kernel should fit even signal lengths");
    Require(spectiary::EffectiveMedianKernelSize(99, 2) == 1, "short signals should not be median filtered");
}

}  // namespace

int main()
{
    TestNoneReturnsInput();
    TestGaussianKeepsConstantSignal();
    TestMedianUsesReflectedEdges();
    TestMedianKernelLargerThanSignalDoesNotIntroduceZeroes();
    TestMedianKernelNormalization();
    return 0;
}
