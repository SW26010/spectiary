#include "domain/spectrum_fixture.h"

#include <cmath>

namespace specforge {
namespace {

double Gaussian(double x, double center, double width, double amplitude)
{
    const double distance = (x - center) / width;
    return amplitude * std::exp(-0.5 * distance * distance);
}

}  // namespace

SpectrumSeries MakeSmallSyntheticSpectrum()
{
    SpectrumSeries series;
    series.name = "Small synthetic spectrum";
    series.wavelength.reserve(768);
    series.flux.reserve(768);

    for (int index = 0; index < 768; ++index) {
        const double wavelength = 3800.0 + static_cast<double>(index) * 3.8;
        const double continuum = 1.0 + 0.04 * std::sin(static_cast<double>(index) * 0.025);
        const double absorption_a = Gaussian(wavelength, 4300.0, 36.0, -0.42);
        const double absorption_b = Gaussian(wavelength, 5175.0, 48.0, -0.28);
        const double emission = Gaussian(wavelength, 5892.0, 22.0, 0.22);
        const double ripple = 0.015 * std::sin(static_cast<double>(index) * 0.19);

        series.wavelength.push_back(wavelength);
        series.flux.push_back(continuum + absorption_a + absorption_b + emission + ripple);
    }

    return series;
}

}  // namespace specforge
