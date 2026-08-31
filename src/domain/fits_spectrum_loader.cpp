#include "domain/fits_spectrum_loader.h"

#include "domain/fits_file_reader.h"
#include "domain/spectrum_loader_support.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace specforge::detail {
namespace {

constexpr double kSpeedOfLightKmPerSecond = 299792.458;

void ThrowIfFitsLoadCanceled(const std::function<bool()>& cancellation_requested)
{
    if (cancellation_requested && cancellation_requested()) {
        throw FitsFileError(FitsFileErrorCode::Canceled, "FITS loading was canceled.");
    }
}

struct FitsMetadataMatch {
    std::string key;
    std::string value;
};

const FitsColumn* FindFitsColumn(const FitsHdu& hdu, std::initializer_list<std::string_view> names)
{
    for (const FitsColumn& column : hdu.columns) {
        for (std::string_view name : names) {
            if (column.normalized_name == name) {
                return &column;
            }
        }
    }
    return nullptr;
}

bool IsOrMaskColumnName(std::string_view normalized_name)
{
    if (normalized_name == "ORMASK" || normalized_name == "ORMASKS") {
        return true;
    }
    constexpr std::string_view kPrefix = "ORMASK";
    if (!normalized_name.starts_with(kPrefix) || normalized_name.size() == kPrefix.size()) {
        return false;
    }
    return std::all_of(
        normalized_name.begin() + static_cast<std::ptrdiff_t>(kPrefix.size()),
        normalized_name.end(),
        [](char character) {
            return std::isdigit(static_cast<unsigned char>(character)) != 0;
        });
}

const FitsColumn* FindFitsMaskColumn(const FitsHdu& hdu)
{
    for (const FitsColumn& column : hdu.columns) {
        if (IsOrMaskColumnName(column.normalized_name)) {
            return &column;
        }
    }
    return nullptr;
}

bool IsFitsNumericColumn(const FitsColumn& column)
{
    return column.numeric_type.has_value();
}

SpectrumDiagnosticCode ToSpectrumDiagnosticCode(FitsFileErrorCode code)
{
    switch (code) {
    case FitsFileErrorCode::OpenFailed:
        return SpectrumDiagnosticCode::OpenFailed;
    case FitsFileErrorCode::UnsupportedFormat:
        return SpectrumDiagnosticCode::UnsupportedFormat;
    case FitsFileErrorCode::InvalidShape:
        return SpectrumDiagnosticCode::InvalidShape;
    case FitsFileErrorCode::Canceled:
        return SpectrumDiagnosticCode::OpenFailed;
    }
    return SpectrumDiagnosticCode::InvalidShape;
}

std::string FormatFitsMetadataNumber(double value)
{
    std::ostringstream stream;
    stream.precision(std::numeric_limits<double>::max_digits10);
    stream << value;
    return stream.str();
}

bool HasLoadedMetadataKey(const LoadedSpectrum& loaded, std::string_view key)
{
    return std::any_of(
        loaded.source_metadata.begin(),
        loaded.source_metadata.end(),
        [key](const SpectrumMetadataEntry& entry) {
            return entry.key == key;
        });
}

std::optional<std::string_view> LoadedMetadataValue(const LoadedSpectrum& loaded, std::string_view key)
{
    for (const SpectrumMetadataEntry& entry : loaded.source_metadata) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::optional<double> LoadedMetadataDouble(const LoadedSpectrum& loaded, std::string_view key)
{
    const std::optional<std::string_view> value = LoadedMetadataValue(loaded, key);
    if (!value) {
        return std::nullopt;
    }
    return ParseDouble(std::string(*value));
}

std::optional<FitsMetadataMatch> FirstFitsHeaderValue(
    const FitsFile& file,
    const std::vector<FitsHdu>& hdus,
    std::initializer_list<std::string_view> header_keys,
    const std::function<bool()>& cancellation_requested)
{
    for (const FitsHdu& hdu : hdus) {
        ThrowIfFitsLoadCanceled(cancellation_requested);
        for (std::string_view key : header_keys) {
            const std::optional<std::string> value =
                file.ReadKeywordString(
                    hdu,
                    key,
                    cancellation_requested);
            if (value && !value->empty()) {
                return FitsMetadataMatch{std::string(key), *value};
            }
        }
    }
    ThrowIfFitsLoadCanceled(cancellation_requested);
    return std::nullopt;
}

bool FitsMetadataTruthy(std::string_view value)
{
    const std::string normalized = UpperAscii(TrimAscii(std::string(value)));
    return normalized == "T" || normalized == "TRUE" || normalized == "1";
}

bool FitsMetadataFalsey(std::string_view value)
{
    const std::string normalized = UpperAscii(TrimAscii(std::string(value)));
    return normalized == "F" || normalized == "FALSE" || normalized == "0";
}

bool RedshiftValueIsUsable(double value)
{
    return std::isfinite(value) && value > -1.0 && value < 20.0;
}

bool RedshiftWarningIsSet(const LoadedSpectrum& loaded)
{
    const std::optional<double> warning = LoadedMetadataDouble(loaded, "redshift_warning");
    return warning && std::isfinite(*warning) && std::abs(*warning) > 0.0;
}

bool SurveyClassUsesPipelineRedshift(const LoadedSpectrum& loaded)
{
    const std::optional<std::string_view> survey_class = LoadedMetadataValue(loaded, "survey_class");
    if (!survey_class) {
        return false;
    }

    const std::string normalized = UpperAscii(TrimAscii(std::string(*survey_class)));
    return normalized == "GALAXY" || normalized == "QSO" || normalized == "AGN";
}

void AddFitsWavelengthFrameMetadata(LoadedSpectrum& loaded)
{
    if (const std::optional<std::string_view> vacuum = LoadedMetadataValue(loaded, "wavelength_vacuum")) {
        if (!HasLoadedMetadataKey(loaded, "wavelength_medium")) {
            if (FitsMetadataTruthy(*vacuum)) {
                loaded.source_metadata.push_back({"wavelength_medium", "vacuum", "fits"});
            } else if (FitsMetadataFalsey(*vacuum)) {
                loaded.source_metadata.push_back({"wavelength_medium", "air", "fits"});
            }
        }
    }
    if (!HasLoadedMetadataKey(loaded, "observer_frame_correction")) {
        const std::optional<std::string_view> heliocentric_applied =
            LoadedMetadataValue(loaded, "heliocentric_correction_applied");
        if (HasLoadedMetadataKey(loaded, "heliocentric_correction_km_s") ||
            (heliocentric_applied && FitsMetadataTruthy(*heliocentric_applied))) {
            loaded.source_metadata.push_back({"observer_frame_correction", "heliocentric", "fits"});
        }
    }
}

void AddFitsRestFrameStatusMetadata(LoadedSpectrum& loaded)
{
    if (!HasLoadedMetadataKey(loaded, "rest_frame_correction_status")) {
        loaded.source_metadata.push_back({"rest_frame_correction_status", "not_applied", "domain"});
    }
}

void AddFitsTargetRestFrameMetadata(LoadedSpectrum& loaded)
{
    if (HasLoadedMetadataKey(loaded, "target_rest_frame_status")) {
        return;
    }

    const bool prefer_pipeline_redshift = SurveyClassUsesPipelineRedshift(loaded);
    const std::optional<double> radial_velocity = LoadedMetadataDouble(loaded, "radial_velocity_km_s");
    const bool radial_velocity_usable = radial_velocity && std::isfinite(*radial_velocity);
    const std::optional<double> redshift = LoadedMetadataDouble(loaded, "redshift");
    const bool redshift_usable = redshift && RedshiftValueIsUsable(*redshift);
    const bool redshift_unreliable = RedshiftWarningIsSet(loaded);

    if (!prefer_pipeline_redshift && radial_velocity_usable) {
        const double target_redshift = *radial_velocity / kSpeedOfLightKmPerSecond;
        loaded.source_metadata.push_back({"target_redshift", FormatFitsMetadataNumber(target_redshift), "domain"});
        loaded.source_metadata.push_back({"target_redshift_source", "radial_velocity_low_speed", "domain"});
        loaded.source_metadata.push_back({"target_redshift_status", "available", "domain"});
        loaded.source_metadata.push_back({"target_rest_frame_status", "available_not_applied", "domain"});
        return;
    }

    if (redshift_usable) {
        loaded.source_metadata.push_back({"target_redshift", FormatFitsMetadataNumber(*redshift), "domain"});
        loaded.source_metadata.push_back({"target_redshift_source", "pipeline_redshift", "domain"});
        if (redshift_unreliable) {
            loaded.source_metadata.push_back({"target_redshift_status", "unreliable", "domain"});
            loaded.source_metadata.push_back({"target_redshift_warning", "zwarning_nonzero", "domain"});
            loaded.source_metadata.push_back({"target_rest_frame_status", "unreliable_not_applied", "domain"});
        } else {
            loaded.source_metadata.push_back({"target_redshift_status", "available", "domain"});
            loaded.source_metadata.push_back({"target_rest_frame_status", "available_not_applied", "domain"});
        }
        return;
    }

    if (redshift && !redshift_usable) {
        loaded.source_metadata.push_back({"target_redshift_status", "invalid", "domain"});
        loaded.source_metadata.push_back({"target_redshift_warning", "invalid_pipeline_redshift", "domain"});
    } else {
        loaded.source_metadata.push_back({"target_redshift_status", "missing", "domain"});
    }
    loaded.source_metadata.push_back({"target_rest_frame_status", "unavailable", "domain"});
}

void AddFitsHeaderMetadata(
    LoadedSpectrum& loaded,
    const FitsFile& file,
    const std::vector<FitsHdu>& hdus,
    const std::function<bool()>& cancellation_requested)
{
    const auto add_first_value = [&](std::string_view output_key, std::initializer_list<std::string_view> header_keys) {
        if (HasLoadedMetadataKey(loaded, output_key)) {
            return;
        }
        if (const std::optional<FitsMetadataMatch> match =
                FirstFitsHeaderValue(
                    file,
                    hdus,
                    header_keys,
                    cancellation_requested)) {
            loaded.source_metadata.push_back({std::string(output_key), match->value, "fits"});
        }
    };

    if (!HasLoadedMetadataKey(loaded, "radial_velocity_km_s")) {
        if (const std::optional<FitsMetadataMatch> match = FirstFitsHeaderValue(
                file,
                hdus,
                {"RADVEL", "RAD_VEL", "RADIALV", "RADIAL_V", "RV", "VRAD", "RVEL", "1D_RV"},
                cancellation_requested)) {
            loaded.source_metadata.push_back({"radial_velocity_km_s", match->value, "fits"});
            loaded.source_metadata.push_back({"radial_velocity_source", "header:" + match->key, "fits"});
        }
    }
    add_first_value("telescope", {"TELESCOP"});
    add_first_value("data_release", {"DATA_V", "RUN2D"});
    add_first_value("heliocentric_correction_km_s", {"HELIO_RV"});
    add_first_value("heliocentric_correction_applied", {"HELIO"});
    add_first_value("redshift", {"Z", "1D_Z"});
    add_first_value("redshift_error", {"Z_ERR", "ZERR", "1D_Z_ERR"});
    add_first_value("redshift_warning", {"ZWARNING", "Z_WARN"});
    add_first_value("redshift_flag", {"ZFLAG"});
    add_first_value("survey_class", {"CLASS", "1D_CLASS"});
    add_first_value("survey_subclass", {"SUBCLASS", "1D_SUBCL"});
    add_first_value("wavelength_vacuum", {"VACUUM"});
    ThrowIfFitsLoadCanceled(cancellation_requested);
    AddFitsWavelengthFrameMetadata(loaded);
    AddFitsRestFrameStatusMetadata(loaded);
    AddFitsTargetRestFrameMetadata(loaded);
}

std::optional<FitsMetadataMatch> FitsScalarColumnMetadata(
    const FitsFile& file,
    const FitsHdu& hdu,
    std::size_t row_index,
    std::initializer_list<std::string_view> column_names,
    const std::function<bool()>& cancellation_requested)
{
    const FitsColumn* column = FindFitsColumn(hdu, column_names);
    if (column == nullptr || column->repeat != 1 || !IsFitsNumericColumn(*column)) {
        return std::nullopt;
    }
    std::vector<double> values =
        file.ReadColumnVector(
            hdu,
            *column,
            row_index,
            false,
            cancellation_requested);
    if (values.empty() || !std::isfinite(values.front())) {
        return std::nullopt;
    }
    return FitsMetadataMatch{column->name, FormatFitsMetadataNumber(values.front())};
}

void AddFitsTableMetadata(
    LoadedSpectrum& loaded,
    const FitsFile& file,
    const FitsHdu& hdu,
    std::size_t row_index,
    bool scalar_rows,
    const std::function<bool()>& cancellation_requested)
{
    if (scalar_rows) {
        return;
    }

    if (!HasLoadedMetadataKey(loaded, "radial_velocity_km_s")) {
        if (const std::optional<FitsMetadataMatch> match = FitsScalarColumnMetadata(
                file,
                hdu,
                row_index,
                {"RADIALVELOCITYKMS",
                 "RADIALVELOCITY",
                 "RADIALVEL",
                 "RADVEL",
                 "VRAD",
                 "RV",
                 "RVEL",
                 "1DRV"},
                cancellation_requested)) {
            loaded.source_metadata.push_back({"radial_velocity_km_s", match->value, "fits"});
            loaded.source_metadata.push_back({"radial_velocity_source", "table_column:" + match->key, "fits"});
        }
    }
    if (!HasLoadedMetadataKey(loaded, "redshift")) {
        if (const std::optional<FitsMetadataMatch> match =
                FitsScalarColumnMetadata(
                    file,
                    hdu,
                    row_index,
                    {"REDSHIFT", "Z", "ZHELIO", "1DZ"},
                    cancellation_requested)) {
            loaded.source_metadata.push_back({"redshift", match->value, "fits"});
        }
    }
    if (!HasLoadedMetadataKey(loaded, "redshift_error")) {
        if (const std::optional<FitsMetadataMatch> match =
                FitsScalarColumnMetadata(
                    file,
                    hdu,
                    row_index,
                    {"ZERR", "ZERROR", "ZERRPIPE", "ZERRNOQSO", "ZERRFULL"},
                    cancellation_requested)) {
            loaded.source_metadata.push_back({"redshift_error", match->value, "fits"});
        }
    }
    if (!HasLoadedMetadataKey(loaded, "redshift_warning")) {
        if (const std::optional<FitsMetadataMatch> match =
                FitsScalarColumnMetadata(
                    file,
                    hdu,
                    row_index,
                    {"ZWARNING", "ZWARN", "ZWARNINGNOQSO"},
                    cancellation_requested)) {
            loaded.source_metadata.push_back({"redshift_warning", match->value, "fits"});
        }
    }
}

std::optional<LoadedSpectrum> TryLoadFitsTableSpectrum(
    const FitsFile& file,
    const std::vector<FitsHdu>& hdus,
    const FitsHdu& hdu,
    const std::filesystem::path& path,
    std::size_t requested_index,
    std::string format,
    const std::function<bool()>& cancellation_requested)
{
    ThrowIfFitsLoadCanceled(cancellation_requested);
    if (hdu.kind != FitsHduKind::BinaryTable ||
        hdu.columns.empty()) {
        return std::nullopt;
    }

    const FitsColumn* flux_column = FindFitsColumn(hdu, {"FLUX"});
    const FitsColumn* wavelength_column = FindFitsColumn(hdu, {"WAVELENGTH", "WAVE", "WAV", "LAMBDA"});
    const FitsColumn* loglam_column = FindFitsColumn(hdu, {"LOGLAM"});
    if (flux_column == nullptr || (wavelength_column == nullptr && loglam_column == nullptr)) {
        return std::nullopt;
    }

    const FitsColumn* x_column = wavelength_column != nullptr ? wavelength_column : loglam_column;
    const bool uses_loglam = loglam_column != nullptr && wavelength_column == nullptr;
    if (!IsFitsNumericColumn(*flux_column) ||
        !IsFitsNumericColumn(*x_column)) {
        throw FitsFileError(
            FitsFileErrorCode::UnsupportedFormat,
            "FITS wavelength/loglam and flux columns must use supported fixed numeric types.");
    }
    const bool scalar_rows = flux_column->repeat == 1 && x_column->repeat == 1;
    if (!scalar_rows && flux_column->repeat != x_column->repeat) {
        throw SpectrumFileLoadError(
            SpectrumDiagnosticCode::WavelengthFluxSizeMismatch,
            "FITS table wavelength/loglam and flux vector columns have different lengths.");
    }

    const std::size_t row_count = hdu.row_count;
    LoadedSpectrum loaded;
    loaded.source_type = "fits_spectrum";
    loaded.format = std::move(format);
    loaded.name = FileNameToUtf8(path);
    loaded.spectrum_count = scalar_rows ? 1U : row_count;
    loaded.current_index = scalar_rows ? 0U : requested_index;
    loaded.can_switch_spectrum = !scalar_rows && row_count > 1;
    loaded.source_metadata.push_back({"hdu_index", std::to_string(hdu.index), "fits"});
    loaded.source_metadata.push_back({"hdu_type", "bintable", "fits"});
    loaded.source_metadata.push_back({"x_column", x_column->name, "fits"});
    loaded.source_metadata.push_back({"flux_column", flux_column->name, "fits"});
    AddFitsTableMetadata(
        loaded,
        file,
        hdu,
        requested_index,
        scalar_rows,
        cancellation_requested);
    loaded.spectrum_metadata.push_back({"hdu_index", std::to_string(hdu.index), "fits"});
    if (!scalar_rows) {
        loaded.spectrum_metadata.push_back({"row_index", std::to_string(requested_index), "fits"});
    }

    loaded.x_values =
        file.ReadColumnVector(
            hdu,
            *x_column,
            requested_index,
            scalar_rows,
            cancellation_requested);
    if (uses_loglam) {
        for (std::size_t index = 0; index < loaded.x_values.size(); ++index) {
            if ((index & 0xfffU) == 0U) {
                ThrowIfFitsLoadCanceled(cancellation_requested);
            }
            loaded.x_values[index] = std::pow(10.0, loaded.x_values[index]);
        }
    }
    loaded.y_values =
        file.ReadColumnVector(
            hdu,
            *flux_column,
            requested_index,
            scalar_rows,
            cancellation_requested);

    const FitsColumn* ivar_column = FindFitsColumn(hdu, {"IVAR", "INVERSEVARIANCE"});
    const FitsColumn* mask_column = FindFitsMaskColumn(hdu);
    std::vector<double> ivar_values;
    std::vector<double> mask_values;
    const bool use_mask = !uses_loglam && mask_column != nullptr;
    const bool use_ivar = !use_mask && ivar_column != nullptr;
    if (use_ivar) {
        ivar_values =
            file.ReadColumnVector(
                hdu,
                *ivar_column,
                requested_index,
                scalar_rows,
                cancellation_requested);
        loaded.source_metadata.push_back({"valid_pixel_rule", "ivar_positive", "fits"});
    }
    if (use_mask) {
        mask_values =
            file.ReadColumnVector(
                hdu,
                *mask_column,
                requested_index,
                scalar_rows,
                cancellation_requested);
        loaded.source_metadata.push_back({"valid_pixel_rule", "ormask_zero", "fits"});
    }

    FilterStats stats;
    FilterSpectrumPixels(
        loaded.x_values,
        loaded.y_values,
        use_mask ? &mask_values : nullptr,
        use_ivar ? &ivar_values : nullptr,
        true,
        stats,
        [&cancellation_requested]() { ThrowIfFitsLoadCanceled(cancellation_requested); });
    AddFilterDiagnostics(loaded.diagnostics, stats);
    loaded.diagnostics.push_back(MakeDiagnostic(
        SpectrumDiagnosticSeverity::Warning,
        SpectrumDiagnosticCode::RestFrameNotApplied,
        "FITS wavelength values are plotted as provided; no rest-frame correction was applied."));
    AddFitsHeaderMetadata(
        loaded,
        file,
        hdus,
        cancellation_requested);
    return loaded;
}

std::optional<LoadedSpectrum> TryLoadFitsImageSpectrum(
    const FitsFile& file,
    const std::vector<FitsHdu>& hdus,
    const FitsHdu& hdu,
    const std::filesystem::path& path,
    std::string format,
    const std::function<bool()>& cancellation_requested)
{
    ThrowIfFitsLoadCanceled(cancellation_requested);
    if (hdu.kind != FitsHduKind::Image) {
        return std::nullopt;
    }
    if (hdu.image_axes.empty()) {
        return std::nullopt;
    }
    if (hdu.image_axes.size() > 2U) {
        throw FitsFileError(
            FitsFileErrorCode::UnsupportedFormat,
            "Only one- or two-dimensional FITS image spectra are supported.");
    }
    const std::optional<double> coeff0 = file.ReadKeywordDouble(
        hdu,
        "COEFF0",
        cancellation_requested);
    const std::optional<double> coeff1 = file.ReadKeywordDouble(
        hdu,
        "COEFF1",
        cancellation_requested);
    if (!coeff0 || !coeff1) {
        return std::nullopt;
    }

    const std::size_t column_count = hdu.image_axes.front();
    const std::size_t row_count =
        hdu.image_axes.size() >= 2U ? hdu.image_axes[1] : 1U;
    if (column_count == 0 || row_count == 0) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::EmptyData, "FITS image spectrum has an empty shape.");
    }

