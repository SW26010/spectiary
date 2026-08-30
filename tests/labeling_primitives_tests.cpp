#include "domain/canonical_timestamp.h"
#include "domain/utf8.h"
#include "domain/uuid_v4.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string_view Utf8Bytes(std::u8string_view text)
{
    return std::string_view(
        reinterpret_cast<const char*>(text.data()), text.size());
}

void TestUuidV4GenerationAndValidation()
{
    std::unordered_set<std::string> generated;
    for (int index = 0; index < 64; ++index) {
        const auto uuid = specforge::GenerateUuidV4();
        Require(uuid.has_value(), "UUID v4 generation should succeed");
        Require(uuid->size() == 36U, "UUID v4 should have 36 characters");
        Require(specforge::IsCanonicalUuidV4(*uuid),
            "generated UUID should use canonical v4 text");
        Require((*uuid)[14] == '4', "generated UUID should set version 4");
        Require((*uuid)[19] == '8' || (*uuid)[19] == '9' ||
                (*uuid)[19] == 'a' || (*uuid)[19] == 'b',
            "generated UUID should set the RFC variant bits");
        generated.insert(*uuid);
    }
    Require(generated.size() == 64U,
        "independently generated UUIDs should be distinct");

    Require(specforge::IsCanonicalUuidV4(
                "00000000-0000-4000-8000-000000000000"),
        "canonical UUID v4 with variant 8 should validate");
    Require(specforge::IsCanonicalUuidV4(
                "ffffffff-ffff-4fff-bfff-ffffffffffff"),
        "canonical UUID v4 with variant b should validate");
}

void TestUuidV4RejectsNonCanonicalText()
{
    const std::vector<std::string_view> rejected = {
        "FFFFFFFF-FFFF-4FFF-BFFF-FFFFFFFFFFFF",
        "{ffffffff-ffff-4fff-bfff-ffffffffffff}",
        "urn:uuid:ffffffff-ffff-4fff-bfff-ffffffffffff",
        "ffffffffffff4fffbfffffffffffffff",
        "ffffffff-ffff-1fff-bfff-ffffffffffff",
        "ffffffff-ffff-5fff-bfff-ffffffffffff",
        "ffffffff-ffff-4fff-7fff-ffffffffffff",
        "ffffffff-ffff-4fff-cfff-ffffffffffff",
        "ffffffff-ffff-4fff-gfff-ffffffffffff",
        "ffffffff_ffff_4fff_bfff_ffffffffffff",
        "ffffffff-ffff-4fff-bfff-fffffffffff",
    };
    for (const std::string_view value : rejected) {
        Require(!specforge::IsCanonicalUuidV4(value),
            "noncanonical UUID v4 text should be rejected");
    }
}

void TestCanonicalTimestampRoundTrip()
{
    constexpr std::string_view leap = "2024-02-29T23:59:59.009Z";
    const auto parsed = specforge::ParseCanonicalTimestamp(leap);
    Require(parsed.has_value(), "real leap-day timestamp should parse");
    Require(specforge::FormatCanonicalTimestamp(*parsed) == leap,
        "canonical timestamp should round-trip byte-for-byte");

    const auto epoch = specforge::CanonicalTimestamp::FromTimePoint(
        specforge::CanonicalTimestamp::TimePoint{
            std::chrono::milliseconds{0}});
    Require(epoch.has_value(), "Unix epoch should be representable");
    Require(specforge::FormatCanonicalTimestamp(*epoch) ==
            "1970-01-01T00:00:00.000Z",
        "time-point formatting should use fixed UTC milliseconds");
    constexpr std::string_view pre_epoch = "1969-12-31T23:59:59.999Z";
    const auto parsed_pre_epoch =
        specforge::ParseCanonicalTimestamp(pre_epoch);
    Require(parsed_pre_epoch.has_value() &&
            specforge::FormatCanonicalTimestamp(*parsed_pre_epoch) ==
                pre_epoch,
        "pre-epoch timestamps should use floor-based UTC calendar fields");

    const auto earlier = specforge::ParseCanonicalTimestamp(
        "2024-02-29T23:59:59.008Z");
    Require(earlier.has_value() && *earlier < *parsed,
        "canonical timestamps should retain strong chronological ordering");

    const auto current = specforge::CurrentCanonicalTimestamp();
    const std::string current_text =
        specforge::FormatCanonicalTimestamp(current);
    Require(current_text.size() == 24U &&
            specforge::ParseCanonicalTimestamp(current_text) == current,
        "current timestamp should be millisecond-precise and canonical");
}

