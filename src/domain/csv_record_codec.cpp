#include "domain/csv_record_codec.h"

#include "domain/utf8.h"
#include "platform/atomic_file.h"


#include <ios>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string_view>
#include <utility>

namespace spectiary {
namespace {

[[nodiscard]] CsvRecordError MakeError(
    CsvRecordErrorKind kind,
    std::size_t record_index,
    std::size_t byte_offset,
    std::string message)
{
    return CsvRecordError{
        .kind = kind,
        .record_index = record_index,
        .byte_offset = byte_offset,
        .message = std::move(message),
    };
}

[[nodiscard]] bool NeedsQuotes(std::string_view field)
{
    return field.find_first_of(",\"\r\n") != std::string_view::npos;
}

}  // namespace

BoundedCsvRecordReader::BoundedCsvRecordReader(
    std::istream& input,
    CsvRecordLimits limits)
    : input_(input), limits_(limits)
{
}

CsvRecordReadResult BoundedCsvRecordReader::ReadRecord()
{
    if (terminal_) {
        if (terminal_error_.kind == CsvRecordErrorKind::None) {
            return CsvRecordReadResult{
                .status = CsvRecordReadStatus::End,
            };
        }
        return CsvRecordReadResult{
            .status = CsvRecordReadStatus::Error,
            .error = terminal_error_,
        };
    }

    enum class FieldState {
        Start,
        Unquoted,
        Quoted,
        AfterClosingQuote,
    };

    const std::size_t record_index = next_record_index_;
    CsvRecord record;
    std::string field;
    FieldState state = FieldState::Start;
    std::size_t record_bytes = 0;
    bool saw_record_byte = false;

    auto fail = [&](CsvRecordError error) {
        terminal_ = true;
        terminal_error_ = std::move(error);
        return CsvRecordReadResult{
            .status = CsvRecordReadStatus::Error,
            .error = terminal_error_,
        };
    };

    auto finish_field = [&]() -> std::optional<CsvRecordError> {
        if (!IsValidUtf8(field)) {
            return MakeError(
                CsvRecordErrorKind::InvalidUtf8,
                record_index,
                file_bytes_,
                "CSV field is not valid UTF-8");
        }
        if (record.size() >= limits_.maximum_fields_per_record) {
            return MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                record_index,
                file_bytes_,
                "CSV record exceeds the field-count limit");
        }
        record.push_back(std::move(field));
        field.clear();
        return std::nullopt;
    };

