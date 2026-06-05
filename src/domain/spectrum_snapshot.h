#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace specforge {

enum class SpectrumAxisQuantity {
    Unknown,
    Wavelength,
    Pixel,
};

enum class SpectrumAxisUnit {
    Unknown,
    Angstrom,
    Pixel,
};

enum class SpectrumAxisFrame {
    Unknown,
    Observed,
    Rest,
};

enum class SpectrumValueQuantity {
    Unknown,
    Flux,
    NormalizedFlux,
    FeatureValue,
};

enum class SpectrumDiagnosticSeverity {
    Info,
    Warning,
    Error,
};

enum class SpectrumDiagnosticCode {
    None,
    OpenFailed,
    UnsupportedFormat,
    CatalogNotSpectrum,
    EmptyData,
    InvalidShape,
    MissingWavelength,
    WavelengthFluxSizeMismatch,
    NoValidPixels,
    NonFiniteValuesFiltered,
    MaskFilteredPixels,
    IvarFilteredPixels,
    AxisFrameUnknown,
    RestFrameNotApplied,
    UnsupportedAxisForSpectralLines,
};

struct SpectrumMetadataEntry {
    std::string key;
    std::string value;
    std::string source;
};

struct SpectrumDiagnostic {
    SpectrumDiagnosticSeverity severity = SpectrumDiagnosticSeverity::Info;
    SpectrumDiagnosticCode code = SpectrumDiagnosticCode::None;
    std::string message;
    std::vector<SpectrumMetadataEntry> metadata;
};

struct SpectrumSourceSnapshot {
    std::string id;
    std::string display_name;
    std::filesystem::path path;
    std::string uri;
    std::vector<SpectrumMetadataEntry> metadata;
};

struct SpectrumCollectionSnapshot {
    std::size_t spectrum_count = 0;
    std::size_t current_index = 0;
    bool can_move_previous = false;
    bool can_move_next = false;
};

using SpectrumValueVector = std::shared_ptr<const std::vector<double>>;

struct CurrentSpectrumSnapshot {
    std::string name;
    SpectrumValueVector x_values;
    SpectrumValueVector y_values;
    std::size_t point_count = 0;
    std::vector<SpectrumMetadataEntry> metadata;
};

struct SpectrumAxisSnapshot {
    SpectrumAxisQuantity x_quantity = SpectrumAxisQuantity::Unknown;
    SpectrumAxisUnit x_unit = SpectrumAxisUnit::Unknown;
    SpectrumAxisFrame x_frame = SpectrumAxisFrame::Unknown;
    SpectrumValueQuantity y_quantity = SpectrumValueQuantity::Unknown;
    std::string x_label;
    std::string y_label;
};

struct SpectrumCapabilities {
    bool can_plot_current_spectrum = false;
    bool can_switch_spectrum = false;
    bool can_show_spectral_lines = false;
    bool can_show_rest_frame_spectral_lines = false;
    bool requires_angstrom_warning = false;
    bool requires_rest_frame_warning = false;
    bool has_domain_error = false;
};

struct SpectrumSnapshot {
    SpectrumSourceSnapshot source;
    SpectrumCollectionSnapshot collection;
    CurrentSpectrumSnapshot current_spectrum;
    SpectrumAxisSnapshot axis;
    SpectrumCapabilities capabilities;
    std::vector<SpectrumDiagnostic> diagnostics;
};

using SpectrumSnapshotHandle = std::shared_ptr<const SpectrumSnapshot>;

}  // namespace specforge
