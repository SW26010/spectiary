#include "domain/canonical_timestamp.h"

#include <cstddef>
#include <stdexcept>

namespace spectiary {
namespace {

[[nodiscard]] bool IsWithinCanonicalTimestampRange(
    CanonicalTimestamp::TimePoint value) noexcept
{
    constexpr std::chrono::sys_days first_supported_day{
        std::chrono::year{0} / std::chrono::January / 1};
    constexpr std::chrono::sys_days first_unsupported_day{
        std::chrono::year{10000} / std::chrono::January / 1};
    return value >= first_supported_day && value < first_unsupported_day;
}

[[nodiscard]] bool ParseDigits(
    std::string_view value,
    std::size_t offset,
    std::size_t count,
    unsigned& result) noexcept
{
    result = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char digit = value[offset + index];
        if (digit < '0' || digit > '9') {
            return false;
        }
        result = result * 10U + static_cast<unsigned>(digit - '0');
    }
    return true;
}

void WriteTwoDigits(std::string& output, std::size_t offset, unsigned value)
{
    output[offset] = static_cast<char>('0' + value / 10U);
    output[offset + 1U] = static_cast<char>('0' + value % 10U);
}

void WriteThreeDigits(std::string& output, std::size_t offset, unsigned value)
{
    output[offset] = static_cast<char>('0' + value / 100U);
    output[offset + 1U] = static_cast<char>('0' + (value / 10U) % 10U);
    output[offset + 2U] = static_cast<char>('0' + value % 10U);
}

void WriteFourDigits(std::string& output, std::size_t offset, unsigned value)
{
    output[offset] = static_cast<char>('0' + value / 1000U);
    output[offset + 1U] =
        static_cast<char>('0' + (value / 100U) % 10U);
    output[offset + 2U] =
        static_cast<char>('0' + (value / 10U) % 10U);
    output[offset + 3U] = static_cast<char>('0' + value % 10U);
}

}  // namespace

CanonicalTimestamp::CanonicalTimestamp(TimePoint value) noexcept
    : value_(value)
{
}

std::optional<CanonicalTimestamp> CanonicalTimestamp::FromTimePoint(
    TimePoint value) noexcept
{
    if (!IsWithinCanonicalTimestampRange(value)) {
        return std::nullopt;
    }
    return CanonicalTimestamp(value);
}

CanonicalTimestamp::TimePoint CanonicalTimestamp::time_point() const noexcept
{
    return value_;
}

CanonicalTimestamp CurrentCanonicalTimestamp()
{
    const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    const auto timestamp = CanonicalTimestamp::FromTimePoint(milliseconds);
    if (!timestamp.has_value()) {
        throw std::out_of_range(
            "current system time is outside the canonical timestamp range");
    }
    return *timestamp;
}

std::string FormatCanonicalTimestamp(CanonicalTimestamp value)
{
    const auto day = std::chrono::floor<std::chrono::days>(value.time_point());
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{value.time_point() - day};

    std::string output = "0000-00-00T00:00:00.000Z";
    WriteFourDigits(
        output, 0, static_cast<unsigned>(static_cast<int>(date.year())));
    WriteTwoDigits(output, 5, static_cast<unsigned>(date.month()));
    WriteTwoDigits(output, 8, static_cast<unsigned>(date.day()));
    WriteTwoDigits(output, 11, static_cast<unsigned>(time.hours().count()));
    WriteTwoDigits(output, 14, static_cast<unsigned>(time.minutes().count()));
    WriteTwoDigits(output, 17, static_cast<unsigned>(time.seconds().count()));
    WriteThreeDigits(
        output, 20, static_cast<unsigned>(time.subseconds().count()));
    return output;
}

std::optional<CanonicalTimestamp> ParseCanonicalTimestamp(
    std::string_view value) noexcept
{
    if (value.size() != 24U || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != '.' || value[23] != 'Z') {
        return std::nullopt;
    }

    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    unsigned millisecond = 0;
    if (!ParseDigits(value, 0, 4, year) ||
        !ParseDigits(value, 5, 2, month) ||
        !ParseDigits(value, 8, 2, day) ||
        !ParseDigits(value, 11, 2, hour) ||
        !ParseDigits(value, 14, 2, minute) ||
        !ParseDigits(value, 17, 2, second) ||
        !ParseDigits(value, 20, 3, millisecond) || hour > 23U ||
        minute > 59U || second > 59U) {
        return std::nullopt;
    }

    const std::chrono::year_month_day date{
        std::chrono::year(static_cast<int>(year)),
        std::chrono::month(month),
        std::chrono::day(day)};
    if (!date.ok()) {
        return std::nullopt;
    }

    const auto time_point =
        std::chrono::sys_days(date) + std::chrono::hours(hour) +
        std::chrono::minutes(minute) + std::chrono::seconds(second) +
        std::chrono::milliseconds(millisecond);
    return CanonicalTimestamp::FromTimePoint(time_point);
}

}  // namespace spectiary