void TestCanonicalTimestampRejectsInvalidText()
{
    Require(specforge::ParseCanonicalTimestamp(
                "2000-02-29T00:00:00.000Z")
                .has_value(),
        "year divisible by 400 should be a leap year");

    const std::vector<std::string_view> rejected = {
        "1900-02-29T00:00:00.000Z",
        "2023-02-29T00:00:00.000Z",
        "2024-00-01T00:00:00.000Z",
        "2024-13-01T00:00:00.000Z",
        "2024-04-31T00:00:00.000Z",
        "2024-01-00T00:00:00.000Z",
        "2024-01-01T24:00:00.000Z",
        "2024-01-01T00:60:00.000Z",
        "2024-01-01T00:00:60.000Z",
        "2024-01-01T00:00:00Z",
        "2024-01-01T00:00:00.00Z",
        "2024-01-01T00:00:00.0000Z",
        "2024-01-01T00:00:00.000z",
        "2024-01-01T00:00:00.000+00:00",
        "2024-01-01 00:00:00.000Z",
        "2024-01-01T00:00:00.xyzZ",
    };
    for (const std::string_view value : rejected) {
        Require(!specforge::ParseCanonicalTimestamp(value).has_value(),
            "invalid or noncanonical timestamp should be rejected");
    }
}

void TestCanonicalTimestampTimePointBounds()
{
    using Timestamp = specforge::CanonicalTimestamp;
    constexpr Timestamp::TimePoint first_supported{
        std::chrono::sys_days{
            std::chrono::year{0} / std::chrono::January / 1}};
    constexpr Timestamp::TimePoint first_unsupported{
        std::chrono::sys_days{
            std::chrono::year{10000} / std::chrono::January / 1}};

    const auto lower_bound = Timestamp::FromTimePoint(first_supported);
    Require(lower_bound.has_value() &&
            specforge::FormatCanonicalTimestamp(*lower_bound) ==
                "0000-01-01T00:00:00.000Z",
        "canonical timestamp should accept its exact lower bound");
    Require(!Timestamp::FromTimePoint(
                 first_supported - std::chrono::milliseconds{1})
                 .has_value(),
        "canonical timestamp should reject values below year 0000");

    const auto upper_bound = Timestamp::FromTimePoint(
        first_unsupported - std::chrono::milliseconds{1});
    Require(upper_bound.has_value() &&
            specforge::FormatCanonicalTimestamp(*upper_bound) ==
                "9999-12-31T23:59:59.999Z",
        "canonical timestamp should accept its last representable millisecond");
    Require(!Timestamp::FromTimePoint(first_unsupported).has_value(),
        "canonical timestamp should reject year 10000");

    const Timestamp::TimePoint wrapped_year{
        std::chrono::milliseconds{2'005'949'145'600'000LL}};
    Require(!Timestamp::FromTimePoint(wrapped_year).has_value(),
        "year 65536 must be rejected before chrono year can narrow it");
}

void TestUtf8Validation()
{
    Require(specforge::IsValidUtf8("plain ASCII"),
        "ASCII should be valid UTF-8");
    Require(specforge::IsValidUtf8(Utf8Bytes(u8"中文 日本語 한국어")),
        "CJK text should be valid UTF-8");
    Require(specforge::IsValidUtf8(Utf8Bytes(u8"label 🧪")),
        "emoji should be valid UTF-8");

    const std::vector<std::string> invalid = {
        std::string("\x80", 1),
        std::string("\xc0\xaf", 2),
        std::string("\xe2\x82", 2),
        std::string("\xe2\x28\xa1", 3),
        std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4),
        std::string("\xf8\x88\x80\x80\x80", 5),
    };
    for (const std::string& value : invalid) {
        Require(!specforge::IsValidUtf8(value),
            "malformed UTF-8 should be rejected");
        Require(!specforge::IsValidUtf8WithNonWhitespace(value),
            "malformed UTF-8 should not pass task-name validation");
    }
}

void TestUtf8NonWhitespaceValidation()
{
    Require(!specforge::IsValidUtf8WithNonWhitespace(""),
        "empty task name should be rejected");
    Require(!specforge::IsValidUtf8WithNonWhitespace(
                Utf8Bytes(
                    u8" \t\r\n\u0085\u00a0\u1680\u2000\u200a"
                    u8"\u2028\u2029\u202f\u205f\u3000")),
        "task name containing only Unicode whitespace should be rejected");
    Require(specforge::IsValidUtf8WithNonWhitespace(
                Utf8Bytes(u8" 低信噪比复核 ")),
        "normal CJK task name should be accepted");
    Require(specforge::IsValidUtf8WithNonWhitespace(
                Utf8Bytes(u8" 🔭 ")),
        "emoji task name should be accepted");
    Require(specforge::IsValidUtf8WithNonWhitespace(
                Utf8Bytes(u8"\u200b")),
        "a non-whitespace Unicode format scalar should not be reclassified");
}

}  // namespace

int main()
{
    try {
        TestUuidV4GenerationAndValidation();
        TestUuidV4RejectsNonCanonicalText();
        TestCanonicalTimestampRoundTrip();
        TestCanonicalTimestampRejectsInvalidText();
        TestCanonicalTimestampTimePointBounds();
        TestUtf8Validation();
        TestUtf8NonWhitespaceValidation();
    } catch (const std::exception& error) {
        std::cerr << "labeling primitives test failed: " << error.what()
                  << '\n';
        return 1;
    }
    return 0;
}