    CsvRecordError byte_error;
    auto read_byte = [&](bool& clean_eof) -> std::optional<char> {
        std::istream::int_type value = std::istream::traits_type::eof();
        try {
            value = input_.peek();
        } catch (const std::ios_base::failure&) {
            clean_eof = input_.eof() && !input_.bad();
            if (!clean_eof) {
                byte_error = MakeError(
                    CsvRecordErrorKind::IoFailure,
                    record_index,
                    file_bytes_,
                    "could not read CSV input");
            }
            return std::nullopt;
        }
        if (value == std::istream::traits_type::eof()) {
            clean_eof = input_.eof() && !input_.bad();
            if (!clean_eof) {
                byte_error = MakeError(
                    CsvRecordErrorKind::IoFailure,
                    record_index,
                    file_bytes_,
                    "could not read CSV input");
            }
            return std::nullopt;
        }
        clean_eof = false;
        if (file_bytes_ >= limits_.maximum_file_bytes) {
            byte_error = MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                record_index,
                file_bytes_,
                "CSV input exceeds the file-size limit");
            return std::nullopt;
        }
        if (record_bytes >= limits_.maximum_record_bytes) {
            byte_error = MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                record_index,
                file_bytes_,
                "CSV record exceeds the record-size limit");
            return std::nullopt;
        }

        const char pending = static_cast<char>(value);
        const bool outside_quotes = state != FieldState::Quoted;
        const bool is_separator = outside_quotes && pending == ',';
        const bool is_record_terminator =
            outside_quotes && (pending == '\r' || pending == '\n');
        const bool starts_field =
            state == FieldState::Start &&
            !is_separator && !is_record_terminator;
        const bool too_many_fields_at_separator =
            is_separator &&
            (record.size() >= limits_.maximum_fields_per_record ||
             limits_.maximum_fields_per_record - record.size() < 2);
        if (too_many_fields_at_separator ||
            ((is_record_terminator || starts_field) &&
             record.size() >= limits_.maximum_fields_per_record)) {
            byte_error = MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                record_index,
                file_bytes_,
                "CSV record exceeds the field-count limit");
            return std::nullopt;
        }

        const bool appends_to_field =
            (state == FieldState::Start &&
             pending != '"' && !is_separator && !is_record_terminator) ||
            (state == FieldState::Unquoted &&
             pending != '"' && !is_separator && !is_record_terminator) ||
            (state == FieldState::Quoted && pending != '"') ||
            (state == FieldState::AfterClosingQuote && pending == '"');
        if (appends_to_field &&
            field.size() >= limits_.maximum_field_bytes) {
            byte_error = MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                record_index,
                file_bytes_,
                "CSV field exceeds the field-size limit");
            return std::nullopt;
        }

        try {
            value = input_.get();
        } catch (const std::ios_base::failure&) {
            byte_error = MakeError(
                CsvRecordErrorKind::IoFailure,
                record_index,
                file_bytes_,
                "could not read CSV input");
            return std::nullopt;
        }
        if (value == std::istream::traits_type::eof()) {
            byte_error = MakeError(
                CsvRecordErrorKind::IoFailure,
                record_index,
                file_bytes_,
                "could not read CSV input");
            return std::nullopt;
        }
        ++file_bytes_;
        ++record_bytes;
        return static_cast<char>(value);
    };

    auto append_field_byte = [&](char value) -> std::optional<CsvRecordError> {
        if (field.size() >= limits_.maximum_field_bytes) {
            return MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                record_index,
                file_bytes_,
                "CSV field exceeds the field-size limit");
        }
        field.push_back(value);
        return std::nullopt;
    };

    auto finish_record = [&]() -> CsvRecordReadResult {
        if (const std::optional<CsvRecordError> error = finish_field()) {
            return fail(*error);
        }
        ++next_record_index_;
        return CsvRecordReadResult{
            .status = CsvRecordReadStatus::Record,
            .record = std::move(record),
        };
    };

    while (true) {
        bool clean_eof = false;
        const std::optional<char> next = read_byte(clean_eof);
        if (!next.has_value()) {
            if (byte_error.kind != CsvRecordErrorKind::None) {
                return fail(std::move(byte_error));
            }
            if (!clean_eof) {
                return fail(MakeError(
                    CsvRecordErrorKind::IoFailure,
                    record_index,
                    file_bytes_,
                    "could not read CSV input"));
            }
            if (!saw_record_byte && state == FieldState::Start &&
                record.empty() && field.empty()) {
                terminal_ = true;
                return CsvRecordReadResult{
                    .status = CsvRecordReadStatus::End,
                };
            }
            if (state == FieldState::Quoted) {
                return fail(MakeError(
                    CsvRecordErrorKind::MalformedQuote,
                    record_index,
                    file_bytes_,
                    "CSV input ends inside a quoted field"));
            }
            return finish_record();
        }

        const char value = *next;
        saw_record_byte = true;

        if (state != FieldState::Quoted && value == '\r') {
            bool crlf_eof = false;
            const std::optional<char> following = read_byte(crlf_eof);
            if (!following.has_value()) {
                if (byte_error.kind != CsvRecordErrorKind::None) {
                    return fail(std::move(byte_error));
                }
                return fail(MakeError(
                    CsvRecordErrorKind::InvalidLineEnding,
                    record_index,
                    file_bytes_,
                    "CSV record terminator must be LF or CRLF"));
            }
            if (*following != '\n') {
                return fail(MakeError(
                    CsvRecordErrorKind::InvalidLineEnding,
                    record_index,
                    file_bytes_,
                    "CSV record terminator must be LF or CRLF"));
            }
            return finish_record();
        }

        switch (state) {
        case FieldState::Start:
            if (value == '"') {
                state = FieldState::Quoted;
            } else if (value == ',') {
                if (const std::optional<CsvRecordError> error =
                        finish_field()) {
                    return fail(*error);
                }
            } else if (value == '\n') {
                return finish_record();
            } else {
                if (const std::optional<CsvRecordError> error =
                        append_field_byte(value)) {
                    return fail(*error);
                }
                state = FieldState::Unquoted;
            }
            break;

        case FieldState::Unquoted:
            if (value == '"') {
                return fail(MakeError(
                    CsvRecordErrorKind::MalformedQuote,
                    record_index,
                    file_bytes_,
                    "quote appears inside an unquoted CSV field"));
            }
            if (value == ',') {
                if (const std::optional<CsvRecordError> error =
                        finish_field()) {
                    return fail(*error);
                }
                state = FieldState::Start;
            } else if (value == '\n') {
                return finish_record();
            } else {
                if (const std::optional<CsvRecordError> error =
                        append_field_byte(value)) {
                    return fail(*error);
                }
            }
            break;

        case FieldState::Quoted:
            if (value == '"') {
                state = FieldState::AfterClosingQuote;
            } else {
                if (const std::optional<CsvRecordError> error =
                        append_field_byte(value)) {
                    return fail(*error);
                }
            }
            break;

        case FieldState::AfterClosingQuote:
            if (value == '"') {
                if (const std::optional<CsvRecordError> error =
                        append_field_byte('"')) {
                    return fail(*error);
                }
                state = FieldState::Quoted;
            } else if (value == ',') {
                if (const std::optional<CsvRecordError> error =
                        finish_field()) {
                    return fail(*error);
                }
                state = FieldState::Start;
            } else if (value == '\n') {
                return finish_record();
            } else {
                return fail(MakeError(
                    CsvRecordErrorKind::MalformedQuote,
                    record_index,
                    file_bytes_,
                    "unexpected byte after a closing CSV quote"));
            }
            break;
        }
    }
}

