#include "domain/csv_record_codec.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <streambuf>
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

std::vector<specforge::CsvRecord> ReadAll(std::string_view input)
{
    std::istringstream stream{std::string(input)};
    specforge::BoundedCsvRecordReader reader(stream);
    std::vector<specforge::CsvRecord> records;
    while (true) {
        specforge::CsvRecordReadResult result = reader.ReadRecord();
        if (result.status == specforge::CsvRecordReadStatus::End) {
            return records;
        }
        Require(
            result.status == specforge::CsvRecordReadStatus::Record,
            result.error.message);
        records.push_back(std::move(result.record));
    }
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

bool HasTemporarySibling(const std::filesystem::path& root)
{
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root, error)) {
        if (error) {
            return true;
        }
        if (entry.path().filename().string().find(".tmp.") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

class ShortWriteBuffer final : public std::streambuf {
public:
    explicit ShortWriteBuffer(std::size_t maximum_bytes)
        : maximum_bytes_(maximum_bytes)
    {
    }

    [[nodiscard]] std::string_view written() const noexcept
    {
        return written_;
    }

protected:
    std::streamsize xsputn(
        const char* data,
        std::streamsize count) override
    {
        const std::size_t requested = static_cast<std::size_t>(count);
        const std::size_t accepted =
            (std::min)(maximum_bytes_, requested);
        written_.append(data, accepted);
        return static_cast<std::streamsize>(accepted);
    }

private:
    std::size_t maximum_bytes_ = 0;
    std::string written_;
};

void TestWriterAndReaderRoundTripLogicalRecords()
{
    const std::vector<specforge::CsvRecord> expected{
        {"sample", "label"},
        {"  untrimmed  ", "星系"},
        {"comma,value", "say \"hello\""},
        {"line one\nline two", "CRLF\r\ninside"},
        {"", ""},
    };

    std::ostringstream output;
    specforge::BoundedCsvRecordWriter writer(output);
    for (const specforge::CsvRecord& record : expected) {
        Require(
            writer.WriteRecord(record).succeeded(),
            "CSV writer should accept valid UTF-8 records");
    }

    const std::string encoded = output.str();
    Require(
        encoded.find("\"comma,value\",\"say \"\"hello\"\"\"\r\n") !=
            std::string::npos,
        "writer should quote commas and double embedded quotes");
    Require(
        encoded.find("\"line one\nline two\"") != std::string::npos &&
            encoded.find("\"CRLF\r\ninside\"") != std::string::npos,
        "writer should preserve quoted LF and CRLF field bytes");
    Require(
        ReadAll(encoded) == expected,
        "writer output should round-trip by logical CSV record");
}

void TestReaderAcceptsLfAndCrLfWithoutTrimming()
{
    const std::vector<specforge::CsvRecord> records =
        ReadAll("  alpha  , beta \n\"x\r\ny\",z\r\n\n");
    const std::vector<specforge::CsvRecord> expected{
        {"  alpha  ", " beta "},
        {"x\r\ny", "z"},
        {""},
    };
    Require(
        records == expected,
        "reader should accept LF/CRLF records and preserve identity bytes");
}

void TestMalformedQuotesAndLineEndingsAreControlledErrors()
{
    const std::vector<std::string> malformed_quotes{
        "a\"b,c\n",
        "\"unterminated",
        "\"closed\"tail,b\n",
    };
    for (const std::string& input : malformed_quotes) {
        std::istringstream stream(input);
        specforge::BoundedCsvRecordReader reader(stream);
        const specforge::CsvRecordReadResult result = reader.ReadRecord();
        Require(
            result.status == specforge::CsvRecordReadStatus::Error &&
                result.error.kind ==
                    specforge::CsvRecordErrorKind::MalformedQuote,
            "malformed quoting should return a controlled quote error");
    }

    std::istringstream stream("a,b\rc,d");
    specforge::BoundedCsvRecordReader reader(stream);
    const specforge::CsvRecordReadResult result = reader.ReadRecord();
    Require(
        result.status == specforge::CsvRecordReadStatus::Error &&
            result.error.kind ==
                specforge::CsvRecordErrorKind::InvalidLineEnding,
        "bare CR should be rejected instead of splitting a record ambiguously");
}

void TestFailbitExceptionsDoNotReclassifyCleanEof()
{
    std::istringstream stream("a\n");
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    specforge::BoundedCsvRecordReader reader(stream);
    Require(
        reader.ReadRecord().has_record(),
        "reader should return the record before an exception-enabled EOF");
    const specforge::CsvRecordReadResult end = reader.ReadRecord();
    Require(
        end.status == specforge::CsvRecordReadStatus::End,
        "clean EOF must remain End when failbit exceptions are enabled");
}

void TestResourceLimitsDoNotConsumeTheFirstRejectedByte()
{
    enum class LimitKind {
        File,
        Record,
        Field,
    };
    for (const LimitKind kind :
         {LimitKind::File, LimitKind::Record, LimitKind::Field}) {
        specforge::CsvRecordLimits limits;
        if (kind == LimitKind::File) {
            limits.maximum_file_bytes = 3;
        } else if (kind == LimitKind::Record) {
            limits.maximum_record_bytes = 3;
        } else {
            limits.maximum_field_bytes = 3;
        }
        std::istringstream stream("abcd");
        specforge::BoundedCsvRecordReader reader(stream, limits);
        const specforge::CsvRecordReadResult result = reader.ReadRecord();
        Require(
            result.error.kind ==
                specforge::CsvRecordErrorKind::ResourceLimitExceeded,
            "reader should report the selected byte limit");
        Require(
            reader.bytes_read() == 3 &&
                result.error.byte_offset == 3 &&
                stream.peek() == 'd',
            "the first over-limit byte must remain unconsumed");
    }

    specforge::CsvRecordLimits field_count_limits;
    field_count_limits.maximum_fields_per_record = 1;
    std::istringstream stream("a,b");
    specforge::BoundedCsvRecordReader reader(stream, field_count_limits);
    const specforge::CsvRecordReadResult result = reader.ReadRecord();
    Require(
        result.error.kind ==
                specforge::CsvRecordErrorKind::ResourceLimitExceeded &&
            reader.bytes_read() == 1 &&
            result.error.byte_offset == 1 &&
            stream.peek() == ',',
        "field-count rejection should leave its first separator unconsumed");
}

void TestBinaryFileRoundTripPreservesPhysicalAndEmbeddedNewlines()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-csv-binary-file-round-trip";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path path = root / "record.csv";
    const std::vector<specforge::CsvRecord> expected{
        {"line one\r\nline two"},
        {"line one\nline two"},
    };
    Require(
        specforge::WriteCsvRecordsAtomically(path, expected).succeeded(),
        "path-level CSV writer should open its temporary file in binary mode");
    Require(
        ReadFile(path) ==
            "\"line one\r\nline two\"\r\n"
            "\"line one\nline two\"\r\n",
        "binary CSV output should preserve field bytes and canonical CRLF");

    specforge::BoundedCsvFileReader reader(path);
    for (const specforge::CsvRecord& record : expected) {
        const specforge::CsvRecordReadResult result = reader.ReadRecord();
        Require(
            result.has_record() && result.record == record,
            "path-level CSV reader should preserve embedded newline bytes");
    }
    Require(
        reader.ReadRecord().status == specforge::CsvRecordReadStatus::End,
        "path-level CSV reader should reach clean EOF");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestUtf8AndEveryByteLimitAreEnforced()
{
    {
        std::istringstream stream(std::string("\xc3\x28\n", 3));
        specforge::BoundedCsvRecordReader reader(stream);
        const specforge::CsvRecordReadResult result = reader.ReadRecord();
        Require(
            result.status == specforge::CsvRecordReadStatus::Error &&
                result.error.kind ==
                    specforge::CsvRecordErrorKind::InvalidUtf8,
            "reader should reject malformed UTF-8");
    }
    {
        specforge::CsvRecordLimits limits;
        limits.maximum_field_bytes = 3;
        std::istringstream stream("four\n");
        specforge::BoundedCsvRecordReader reader(stream, limits);
        const specforge::CsvRecordReadResult result = reader.ReadRecord();
        Require(
            result.error.kind ==
                specforge::CsvRecordErrorKind::ResourceLimitExceeded,
            "reader should enforce the decoded field-byte limit");
    }
    {
        specforge::CsvRecordLimits limits;
        limits.maximum_record_bytes = 4;
        std::istringstream stream("a,b\r\n");
        specforge::BoundedCsvRecordReader reader(stream, limits);
        const specforge::CsvRecordReadResult result = reader.ReadRecord();
        Require(
            result.error.kind ==
                specforge::CsvRecordErrorKind::ResourceLimitExceeded,
            "reader should include CRLF in the record-byte limit");
    }
    {
        specforge::CsvRecordLimits limits;
        limits.maximum_file_bytes = 4;
        std::istringstream stream("a\nb\nc\n");
        specforge::BoundedCsvRecordReader reader(stream, limits);
        Require(reader.ReadRecord().has_record(), "first bounded record should fit");
        Require(reader.ReadRecord().has_record(), "second bounded record should fit");
        const specforge::CsvRecordReadResult result = reader.ReadRecord();
        Require(
            result.error.kind ==
                specforge::CsvRecordErrorKind::ResourceLimitExceeded,
            "reader should enforce the cumulative file-byte limit");
    }
    {
        specforge::CsvRecordLimits limits;
        limits.maximum_fields_per_record = 1;
        std::istringstream stream("a,b\n");
        specforge::BoundedCsvRecordReader reader(stream, limits);
        const specforge::CsvRecordReadResult result = reader.ReadRecord();
        Require(
            result.error.kind ==
                specforge::CsvRecordErrorKind::ResourceLimitExceeded,
            "reader should bound empty-field amplification");
    }
}

void TestWriterRejectsInvalidUtf8BeforeWritingRecord()
{
    for (const std::string text : {"", "ASCII", "\xc2\xa2\xe4\xb8\xad\xf0\x9f\x98\x80"}) {
        std::ostringstream output;
        specforge::BoundedCsvRecordWriter writer(output);
        Require(writer.WriteRecord(specforge::CsvRecord{text}).succeeded(), "valid UTF-8 should serialize");
        std::istringstream input(output.str());
        specforge::BoundedCsvRecordReader reader(input);
        const auto result = reader.ReadRecord();
        Require(result.has_record() && result.record == specforge::CsvRecord{text},
                "empty, ASCII and multibyte fields should round-trip");
    }
    for (const std::string text : {"\xc3", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80"}) {
        std::ostringstream output;
        specforge::BoundedCsvRecordWriter writer(output);
        Require(writer.WriteRecord(specforge::CsvRecord{text}).error.kind == specforge::CsvRecordErrorKind::InvalidUtf8 && output.str().empty(),
                "invalid Unicode sequences should fail before CSV output");
        std::istringstream input(text + "\n");
        specforge::BoundedCsvRecordReader reader(input);
        Require(reader.ReadRecord().error.kind == specforge::CsvRecordErrorKind::InvalidUtf8,
                "CSV input should reject truncated, overlong, surrogate and out-of-range sequences");
    }
    std::ostringstream output;
    specforge::BoundedCsvRecordWriter writer(output);
    const specforge::CsvRecord record{std::string("\xc3\x28", 2)};
    const specforge::CsvRecordWriteResult result = writer.WriteRecord(record);
    Require(
        !result.succeeded() &&
            result.error.kind == specforge::CsvRecordErrorKind::InvalidUtf8 &&
            output.str().empty(),
        "writer should reject malformed UTF-8 before emitting a record");
}

void TestShortWriteReportsConfirmedPartialByteOffset()
{
    ShortWriteBuffer buffer(3);
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit);
    specforge::BoundedCsvRecordWriter writer(output);
    const specforge::CsvRecordWriteResult result =
        writer.WriteRecord(specforge::CsvRecord{"abcdef"});
    Require(
        !result.succeeded() &&
            result.error.kind == specforge::CsvRecordErrorKind::IoFailure &&
            result.error.byte_offset == 3 &&
            writer.bytes_written() == 3 &&
            buffer.written() == "abc",
        "short write should report the bytes confirmed by the stream buffer");
}

void TestWriterEnforcesRecordFileAndFieldCountLimits()
{
    {
        specforge::CsvRecordLimits limits;
        limits.maximum_record_bytes = 4;
        std::ostringstream output;
        specforge::BoundedCsvRecordWriter writer(output, limits);
        const specforge::CsvRecordWriteResult result =
            writer.WriteRecord(specforge::CsvRecord{"a,b"});
        Require(
            result.error.kind ==
                    specforge::CsvRecordErrorKind::ResourceLimitExceeded &&
                output.str().empty(),
            "writer should bound quoting expansion plus CRLF before output");
    }
    {
        specforge::CsvRecordLimits limits;
        limits.maximum_file_bytes = 4;
        std::ostringstream output;
        specforge::BoundedCsvRecordWriter writer(output, limits);
        Require(
            writer.WriteRecord(specforge::CsvRecord{"a"}).succeeded(),
            "first writer record should fit the file limit");
        const specforge::CsvRecordWriteResult result =
            writer.WriteRecord(specforge::CsvRecord{"b"});
        Require(
            result.error.kind ==
                    specforge::CsvRecordErrorKind::ResourceLimitExceeded &&
                output.str() == "a\r\n",
            "writer should enforce cumulative file bytes before another record");
    }
    {
        specforge::CsvRecordLimits limits;
        limits.maximum_fields_per_record = 0;
        std::ostringstream output;
        specforge::BoundedCsvRecordWriter writer(output, limits);
        const specforge::CsvRecordWriteResult result =
            writer.WriteRecord(specforge::CsvRecord{});
        Require(
            result.error.kind ==
                    specforge::CsvRecordErrorKind::ResourceLimitExceeded &&
                output.str().empty(),
            "a blank CSV record should still count as one empty field");
    }
}

void TestAtomicWriterPreservesTargetWhenALaterRecordExceedsLimits()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-csv-record-codec-tests";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    const std::filesystem::path target = root / "labels.csv";
    {
        std::ofstream stream(target, std::ios::binary);
        stream << "old CSV";
    }

    specforge::CsvRecordLimits limits;
    limits.maximum_field_bytes = 3;
    const std::vector<specforge::CsvRecord> invalid_records{
        {"ok"},
        {"too long"},
    };
    const specforge::CsvRecordWriteResult failed =
        specforge::WriteCsvRecordsAtomically(
            target,
            invalid_records,
            limits);
    Require(
        !failed.succeeded() &&
            failed.error.kind ==
                specforge::CsvRecordErrorKind::ResourceLimitExceeded,
        "atomic writer should return the codec's structured limit error");
    Require(
        ReadFile(target) == "old CSV",
        "failed atomic encoding should preserve the existing target");
    Require(
        !HasTemporarySibling(root),
        "failed atomic encoding should clean its sibling temporary file");

    const std::vector<specforge::CsvRecord> valid_records{
        {"sample", "label"},
        {"a,b", "星系"},
    };
    const specforge::CsvRecordWriteResult written =
        specforge::WriteCsvRecordsAtomically(target, valid_records);
    Require(written.succeeded(), "valid atomic CSV write should succeed");
    Require(
        ReadAll(ReadFile(target)) == valid_records,
        "atomically written CSV should round-trip through the reader");

    std::filesystem::remove_all(root, cleanup_error);
}

}  // namespace

int main()
{
    try {
        TestWriterAndReaderRoundTripLogicalRecords();
        TestReaderAcceptsLfAndCrLfWithoutTrimming();
        TestMalformedQuotesAndLineEndingsAreControlledErrors();
        TestFailbitExceptionsDoNotReclassifyCleanEof();
        TestResourceLimitsDoNotConsumeTheFirstRejectedByte();
        TestBinaryFileRoundTripPreservesPhysicalAndEmbeddedNewlines();
        TestUtf8AndEveryByteLimitAreEnforced();
        TestWriterRejectsInvalidUtf8BeforeWritingRecord();
        TestShortWriteReportsConfirmedPartialByteOffset();
        TestWriterEnforcesRecordFileAndFieldCountLimits();
        TestAtomicWriterPreservesTargetWhenALaterRecordExceedsLimits();
        std::cout << "CSV record codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
