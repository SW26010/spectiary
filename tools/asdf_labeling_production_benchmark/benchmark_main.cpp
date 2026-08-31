#include "benchmark_document_factory.h"

#include "domain/canonical_timestamp.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#ifndef SPECFORGE_BENCHMARK_BUILD_CONFIGURATION
#define SPECFORGE_BENCHMARK_BUILD_CONFIGURATION "unknown"
#endif

#ifndef SPECFORGE_BENCHMARK_COMPILER
#define SPECFORGE_BENCHMARK_COMPILER "unknown"
#endif

namespace {

using specforge::asdf_labeling_benchmark::BenchmarkDocument;
using specforge::asdf_labeling_benchmark::DatasetCase;

enum class Operation {
    InitialWrite,
    ReadOpen,
    ValuesRewriteOne,
    ValuesRewriteThousand,
    MetadataRewrite,
};

struct Options {
    DatasetCase dataset_case = DatasetCase::SourceIndex;
    Operation operation = Operation::InitialWrite;
    std::size_t sample_count =
        specforge::asdf_labeling_benchmark::kProductionSampleCount;
    std::filesystem::path directory;
    std::string commit = "unknown";
    bool has_case = false;
    bool has_operation = false;
};

struct ProcessMemory {
    std::uint64_t working_set = 0;
    std::uint64_t private_bytes = 0;
    std::uint64_t process_peak_working_set = 0;
};

struct Measurement {
    double wall_time_ms = 0.0;
    double cpu_time_ms = 0.0;
    ProcessMemory before;
    std::uint64_t sampled_peak_working_set = 0;
    std::uint64_t sampled_peak_private_bytes = 0;
};

struct Record {
    std::string commit = "unknown";
    std::string build_configuration =
        SPECFORGE_BENCHMARK_BUILD_CONFIGURATION;
    std::string compiler = SPECFORGE_BENCHMARK_COMPILER;
    std::string os;
    std::string cpu;
    std::uint32_t logical_cpu_count = 0;
    std::uint64_t physical_memory = 0;
    std::string filesystem_path;
    std::string dataset_case;
    std::string operation;
    std::size_t sample_count = 0;
    std::string roster_kind;
    std::size_t roster_width = 0;
    std::size_t iterations = 1;
    double wall_time_ms = 0.0;
    double cpu_time_ms = 0.0;
    std::uint64_t file_size_bytes = 0;
    std::uint64_t working_set_before = 0;
    std::uint64_t sampled_peak_working_set = 0;
    std::uint64_t private_bytes_before = 0;
    std::uint64_t sampled_peak_private_bytes = 0;
    std::uint64_t process_peak_working_set = 0;
    bool roster_block_reused = false;
    bool success = false;
    std::string error_kind = "benchmark_setup_failure";
};

[[nodiscard]] Operation ParseOperation(std::string_view value)
{
    if (value == "initial-write") {
        return Operation::InitialWrite;
    }
    if (value == "read-open") {
        return Operation::ReadOpen;
    }
    if (value == "values-rewrite-1") {
        return Operation::ValuesRewriteOne;
    }
    if (value == "values-rewrite-1000") {
        return Operation::ValuesRewriteThousand;
    }
    if (value == "metadata-rewrite") {
        return Operation::MetadataRewrite;
    }
    throw std::invalid_argument(
        "--operation must be initial-write, read-open, values-rewrite-1, "
        "values-rewrite-1000, or metadata-rewrite");
}

[[nodiscard]] std::string_view OperationName(Operation operation)
{
    switch (operation) {
    case Operation::InitialWrite:
        return "initial-write";
    case Operation::ReadOpen:
        return "read-open";
    case Operation::ValuesRewriteOne:
        return "values-rewrite-1";
    case Operation::ValuesRewriteThousand:
        return "values-rewrite-1000";
    case Operation::MetadataRewrite:
        return "metadata-rewrite";
    }
    throw std::invalid_argument("unknown production benchmark operation");
}

[[nodiscard]] std::size_t ParseSize(std::string_view value)
{
    std::size_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        throw std::invalid_argument("--sample-count must be an integer");
    }
    return parsed;
}

