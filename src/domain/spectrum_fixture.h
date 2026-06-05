#pragma once

#include "domain/spectrum_snapshot.h"

#include <string>
#include <vector>

namespace specforge {

struct SpectrumSeries {
    std::string name;
    std::vector<double> wavelength;
    std::vector<double> flux;
};

SpectrumSeries MakeSmallSyntheticSpectrum();
SpectrumSnapshotHandle MakeSmallSyntheticSpectrumSnapshot();

}  // namespace specforge
