#include "domain/spectrum_fixture.h"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace specforge {
namespace {

struct SyntheticSpectrumData {
    std::string name;
    std::vector<double> wavelength;
    std::vector<double> flux;
};

double Gaussian(double x, double center, double width, double amplitude)
{
    const double distance = (x - center) / width;
    return amplitude * std::exp(-0.5 * distance * distance);
}

SyntheticSpectrumData MakeSmallSyntheticSpectrumData()
{
    SyntheticSpectrumData series;
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

}  // namespace

SpectrumSnapshotHandle MakeSmallSyntheticSpectrumSnapshot()
{
    SyntheticSpectrumData series = MakeSmallSyntheticSpectrumData();

    auto snapshot = std::make_shared<SpectrumSnapshot>();
    snapshot->source.id = "synthetic.small_spectrum";
    snapshot->source.display_name = "Small synthetic fixture";
    snapshot->source.uri = "synthetic://small-spectrum";
    snapshot->source.metadata.push_back({"source_type", "synthetic_fixture", "domain"});

    snapshot->collection.spectrum_count = 1;
    snapshot->collection.current_index = 0;

    snapshot->current_spectrum.name = series.name;
    snapshot->current_spectrum.x_values = std::make_shared<const std::vector<double>>(std::move(series.wavelength));
    snapshot->current_spectrum.y_values = std::make_shared<const std::vector<double>>(std::move(series.flux));
    snapshot->current_spectrum.point_count = snapshot->current_spectrum.x_values->size();
    snapshot->current_spectrum.metadata.push_back({"fixture", "small synthetic spectrum", "domain"});

    snapshot->axis.x_quantity = SpectrumAxisQuantity::Wavelength;
    snapshot->axis.x_unit = SpectrumAxisUnit::Angstrom;
    snapshot->axis.x_frame = SpectrumAxisFrame::Unknown;
    snapshot->axis.y_quantity = SpectrumValueQuantity::Flux;
    snapshot->axis.x_label = "Wavelength (Å)";
    snapshot->axis.y_label = "Flux";

    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_show_spectral_lines = true;
    snapshot->capabilities.requires_rest_frame_warning = true;

    snapshot->diagnostics.push_back({
        SpectrumDiagnosticSeverity::Warning,
        SpectrumDiagnosticCode::AxisFrameUnknown,
        "Synthetic fixture has no verified wavelength coordinate frame.",
        {},
    });

    return snapshot;
}

}  // namespace specforge