void PrintUsage()
{
    std::cout
        << "Usage: specforge_asdf_labeling_production_benchmark "
        << "--case source-index|explicit-unicode "
        << "--operation initial-write|read-open|values-rewrite-1|"
        << "values-rewrite-1000|metadata-rewrite "
        << "--directory PATH [--sample-count N] [--commit SHA]\n";
}

[[nodiscard]] Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help") {
            PrintUsage();
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument(
                "missing value for " + std::string(argument));
        }
        const std::string_view value = argv[++index];
        if (argument == "--case") {
            options.dataset_case =
                specforge::asdf_labeling_benchmark::ParseDatasetCase(value);
            options.has_case = true;
        } else if (argument == "--operation") {
            options.operation = ParseOperation(value);
            options.has_operation = true;
        } else if (argument == "--directory") {
            options.directory = std::filesystem::path(std::string(value));
        } else if (argument == "--sample-count") {
            options.sample_count = ParseSize(value);
        } else if (argument == "--commit") {
            options.commit = value;
        } else {
            throw std::invalid_argument(
                "unknown argument: " + std::string(argument));
        }
    }

    if (!options.has_case || !options.has_operation ||
        options.directory.empty()) {
        throw std::invalid_argument(
            "--case, --operation, and --directory are required");
    }
    if (options.operation == Operation::ValuesRewriteThousand &&
        options.sample_count < 1'000U) {
        throw std::invalid_argument(
            "values-rewrite-1000 requires at least 1000 samples");
    }
    return options;
}

[[nodiscard]] std::string PathUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return "unknown";
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            size,
            nullptr,
            nullptr) != size) {
        return "unknown";
    }
    return output;
}

[[nodiscard]] std::string OperatingSystemDescription()
{
    using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return "Windows";
    }
    const auto rtl_get_version = reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version == nullptr) {
        return "Windows";
    }
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version(&version) != 0) {
        return "Windows";
    }
    return "Windows " + std::to_string(version.dwMajorVersion) + "." +
        std::to_string(version.dwMinorVersion) + " build " +
        std::to_string(version.dwBuildNumber);
}

[[nodiscard]] std::string CpuDescription()
{
    const DWORD required =
        GetEnvironmentVariableW(L"PROCESSOR_IDENTIFIER", nullptr, 0);
    if (required == 0U) {
        return "unknown";
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        L"PROCESSOR_IDENTIFIER", value.data(), required);
    if (written == 0U || written >= required) {
        return "unknown";
    }
    value.resize(written);
    return WideToUtf8(value);
}

[[nodiscard]] std::uint64_t PhysicalMemoryBytes()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return 0;
    }
    return status.ullTotalPhys;
}

[[nodiscard]] ProcessMemory QueryProcessMemory()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        throw std::runtime_error("GetProcessMemoryInfo failed");
    }
    return ProcessMemory{
        .working_set = static_cast<std::uint64_t>(counters.WorkingSetSize),
        .private_bytes = static_cast<std::uint64_t>(counters.PrivateUsage),
        .process_peak_working_set =
            static_cast<std::uint64_t>(counters.PeakWorkingSetSize),
    };
}

[[nodiscard]] std::uint64_t FileTimeTicks(FILETIME value) noexcept
{
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

[[nodiscard]] std::uint64_t ProcessCpuTicks()
{
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(
            GetCurrentProcess(),
            &created,
            &exited,
            &kernel,
            &user)) {
        throw std::runtime_error("GetProcessTimes failed");
    }
    return FileTimeTicks(kernel) + FileTimeTicks(user);
}

