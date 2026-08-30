#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace specforge {

using CsvRecord = std::vector<std::string>;

// Limits are measured in encoded UTF-8 bytes.  A record includes its commas,
// quotes, and LF/CRLF terminator.  Callers may lower or raise these defaults
// for a specific interchange contract without changing the codec.
struct CsvRecordLimits {
    std::size_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL;
    std::size_t maximum_record_bytes = 8ULL * 1024ULL * 1024ULL;
    std::size_t maximum_field_bytes = 4ULL * 1024ULL * 1024ULL;
    std::size_t maximum_fields_per_record = 65536;
};

enum class CsvRecordErrorKind {
    None,
    IoFailure,
    InvalidUtf8,
    MalformedQuote,
    InvalidLineEnding,
    ResourceLimitExceeded,
};

struct CsvRecordError {
    CsvRecordErrorKind kind = CsvRecordErrorKind::None;
    std::size_t record_index = 0;
    // Number of bytes the codec confirmed as consumed from input or accepted
    // by the output stream buffer before the error was reported.  A stream
    // buffer that throws after an unreported side effect cannot contribute to
    // this observable offset.
    std::size_t byte_offset = 0;
    std::string message;
};

enum class CsvRecordReadStatus {
    Record,
    End,
    Error,
};

struct CsvRecordReadResult {
    CsvRecordReadStatus status = CsvRecordReadStatus::End;
    CsvRecord record;
    CsvRecordError error;

    [[nodiscard]] bool has_record() const noexcept
    {
        return status == CsvRecordReadStatus::Record;
    }
};

class BoundedCsvRecordReader {
public:
    // The codec consumes exact bytes.  A file-backed stream must be opened
    // with std::ios::binary, especially on Windows where text mode translates
    // newlines and invalidates byte limits and embedded-newline round-trips.
    explicit BoundedCsvRecordReader(
        std::istream& input,
        CsvRecordLimits limits = {});

    // Empty files contain no records.  A blank physical line is one record
    // containing one empty field.  Quoted LF and CRLF bytes remain part of the
    // field instead of terminating the logical record.
    [[nodiscard]] CsvRecordReadResult ReadRecord();

    [[nodiscard]] std::size_t bytes_read() const noexcept
    {
        return file_bytes_;
    }

private:
    std::istream& input_;
    CsvRecordLimits limits_;
    std::size_t file_bytes_ = 0;
    std::size_t next_record_index_ = 0;
    bool terminal_ = false;
    CsvRecordError terminal_error_;
};

// Path-level reader that guarantees binary mode for callers which do not
// already own an input stream.
class BoundedCsvFileReader {
public:
    explicit BoundedCsvFileReader(
        const std::filesystem::path& path,
        CsvRecordLimits limits = {});

    BoundedCsvFileReader(const BoundedCsvFileReader&) = delete;
    BoundedCsvFileReader& operator=(const BoundedCsvFileReader&) = delete;
    BoundedCsvFileReader(BoundedCsvFileReader&&) = delete;
    BoundedCsvFileReader& operator=(BoundedCsvFileReader&&) = delete;

    [[nodiscard]] CsvRecordReadResult ReadRecord();

    [[nodiscard]] std::size_t bytes_read() const noexcept
    {
        return reader_.bytes_read();
    }

private:
    std::ifstream input_;
    BoundedCsvRecordReader reader_;
    CsvRecordError open_error_;
};

struct CsvRecordWriteResult {
    bool written = false;
    CsvRecordError error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return written;
    }
};

class BoundedCsvRecordWriter {
public:
    // As with the reader, callers supplying a file-backed stream must open it
    // with std::ios::binary.  WriteCsvRecordsAtomically does this itself.
    explicit BoundedCsvRecordWriter(
        std::ostream& output,
        CsvRecordLimits limits = {});

    // Records use a canonical CRLF terminator.  Fields are quoted only when
    // required for lossless CSV decoding; embedded quotes are doubled.  An
    // empty field span is encoded as one empty field, matching the reader's
    // interpretation of a blank physical line.
    [[nodiscard]] CsvRecordWriteResult WriteRecord(
        std::span<const std::string> fields);

    [[nodiscard]] std::size_t bytes_written() const noexcept
    {
        return file_bytes_;
    }

private:
    std::ostream& output_;
    CsvRecordLimits limits_;
    std::size_t file_bytes_ = 0;
    std::size_t next_record_index_ = 0;
    bool terminal_ = false;
    CsvRecordError terminal_error_;
};

// Encodes every record into a sibling temporary file and replaces the target
// only after the complete bounded stream has been written and closed.
[[nodiscard]] CsvRecordWriteResult WriteCsvRecordsAtomically(
    const std::filesystem::path& path,
    std::span<const CsvRecord> records,
    CsvRecordLimits limits = {});

}  // namespace specforge
