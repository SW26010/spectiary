#include "domain/spectrum_loader.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using specforge::SpectrumAxisQuantity;
using specforge::SpectrumDiagnosticCode;
using specforge::SpectrumDiagnosticSeverity;
using specforge::SpectrumSnapshotHandle;
using specforge::SpectrumValueQuantity;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string ShapeText(const std::vector<std::size_t>& shape)
{
    std::ostringstream stream;
    stream << '(';
    for (std::size_t index = 0; index < shape.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << shape[index];
    }
    if (shape.size() == 1) {
        stream << ',';
    }
    stream << ')';
    return stream.str();
}

template <typename T>
std::vector<unsigned char> BytesFor(const std::vector<T>& values)
{
    std::vector<unsigned char> bytes(values.size() * sizeof(T));
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), values.data(), bytes.size());
    }
    return bytes;
}

void WriteNpy(
    const std::filesystem::path& path,
    std::string_view descr,
    const std::vector<std::size_t>& shape,
    const std::vector<unsigned char>& payload)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open test fixture for writing");

    std::string header = "{'descr': '";
    header += descr;
    header += "', 'fortran_order': False, 'shape': ";
    header += ShapeText(shape);
    header += ", }";

    constexpr std::size_t kPreambleSize = 10;
    const std::size_t header_with_newline = header.size() + 1;
    const std::size_t padding = (16 - ((kPreambleSize + header_with_newline) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    Require(header.size() <= std::numeric_limits<std::uint16_t>::max(), "test NPY header is too large");

    constexpr unsigned char kMagic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    stream.write(reinterpret_cast<const char*>(kMagic), static_cast<std::streamsize>(sizeof(kMagic)));
    constexpr char kVersion[] = {1, 0};
    stream.write(kVersion, static_cast<std::streamsize>(sizeof(kVersion)));

    const auto header_length = static_cast<std::uint16_t>(header.size());
    const char length_bytes[] = {
        static_cast<char>(header_length & 0xffU),
        static_cast<char>((header_length >> 8U) & 0xffU),
    };
    stream.write(length_bytes, static_cast<std::streamsize>(sizeof(length_bytes)));
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!payload.empty()) {
        stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    Require(stream.good(), "could not write test NPY fixture");
}

SpectrumDiagnosticCode FirstDiagnosticCode(const SpectrumSnapshotHandle& snapshot)
{
    Require(snapshot != nullptr, "expected a snapshot");
    Require(!snapshot->diagnostics.empty(), "expected a diagnostic");
    return snapshot->diagnostics.front().code;
}

bool HasDiagnosticCode(const SpectrumSnapshotHandle& snapshot, SpectrumDiagnosticCode code)
{
    Require(snapshot != nullptr, "expected a snapshot");
    for (const specforge::SpectrumDiagnostic& diagnostic : snapshot->diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

std::string_view MetadataValue(const SpectrumSnapshotHandle& snapshot, std::string_view key)
{
    Require(snapshot != nullptr, "expected a snapshot");
    for (const specforge::SpectrumMetadataEntry& entry : snapshot->source.metadata) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return {};
}

void TestLoadsSelectedNpyRow()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_row_X.npy";
    WriteNpy(
        path,
        "<f8",
        {2, 3},
        BytesFor<double>({
            1.0,
            2.0,
            std::numeric_limits<double>::quiet_NaN(),
            4.0,
            5.0,
            6.0,
        }));

    const SpectrumSnapshotHandle first = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(first->capabilities.can_plot_current_spectrum, "first row should be plottable");
    Require(first->collection.spectrum_count == 2, "row count should be preserved");
    Require(first->collection.current_index == 0, "first row index should be selected");
    Require(first->collection.can_move_next, "first row should allow next navigation");
    Require(first->current_spectrum.point_count == 2, "non-finite pixels should be filtered");
    Require(first->axis.x_quantity == SpectrumAxisQuantity::Pixel, "short matrix should use pixel axis");
    Require(first->axis.y_quantity == SpectrumValueQuantity::FeatureValue, "X.npy should be labeled as feature data");
    Require(
        HasDiagnosticCode(first, SpectrumDiagnosticCode::NonFiniteValuesFiltered),
        "filtered row should report non-finite filtering");

    const SpectrumSnapshotHandle second = specforge::LoadSpectrumSnapshotFromPath(path, 1);
    Require(second->capabilities.can_plot_current_spectrum, "second row should be plottable");
    Require(second->collection.current_index == 1, "second row index should be selected");
    Require(second->collection.can_move_previous, "second row should allow previous navigation");
    Require(second->current_spectrum.point_count == 3, "finite second row should keep all points");
}

void TestRejectsAuxiliaryNpyArrays()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_label.npy";
    WriteNpy(path, "<f8", {1, 2}, BytesFor<double>({1.0, 2.0}));

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "auxiliary arrays should not be plottable");
    Require(
        FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::UnsupportedFormat,
        "auxiliary arrays should be classified as unsupported format");
}

void TestClassifiesUnsupportedDtype()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_flux.npy";
    WriteNpy(path, "<i4", {1, 3}, BytesFor<std::int32_t>({1, 2, 3}));

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(
        FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::UnsupportedFormat,
        "integer NPY arrays should be unsupported, not invalid shape");
}

void TestClassifiesEmptyShape()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_empty_flux.npy";
    WriteNpy(path, "<f8", {0, 3}, {});

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::EmptyData, "empty NPY shape should be empty data");
}

void TestUnsupportedCsvUsesDomainSnapshot()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_pending.csv";
    {
        std::ofstream stream(path);
        Require(stream.good(), "could not open CSV test fixture for writing");
        stream << "wavelength,flux\n1,2\n";
        Require(stream.good(), "could not write CSV test fixture");
    }

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "unsupported CSV should not be plottable");
    Require(snapshot->capabilities.has_domain_error, "unsupported CSV should be a domain error snapshot");
    Require(FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::UnsupportedFormat, "CSV should be unsupported");
    Require(snapshot->diagnostics.front().severity == SpectrumDiagnosticSeverity::Error, "CSV diagnostic should be an error");
    Require(MetadataValue(snapshot, "source_type") == "file", "CSV source type should come from domain");
    Require(MetadataValue(snapshot, "format") == "csv", "CSV format should come from domain");
}

void TestFolderUsesDomainSnapshot()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_folder_source";
    std::error_code error;
    std::filesystem::remove_all(path, error);
    Require(std::filesystem::create_directory(path), "could not create folder source test fixture");

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "folder source should not be plottable");
    Require(snapshot->capabilities.has_domain_error, "folder source should be a domain error snapshot");
    Require(FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::UnsupportedFormat, "folder should be unsupported");
    Require(snapshot->diagnostics.front().severity == SpectrumDiagnosticSeverity::Error, "folder diagnostic should be an error");
    Require(MetadataValue(snapshot, "source_type") == "folder", "folder source type should come from domain");

    std::filesystem::remove_all(path, error);
}

}  // namespace

int main()
{
    TestLoadsSelectedNpyRow();
    TestRejectsAuxiliaryNpyArrays();
    TestClassifiesUnsupportedDtype();
    TestClassifiesEmptyShape();
    TestUnsupportedCsvUsesDomainSnapshot();
    TestFolderUsesDomainSnapshot();
    return 0;
}