void AtomicMaximum(std::atomic<std::uint64_t>& value,
    std::uint64_t candidate) noexcept
{
    std::uint64_t observed = value.load(std::memory_order_relaxed);
    while (candidate > observed &&
           !value.compare_exchange_weak(
               observed,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

class MemorySampler final {
public:
    MemorySampler()
    {
        thread_ = std::thread([this]() { Run(); });
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this]() { return ready_; });
    }

    MemorySampler(const MemorySampler&) = delete;
    MemorySampler& operator=(const MemorySampler&) = delete;

    ~MemorySampler()
    {
        Stop();
    }

    void SetBaseline(const ProcessMemory& memory) noexcept
    {
        peak_working_set_.store(
            memory.working_set, std::memory_order_relaxed);
        peak_private_bytes_.store(
            memory.private_bytes, std::memory_order_relaxed);
    }

    void Begin()
    {
        {
            std::lock_guard lock(mutex_);
            begun_ = true;
        }
        condition_.notify_one();
    }

    void Stop() noexcept
    {
        EndSampling();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void EndSampling() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        condition_.notify_one();
    }

    [[nodiscard]] std::uint64_t peak_working_set() const noexcept
    {
        return peak_working_set_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t peak_private_bytes() const noexcept
    {
        return peak_private_bytes_.load(std::memory_order_relaxed);
    }

private:
    void SampleOnce() noexcept
    {
        try {
            const ProcessMemory memory = QueryProcessMemory();
            AtomicMaximum(peak_working_set_, memory.working_set);
            AtomicMaximum(peak_private_bytes_, memory.private_bytes);
        } catch (...) {
        }
    }

    void Run() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            ready_ = true;
        }
        condition_.notify_one();

        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this]() { return begun_ || stop_; });
        while (!stop_) {
            lock.unlock();
            SampleOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    bool ready_ = false;
    bool begun_ = false;
    bool stop_ = false;
    std::thread thread_;
    std::atomic<std::uint64_t> peak_working_set_ = 0;
    std::atomic<std::uint64_t> peak_private_bytes_ = 0;
};

template <typename Callable>
[[nodiscard]] auto MeasureProductionCall(Callable&& callable)
{
    using Result = std::invoke_result_t<Callable>;
    MemorySampler sampler;
    const ProcessMemory before = QueryProcessMemory();
    sampler.SetBaseline(before);
    const std::uint64_t cpu_before = ProcessCpuTicks();
    const auto wall_before = std::chrono::steady_clock::now();
    sampler.Begin();
    Result result = std::invoke(std::forward<Callable>(callable));
    const auto wall_after = std::chrono::steady_clock::now();
    sampler.EndSampling();
    const std::uint64_t cpu_after = ProcessCpuTicks();
    sampler.Stop();

    const std::chrono::duration<double, std::milli> elapsed =
        wall_after - wall_before;
    return std::pair<Result, Measurement>{
        std::move(result),
        Measurement{
            .wall_time_ms = elapsed.count(),
            .cpu_time_ms =
                static_cast<double>(cpu_after - cpu_before) / 10'000.0,
            .before = before,
            .sampled_peak_working_set = sampler.peak_working_set(),
            .sampled_peak_private_bytes = sampler.peak_private_bytes(),
        },
    };
}

[[nodiscard]] specforge::CanonicalTimestamp RequiredTimestamp(
    std::string_view text)
{
    const std::optional<specforge::CanonicalTimestamp> parsed =
        specforge::ParseCanonicalTimestamp(text);
    if (!parsed) {
        throw std::runtime_error("benchmark timestamp is invalid");
    }
    return *parsed;
}

[[nodiscard]] std::string CodecErrorName(
    specforge::SampleLabelingAsdfErrorKind kind)
{
    using Kind = specforge::SampleLabelingAsdfErrorKind;
    switch (kind) {
    case Kind::None:
        return "none";
    case Kind::OpenFailed:
        return "open_failed";
    case Kind::IoFailure:
        return "io_failure";
    case Kind::MalformedDocument:
        return "malformed_document";
    case Kind::UnsupportedProfile:
        return "unsupported_profile";
    case Kind::ResourceLimitExceeded:
        return "resource_limit_exceeded";
    case Kind::SemanticValidationFailed:
        return "semantic_validation_failed";
    }
    return "unknown_codec_failure";
}