    LoadedSpectrum loaded;
    loaded.source_type = "fits_spectrum";
    loaded.format = std::move(format);
    loaded.name = FileNameToUtf8(path);
    loaded.source_metadata.push_back({"hdu_index", std::to_string(hdu.index), "fits"});
    loaded.source_metadata.push_back({"hdu_type", "image", "fits"});
    loaded.source_metadata.push_back({"wavelength_formula", "10**(coeff0+coeff1*pixel)", "fits"});
    loaded.source_metadata.push_back({"coeff0", std::to_string(*coeff0), "fits"});
    loaded.source_metadata.push_back({"coeff1", std::to_string(*coeff1), "fits"});
    loaded.spectrum_metadata.push_back({"hdu_index", std::to_string(hdu.index), "fits"});
    loaded.y_values = file.ReadImageRow(
        hdu,
        0,
        column_count,
        cancellation_requested);
    loaded.x_values.reserve(column_count);
    for (std::size_t index = 0; index < column_count; ++index) {
        if ((index & 0xfffU) == 0U) {
            ThrowIfFitsLoadCanceled(cancellation_requested);
        }
        loaded.x_values.push_back(std::pow(10.0, *coeff0 + *coeff1 * static_cast<double>(index)));
    }

    std::vector<double> ivar_values;
    if (row_count > 1) {
        ivar_values = file.ReadImageRow(
            hdu,
            1,
            column_count,
            cancellation_requested);
    }
    std::vector<double> mask_values;
    if (row_count > 4) {
        mask_values = file.ReadImageRow(
            hdu,
            4,
            column_count,
            cancellation_requested);
    }
    if (!ivar_values.empty() && !mask_values.empty()) {
        loaded.source_metadata.push_back({"valid_pixel_rule", "ivar_positive_and_ormask_zero", "fits"});
    } else if (!ivar_values.empty()) {
        loaded.source_metadata.push_back({"valid_pixel_rule", "ivar_positive", "fits"});
    } else if (!mask_values.empty()) {
        loaded.source_metadata.push_back({"valid_pixel_rule", "ormask_zero", "fits"});
    }