BoundedCsvFileReader::BoundedCsvFileReader(
    const std::filesystem::path& path,
    CsvRecordLimits limits)
    : input_(path, std::ios::in | std::ios::binary),
      reader_(input_, limits)
{
    if (!input_.is_open()) {
        open_error_ = MakeError(
            CsvRecordErrorKind::IoFailure,
            0,
            0,
            "could not open CSV file for reading: " + path.string());
    }
}

CsvRecordReadResult BoundedCsvFileReader::ReadRecord()
{
    if (open_error_.kind != CsvRecordErrorKind::None) {
        return CsvRecordReadResult{
            .status = CsvRecordReadStatus::Error,
            .error = open_error_,
        };
    }
    return reader_.ReadRecord();
}

BoundedCsvRecordWriter::BoundedCsvRecordWriter(
    std::ostream& output,
    CsvRecordLimits limits)
    : output_(output), limits_(limits)
{
}

CsvRecordWriteResult BoundedCsvRecordWriter::WriteRecord(
    std::span<const std::string> fields)
{
    if (terminal_) {
        return CsvRecordWriteResult{
            .error = terminal_error_,
        };
    }

    auto fail = [&](CsvRecordError error) {
        terminal_ = true;
        terminal_error_ = std::move(error);
        return CsvRecordWriteResult{
            .error = terminal_error_,
        };
    };

    const std::size_t logical_field_count =
        fields.empty() ? 1 : fields.size();
    if (logical_field_count > limits_.maximum_fields_per_record) {
        return fail(MakeError(
            CsvRecordErrorKind::ResourceLimitExceeded,
            next_record_index_,
            file_bytes_,
            "CSV record exceeds the field-count limit"));
    }

    std::string encoded;
    auto append = [&](char value) -> bool {
        if (encoded.size() >= limits_.maximum_record_bytes) {
            return false;
        }
        encoded.push_back(value);
        return true;
    };

    for (std::size_t field_index = 0;
         field_index < fields.size();
         ++field_index) {
        const std::string& field = fields[field_index];
        if (field.size() > limits_.maximum_field_bytes) {
            return fail(MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                next_record_index_,
                file_bytes_,
                "CSV field exceeds the field-size limit"));
        }
        if (!IsValidUtf8(field)) {
            return fail(MakeError(
                CsvRecordErrorKind::InvalidUtf8,
                next_record_index_,
                file_bytes_,
                "CSV field is not valid UTF-8"));
        }
        if (field_index != 0 && !append(',')) {
            return fail(MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                next_record_index_,
                file_bytes_,
                "CSV record exceeds the record-size limit"));
        }

        const bool quoted = NeedsQuotes(field);
        if (quoted && !append('"')) {
            return fail(MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                next_record_index_,
                file_bytes_,
                "CSV record exceeds the record-size limit"));
        }
        for (const char value : field) {
            if (!append(value) || (value == '"' && !append('"'))) {
                return fail(MakeError(
                    CsvRecordErrorKind::ResourceLimitExceeded,
                    next_record_index_,
                    file_bytes_,
                    "CSV record exceeds the record-size limit"));
            }
        }
        if (quoted && !append('"')) {
            return fail(MakeError(
                CsvRecordErrorKind::ResourceLimitExceeded,
                next_record_index_,
                file_bytes_,
                "CSV record exceeds the record-size limit"));
        }
    }

    if (!append('\r') || !append('\n')) {
        return fail(MakeError(
            CsvRecordErrorKind::ResourceLimitExceeded,
            next_record_index_,
            file_bytes_,
            "CSV record exceeds the record-size limit"));
    }
    if (file_bytes_ > limits_.maximum_file_bytes ||
        encoded.size() > limits_.maximum_file_bytes - file_bytes_) {
        return fail(MakeError(
            CsvRecordErrorKind::ResourceLimitExceeded,
            next_record_index_,
            file_bytes_,
            "CSV output exceeds the file-size limit"));
    }

    if (encoded.size() > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max())) {
        return fail(MakeError(
            CsvRecordErrorKind::ResourceLimitExceeded,
            next_record_index_,
            file_bytes_,
            "CSV record exceeds the output stream-size limit"));
    }
    if (!output_ || output_.rdbuf() == nullptr) {
        return fail(MakeError(
            CsvRecordErrorKind::IoFailure,
            next_record_index_,
            file_bytes_,
            "could not write CSV output"));
    }

    std::streamsize accepted = 0;
    try {
        std::ostream::sentry sentry(output_);
        if (!sentry) {
            return fail(MakeError(
                CsvRecordErrorKind::IoFailure,
                next_record_index_,
                file_bytes_,
                "could not write CSV output"));
        }
        accepted = output_.rdbuf()->sputn(
            encoded.data(),
            static_cast<std::streamsize>(encoded.size()));
    } catch (const std::ios_base::failure&) {
        return fail(MakeError(
            CsvRecordErrorKind::IoFailure,
            next_record_index_,
            file_bytes_,
            "could not write CSV output"));
    }

    if (accepted < 0 ||
        accepted > static_cast<std::streamsize>(encoded.size())) {
        try {
            output_.setstate(std::ios::badbit);
        } catch (const std::ios_base::failure&) {
        }
        return fail(MakeError(
            CsvRecordErrorKind::IoFailure,
            next_record_index_,
            file_bytes_,
            "CSV output stream returned an invalid write count"));
    }

    file_bytes_ += static_cast<std::size_t>(accepted);
    if (static_cast<std::size_t>(accepted) != encoded.size()) {
        try {
            output_.setstate(std::ios::badbit);
        } catch (const std::ios_base::failure&) {
        }
        return fail(MakeError(
            CsvRecordErrorKind::IoFailure,
            next_record_index_,
            file_bytes_,
            "could not write complete CSV record"));
    }
    if (!output_) {
        return fail(MakeError(
            CsvRecordErrorKind::IoFailure,
            next_record_index_,
            file_bytes_,
            "could not write CSV output"));
    }

    ++next_record_index_;
    return CsvRecordWriteResult{
        .written = true,
    };
}