[[nodiscard]] std::string StoreErrorName(
    const specforge::SampleLabelingAsdfStoreError& error)
{
    using Kind = specforge::SampleLabelingAsdfStoreErrorKind;
    switch (error.kind) {
    case Kind::None:
        return "none";
    case Kind::CodecFailure:
        return "codec_failure." + CodecErrorName(error.codec_kind);
    case Kind::SourceMismatch:
        return "source_mismatch";
    case Kind::PreservationIdentityMismatch:
        return "preservation_identity_mismatch";
    case Kind::DurableBaseUnavailable:
        return "durable_base_unavailable";
    case Kind::AtomicWriteFailure:
        return "atomic_write_failure";
    case Kind::PublishedGenerationMismatch:
        return "published_generation_mismatch";
    }
    return "unknown_store_failure";
}

void ApplyMeasurement(Record& record, const Measurement& measurement)
{
    record.wall_time_ms = measurement.wall_time_ms;
    record.cpu_time_ms = measurement.cpu_time_ms;
    record.working_set_before = measurement.before.working_set;
    record.private_bytes_before = measurement.before.private_bytes;
    record.sampled_peak_working_set =
        measurement.sampled_peak_working_set;
    record.sampled_peak_private_bytes =
        measurement.sampled_peak_private_bytes;
}

void CreateFreshDirectory(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::runtime_error(
            "could not create benchmark directory: " + error.message());
    }
    if (!std::filesystem::is_empty(directory, error) || error) {
        throw std::runtime_error(
            "benchmark directory must be empty and unique per subprocess");
    }
}

void WriteSetupDocument(const std::filesystem::path& path,
    const BenchmarkDocument& benchmark_document)
{
    const specforge::SampleLabelingSourceCompatibility compatibility =
        specforge::asdf_labeling_benchmark::MakeCompatibilityView(
            benchmark_document.document);
    const specforge::SampleLabelingAsdfStoreGenerationWriteResult setup =
        specforge::WriteSampleLabelingAsdfDocumentAndOpenAtomically(
            path,
            benchmark_document.document,
            compatibility);
    if (!setup.succeeded()) {
        throw std::runtime_error(
            "production store setup failed: " + setup.error.message);
    }
}

void MutateValues(
    specforge::SampleLabelingDocument& replacement,
    std::size_t mutation_count)
{
    if (replacement.annotation.values.size() < mutation_count) {
        throw std::runtime_error(
            "benchmark dataset is smaller than the mutation count");
    }
    const std::size_t stride =
        replacement.annotation.values.size() / mutation_count;
    for (std::size_t index = 0; index < mutation_count; ++index) {
        const std::size_t value_index = index * stride;
        std::int32_t& value = replacement.annotation.values[value_index];
        value = value == 3 ? 0 : value + 1;
    }
    replacement.labeling.canonical_metadata.modified_at =
        RequiredTimestamp("2026-08-31T00:00:01.000Z");
}

[[nodiscard]] std::uint64_t FileSize(const std::filesystem::path& path)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > std::numeric_limits<std::uint64_t>::max()) {
        return 0;
    }
    return static_cast<std::uint64_t>(size);
}