    FilterStats stats;
    FilterSpectrumPixels(
        loaded.x_values,
        loaded.y_values,
        mask_values.empty() ? nullptr : &mask_values,
        ivar_values.empty() ? nullptr : &ivar_values,
        true,
        stats,
        [&cancellation_requested]() { ThrowIfFitsLoadCanceled(cancellation_requested); });
    AddFilterDiagnostics(loaded.diagnostics, stats);
    loaded.diagnostics.push_back(MakeDiagnostic(
        SpectrumDiagnosticSeverity::Warning,
        SpectrumDiagnosticCode::RestFrameNotApplied,
        "FITS wavelength values are plotted as provided; no rest-frame correction was applied."));
    AddFitsHeaderMetadata(
        loaded,
        file,
        hdus,
        cancellation_requested);
    return loaded;
}

}  // namespace

bool IsFitsSourcePath(const std::filesystem::path& path)
{
    const std::string format = SourceFormatLabel(path);
    return format == "fits" || format == "fits.gz";
}

SpectrumSnapshotHandle LoadFitsSnapshot(const std::filesystem::path& path, std::size_t spectrum_index)
{
    return LoadFitsSnapshotCancelable(path, spectrum_index, {});
}

SpectrumSnapshotHandle LoadFitsSnapshotCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const std::function<bool()>& cancellation_requested)
{
    const std::string format = SourceFormatLabel(path);
    try {
        FitsFile file = FitsFile::Open(
            path,
            format == "fits.gz"
                ? FitsSourceEncoding::Gzip
                : FitsSourceEncoding::Plain,
            kMaxSynchronousFitsFileBytes,
            kMaxSynchronousInflatedFitsBytes,
            cancellation_requested);
        const std::vector<FitsHdu>& hdus = file.hdus();

        for (const FitsHdu& hdu : hdus) {
            if (cancellation_requested && cancellation_requested()) {
                return nullptr;
            }
            std::optional<LoadedSpectrum> loaded =
                TryLoadFitsTableSpectrum(
                    file,
                    hdus,
                    hdu,
                    path,
                    spectrum_index,
                    format,
                    cancellation_requested);
            if (loaded) {
                return MakeLoadedSpectrumSnapshot(
                    path,
                    std::move(*loaded),
                    [&cancellation_requested]() { ThrowIfFitsLoadCanceled(cancellation_requested); });
            }
        }
        for (const FitsHdu& hdu : hdus) {
            if (cancellation_requested && cancellation_requested()) {
                return nullptr;
            }
            std::optional<LoadedSpectrum> loaded =
                TryLoadFitsImageSpectrum(
                    file,
                    hdus,
                    hdu,
                    path,
                    format,
                    cancellation_requested);
            if (loaded) {
                return MakeLoadedSpectrumSnapshot(
                    path,
                    std::move(*loaded),
                    [&cancellation_requested]() { ThrowIfFitsLoadCanceled(cancellation_requested); });
            }
        }

        for (const FitsHdu& hdu : hdus) {
            ThrowIfFitsLoadCanceled(cancellation_requested);
            if (hdu.kind == FitsHduKind::AsciiTable) {
                throw FitsFileError(
                    FitsFileErrorCode::UnsupportedFormat,
                    "ASCII FITS tables are not supported as spectra.");
            }
        }

        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::CatalogNotSpectrum,
            "This FITS file looks like a catalog or unsupported FITS, not a single spectrum.",
            {{"format", format, "domain"}},
            "file",
            {{"format", format, "domain"}});
    } catch (const FitsFileError& error) {
        if (error.code() == FitsFileErrorCode::Canceled) {
            return nullptr;
        }
        return MakeErrorSnapshot(
            path,
            ToSpectrumDiagnosticCode(error.code()),
            error.what(),
            {{"format", format, "domain"}},
            "file",
            {{"format", format, "domain"}});
    } catch (const SpectrumFileLoadError& error) {
        return MakeErrorSnapshot(
            path,
            error.code(),
            error.what(),
            {{"format", format, "domain"}},
            "file",
            {{"format", format, "domain"}});
    } catch (const std::exception& error) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::InvalidShape,
            error.what(),
            {{"format", format, "domain"}},
            "file",
            {{"format", format, "domain"}});
    }
}

}  // namespace specforge::detail