CsvRecordWriteResult WriteCsvRecordsAtomically(
    const std::filesystem::path& path,
    std::span<const CsvRecord> records,
    CsvRecordLimits limits)
{
    CsvRecordError codec_error;
    std::string atomic_error;
    AtomicFileWriteOptions options;
    options.open_mode = std::ios::binary | std::ios::trunc;
    options.target_description = "CSV file";

    const bool written = WriteFileAtomically(
        path,
        options,
        [&](std::ostream& output, std::string& error) {
            BoundedCsvRecordWriter writer(output, limits);
            for (const CsvRecord& record : records) {
                const CsvRecordWriteResult result =
                    writer.WriteRecord(record);
                if (!result.succeeded()) {
                    codec_error = result.error;
                    error = result.error.message;
                    return false;
                }
            }
            return true;
        },
        &atomic_error);
    if (written) {
        return CsvRecordWriteResult{
            .written = true,
        };
    }
    if (codec_error.kind != CsvRecordErrorKind::None) {
        return CsvRecordWriteResult{
            .error = std::move(codec_error),
        };
    }
    return CsvRecordWriteResult{
        .error = MakeError(
            CsvRecordErrorKind::IoFailure,
            0,
            0,
            std::move(atomic_error)),
    };
}

}  // namespace spectiary