void RunOperation(const Options& options, Record& record)
{
    const std::filesystem::path directory =
        std::filesystem::absolute(options.directory);
    CreateFreshDirectory(directory);
    const std::filesystem::path path = directory / "labels.asdf";
    record.filesystem_path = PathUtf8(directory);

    BenchmarkDocument benchmark_document =
        specforge::asdf_labeling_benchmark::MakeBenchmarkDocument(
            options.dataset_case, options.sample_count);
    record.roster_width = benchmark_document.roster_width;
    record.roster_kind =
        benchmark_document.document.source.roster.identity_kind;
    const specforge::SampleLabelingSourceCompatibility compatibility =
        specforge::asdf_labeling_benchmark::MakeCompatibilityView(
            benchmark_document.document);

    if (options.operation == Operation::InitialWrite) {
        auto [result, measurement] = MeasureProductionCall([&]() {
            return specforge::WriteSampleLabelingAsdfDocumentAndOpenAtomically(
                path,
                benchmark_document.document,
                compatibility);
        });
        ApplyMeasurement(record, measurement);
        record.success = result.succeeded() && result.document_replaced;
        record.error_kind = record.success
            ? "none"
            : (result.succeeded()
                   ? "benchmark_validation_failure"
                   : StoreErrorName(result.error));
    } else {
        WriteSetupDocument(path, benchmark_document);
        specforge::asdf_labeling_benchmark::SeedForwardUnknownMetadata(path);

        if (options.operation == Operation::ReadOpen) {
            auto [result, measurement] = MeasureProductionCall([&]() {
                return specforge::OpenSampleLabelingAsdfDocumentStore(
                    path, compatibility);
            });
            ApplyMeasurement(record, measurement);
            const bool validated = result.succeeded() &&
                result.snapshot->durable_base().valid() &&
                result.snapshot->document().source.sample_count ==
                    options.sample_count &&
                specforge::asdf_labeling_benchmark::
                    ContainsSeededForwardUnknownMetadata(path);
            record.success = validated;
            record.error_kind = validated
                ? "none"
                : (result.succeeded()
                       ? "benchmark_validation_failure"
                       : StoreErrorName(result.error));
        } else {
            specforge::SampleLabelingAsdfStoreOpenResult opened =
                specforge::OpenSampleLabelingAsdfDocumentStore(
                    path, compatibility);
            if (!opened.succeeded()) {
                throw std::runtime_error(
                    "production store rewrite setup open failed: " +
                    opened.error.message);
            }
            specforge::SampleLabelingDocument replacement =
                opened.snapshot->document();

            if (options.operation == Operation::MetadataRewrite) {
                replacement.labeling.name =
                    "Production ASDF scale benchmark metadata rewrite";
                replacement.labeling.canonical_metadata.modified_at =
                    RequiredTimestamp("2026-08-31T00:00:02.000Z");
                auto [result, measurement] = MeasureProductionCall([&]() {
                    return specforge::
                        RewriteSampleLabelingAsdfDocumentAndReopenAtomically(
                            *opened.snapshot,
                            replacement,
                            compatibility);
                });
                ApplyMeasurement(record, measurement);
                const bool validated = result.succeeded() &&
                    result.document_replaced &&
                    result.snapshot->document().labeling.name ==
                        replacement.labeling.name &&
                    specforge::asdf_labeling_benchmark::
                        ContainsSeededForwardUnknownMetadata(path);
                record.success = validated;
                record.error_kind = validated
                    ? "none"
                    : (result.succeeded()
                           ? "benchmark_validation_failure"
                           : StoreErrorName(result.error));
            } else {
                const std::size_t mutation_count =
                    options.operation == Operation::ValuesRewriteOne
                    ? 1U
                    : 1'000U;
                MutateValues(replacement, mutation_count);
                auto [result, measurement] = MeasureProductionCall([&]() {
                    return specforge::RewriteSampleLabelingAsdfValuesAtomically(
                        *opened.snapshot, replacement);
                });
                ApplyMeasurement(record, measurement);
                record.roster_block_reused = result.roster_block_reused;
                const bool expected_reuse =
                    options.dataset_case == DatasetCase::ExplicitUnicode;
                const bool validated = result.succeeded() &&
                    result.roster_block_reused == expected_reuse &&
                    opened.snapshot->document().annotation.values ==
                        replacement.annotation.values &&
                    specforge::asdf_labeling_benchmark::
                        ContainsSeededForwardUnknownMetadata(path);
                record.success = validated;
                record.error_kind = validated
                    ? "none"
                    : (result.succeeded()
                           ? "benchmark_validation_failure"
                           : StoreErrorName(result.error));
            }
        }
    }

    record.file_size_bytes = FileSize(path);
    record.process_peak_working_set =
        QueryProcessMemory().process_peak_working_set;
}

[[nodiscard]] std::string EscapeJson(std::string_view value)
{
    constexpr std::string_view hex = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size() + 16U);
    for (unsigned char character : value) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (character < 0x20U) {
                escaped += "\\u00";
                escaped.push_back(hex[character >> 4U]);
                escaped.push_back(hex[character & 0x0fU]);
            } else {
                escaped.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return escaped;
}

