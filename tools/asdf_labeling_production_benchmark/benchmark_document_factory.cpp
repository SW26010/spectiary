#include "benchmark_document_factory.h"

#include "domain/canonical_timestamp.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace spectiary::asdf_labeling_benchmark {
namespace {

constexpr std::string_view kForwardRootToken =
    "production-benchmark-root-survives";
constexpr std::string_view kForwardTaskToken =
    "production-benchmark-task-survives";
constexpr std::string_view kForwardLabelToken =
    "production-benchmark-label-survives";

[[nodiscard]] std::string MakeExplicitName(std::size_t index)
{
    if (index >= 100'000'000U) {
        throw std::invalid_argument(
            "explicit benchmark roster indexes must fit eight digits");
    }

    std::array<char, 8> digits{};
    for (std::size_t position = digits.size(); position != 0U; --position) {
        digits[position - 1U] =
            static_cast<char>('0' + static_cast<char>(index % 10U));
        index /= 10U;
    }

    std::string name = "样本-";
    name.append(digits.data(), digits.size());
    name += "-α-𐐷.fits";
    return name;
}

[[nodiscard]] std::size_t CountUtf8Codepoints(std::string_view text)
{
    return static_cast<std::size_t>(std::count_if(
        text.begin(), text.end(), [](unsigned char byte) {
            return (byte & 0xc0U) != 0x80U;
        }));
}

[[nodiscard]] CanonicalTimestamp RequiredTimestamp(std::string_view text)
{
    const std::optional<CanonicalTimestamp> parsed =
        ParseCanonicalTimestamp(text);
    if (!parsed) {
        throw std::runtime_error(
            "production benchmark canonical timestamp is invalid");
    }
    return *parsed;
}

[[nodiscard]] std::vector<unsigned char> ReadAllBytes(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        throw std::runtime_error(
            "production benchmark fixture could not be opened");
    }
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void ReplaceTextOnce(std::vector<unsigned char>& bytes,
    std::string_view old_text,
    std::string_view new_text)
{
    const auto found = std::search(
        bytes.begin(), bytes.end(), old_text.begin(), old_text.end());
    if (found == bytes.end()) {
        throw std::runtime_error(
            "production benchmark unknown-field fixture target is absent");
    }
    const std::size_t offset =
        static_cast<std::size_t>(found - bytes.begin());
    bytes.erase(
        found,
        found + static_cast<std::ptrdiff_t>(old_text.size()));
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        new_text.begin(),
        new_text.end());
}

[[nodiscard]] bool ContainsText(
    std::span<const unsigned char> bytes,
    std::string_view text)
{
    return std::search(
               bytes.begin(), bytes.end(), text.begin(), text.end()) !=
        bytes.end();
}

}  // namespace

DatasetCase ParseDatasetCase(std::string_view value)
{
    if (value == "source-index") {
        return DatasetCase::SourceIndex;
    }
    if (value == "explicit-unicode") {
        return DatasetCase::ExplicitUnicode;
    }
    throw std::invalid_argument(
        "--case must be source-index or explicit-unicode");
}

std::string_view DatasetCaseName(DatasetCase dataset_case)
{
    switch (dataset_case) {
    case DatasetCase::SourceIndex:
        return "source-index";
    case DatasetCase::ExplicitUnicode:
        return "explicit-unicode";
    }
    throw std::invalid_argument("unknown production benchmark case");
}