void WriteJsonString(std::ostream& output, std::string_view value)
{
    output << '"' << EscapeJson(value) << '"';
}

void PrintRecord(const Record& record)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(3) << '{';
    const auto text_field = [&output](
                                std::string_view name,
                                std::string_view value) {
        WriteJsonString(output, name);
        output << ':';
        WriteJsonString(output, value);
        output << ',';
    };
    const auto integer_field = [&output](
                                   std::string_view name,
                                   std::uint64_t value) {
        WriteJsonString(output, name);
        output << ':' << value << ',';
    };
    const auto number_field = [&output](
                                  std::string_view name,
                                  double value) {
        WriteJsonString(output, name);
        output << ':' << value << ',';
    };

    text_field("commit", record.commit);
    text_field("build_configuration", record.build_configuration);
    text_field("compiler", record.compiler);
    text_field("os", record.os);
    text_field("cpu", record.cpu);
    integer_field("logical_cpu_count", record.logical_cpu_count);
    integer_field("physical_memory", record.physical_memory);
    text_field("filesystem_path", record.filesystem_path);
    text_field("case", record.dataset_case);
    text_field("operation", record.operation);
    integer_field("sample_count", record.sample_count);
    text_field("roster_kind", record.roster_kind);
    integer_field("roster_width", record.roster_width);
    integer_field("iterations", record.iterations);
    number_field("wall_time_ms", record.wall_time_ms);
    number_field("cpu_time_ms", record.cpu_time_ms);
    integer_field("file_size_bytes", record.file_size_bytes);
    integer_field("working_set_before", record.working_set_before);
    integer_field(
        "sampled_peak_working_set", record.sampled_peak_working_set);
    integer_field("private_bytes_before", record.private_bytes_before);
    integer_field(
        "sampled_peak_private_bytes", record.sampled_peak_private_bytes);
    integer_field(
        "process_peak_working_set", record.process_peak_working_set);
    WriteJsonString(output, "roster_block_reused");
    output << ':' << (record.roster_block_reused ? "true" : "false")
           << ',';
    WriteJsonString(output, "success");
    output << ':' << (record.success ? "true" : "false") << ',';
    WriteJsonString(output, "error_kind");
    output << ':';
    WriteJsonString(output, record.error_kind);
    output << '}';
    std::cout << output.str() << '\n';
}

[[nodiscard]] Record MakeBaseRecord(const Options& options)
{
    Record record;
    record.commit = options.commit;
    record.os = OperatingSystemDescription();
    record.cpu = CpuDescription();
    record.logical_cpu_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    record.physical_memory = PhysicalMemoryBytes();
    record.filesystem_path = options.directory.empty()
        ? std::string{}
        : PathUtf8(std::filesystem::absolute(options.directory));
    record.dataset_case = std::string(
        specforge::asdf_labeling_benchmark::DatasetCaseName(
            options.dataset_case));
    record.operation = std::string(OperationName(options.operation));
    record.sample_count = options.sample_count;
    record.roster_kind = options.dataset_case == DatasetCase::ExplicitUnicode
        ? std::string{specforge::kSampleLabelingDocumentExplicitNamesRoster}
        : std::string{specforge::kSampleLabelingDocumentSourceIndexRoster};
    record.roster_width =
        options.dataset_case == DatasetCase::ExplicitUnicode
        ? specforge::asdf_labeling_benchmark::
              kExplicitUnicodeRosterWidth
        : 0U;
    return record;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    try {
        options = ParseOptions(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        PrintUsage();
        return 2;
    }

    Record record = MakeBaseRecord(options);
    try {
        RunOperation(options, record);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        record.error_kind = "benchmark_setup_failure";
        try {
            record.process_peak_working_set =
                QueryProcessMemory().process_peak_working_set;
        } catch (...) {
        }
    } catch (...) {
        std::cerr << "unknown production benchmark failure\n";
        record.error_kind = "benchmark_setup_failure";
    }
    PrintRecord(record);
    return record.success ? 0 : 1;
}