BenchmarkDocument MakeBenchmarkDocument(
    DatasetCase dataset_case,
    std::size_t sample_count)
{
    if (sample_count == 0U || sample_count > kProductionSampleCount) {
        throw std::invalid_argument(
            "--sample-count must be between 1 and 1000000");
    }

    SampleLabelingDocument document;
    document.source.base_identity =
        "asdf-production-benchmark-source-v1";
    document.source.kind = "fits";
    document.source.name = "deterministic-production-benchmark.fits";
    document.source.fingerprint =
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    document.source.sample_count = sample_count;

    std::size_t roster_width = 0;
    if (dataset_case == DatasetCase::ExplicitUnicode) {
        document.source.roster.identity_kind =
            std::string{kSampleLabelingDocumentExplicitNamesRoster};
        document.source.roster.sample_names.reserve(sample_count);
        for (std::size_t index = 0; index < sample_count; ++index) {
            document.source.roster.sample_names.push_back(
                MakeExplicitName(index));
        }
        roster_width = kExplicitUnicodeRosterWidth;
        if (CountUtf8Codepoints(
                document.source.roster.sample_names.back()) !=
            roster_width) {
            throw std::runtime_error(
                "explicit benchmark roster width is not deterministic");
        }
    } else {
        document.source.roster.identity_kind =
            std::string{kSampleLabelingDocumentSourceIndexRoster};
    }

    document.annotation.values.resize(
        sample_count,
        kSampleLabelingDocumentUnlabeledValue);
    for (std::size_t index = 0; index < sample_count; index += 100U) {
        document.annotation.values[index] =
            static_cast<std::int32_t>((index / 100U) % 4U);
    }

    document.labeling.id =
        "00000000-0000-4000-8000-000000000078";
    document.labeling.name = "Production ASDF scale benchmark";
    document.labeling.canonical_metadata.created_at =
        RequiredTimestamp("2026-08-31T00:00:00.000Z");
    document.labeling.canonical_metadata.modified_at =
        document.labeling.canonical_metadata.created_at;
    document.labeling.canonical_metadata.origin.kind = "manual";
    document.labeling.canonical_metadata.description =
        "Deterministic production-store benchmark input";
    document.labeling.canonical_metadata.authors = {
        {.name = "Spectiary benchmark"},
    };
    document.labeling.labels = {
        {0, "accepted", "a"},
        {1, "review", "r"},
        {2, "rejected", "x"},
        {3, "uncertain", "u"},
    };

    return BenchmarkDocument{
        .document = std::move(document),
        .roster_width = roster_width,
    };
}

SampleLabelingSourceCompatibility MakeCompatibilityView(
    const SampleLabelingDocument& document) noexcept
{
    return SampleLabelingSourceCompatibility{
        .base_identity = document.source.base_identity,
        .source_kind = document.source.kind,
        .source_name = document.source.name,
        .source_fingerprint = document.source.fingerprint,
        .sample_count = document.source.sample_count,
        .sample_names = document.source.roster.sample_names,
    };
}

void SeedForwardUnknownMetadata(const std::filesystem::path& path)
{
    std::vector<unsigned char> bytes = ReadAllBytes(path);
    ReplaceTextOnce(bytes,
        "\nschema_version: ",
        "\nfuture_benchmark:\n  token: \"production-benchmark-root-survives\"\nschema_version: ");
    ReplaceTextOnce(bytes,
        "\n  labels:\n",
        "\n  future_benchmark:\n    token: \"production-benchmark-task-survives\"\n  labels:\n");
    ReplaceTextOnce(bytes,
        "    shortcut: \"a\"\n",
        "    shortcut: \"a\"\n    future_benchmark: \"production-benchmark-label-survives\"\n");

    std::ofstream output(
        path,
        std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        throw std::runtime_error(
            "production benchmark fixture could not be rewritten");
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output.good()) {
        throw std::runtime_error(
            "production benchmark fixture could not be flushed");
    }
}

bool ContainsSeededForwardUnknownMetadata(
    const std::filesystem::path& path)
{
    const std::vector<unsigned char> bytes = ReadAllBytes(path);
    return ContainsText(bytes, kForwardRootToken) &&
        ContainsText(bytes, kForwardTaskToken) &&
        ContainsText(bytes, kForwardLabelToken);
}

}  // namespace spectiary::asdf_labeling_benchmark
