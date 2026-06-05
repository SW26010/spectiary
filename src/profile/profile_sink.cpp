#include "profile/profile_sink.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

namespace specforge {
namespace {

bool IsProfileEnabled()
{
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, "SPECFORGE_PROFILE") != 0 || raw_value == nullptr) {
        return false;
    }

    std::unique_ptr<char, decltype(&std::free)> value(raw_value, std::free);
    std::string normalized(value.get());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes";
}

}  // namespace

ProfileSink::Field ProfileSink::Field::String(std::string_view name, std::string value)
{
    return Field{name, std::move(value), true};
}

ProfileSink::Field ProfileSink::Field::Number(std::string_view name, std::string value)
{
    return Field{name, std::move(value), false};
}

ProfileSink::Field ProfileSink::Field::Bool(std::string_view name, bool value)
{
    return Field{name, value ? "true" : "false", false};
}

ProfileSink::ProfileSink(std::filesystem::path path) : path_(std::move(path)), stream_(path_)
{
}

ProfileSink ProfileSink::CreateDefault()
{
    if (!IsProfileEnabled()) {
        return ProfileSink();
    }

    const std::filesystem::path directory = std::filesystem::current_path() / "logs";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return ProfileSink(directory / ("specforge-profile-" + TimestampForFileName() + ".jsonl"));
}

void ProfileSink::WriteEvent(std::string_view event_name, std::initializer_list<Field> fields)
{
    if (!stream_.is_open()) {
        return;
    }

    stream_ << "{\"steady_ns\":" << SteadyNanoseconds() << ",\"event\":\"" << EscapeJson(event_name) << "\"";
    for (const Field& field : fields) {
        stream_ << ",\"" << EscapeJson(field.name) << "\":";
        if (field.quoted) {
            stream_ << "\"" << EscapeJson(field.value) << "\"";
        } else {
            stream_ << field.value;
        }
    }
    stream_ << "}\n";
}

void ProfileSink::WriteDuration(std::string_view event_name, std::uint64_t frame_index, double milliseconds)
{
    if (!stream_.is_open()) {
        return;
    }

    std::ostringstream value;
    value << std::fixed << std::setprecision(4) << milliseconds;

    WriteEvent(event_name, {
                               Field::Number("frame", std::to_string(frame_index)),
                               Field::Number("duration_ms", value.str()),
                           });
}

std::int64_t ProfileSink::SteadyNanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string ProfileSink::EscapeJson(std::string_view value)
{
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            escaped << "\\\"";
            break;
        case '\\':
            escaped << "\\\\";
            break;
        case '\b':
            escaped << "\\b";
            break;
        case '\f':
            escaped << "\\f";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            if (character < 0x20) {
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(character)
                        << std::dec;
            } else {
                escaped << character;
            }
            break;
        }
    }
    return escaped.str();
}

std::string ProfileSink::TimestampForFileName()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);

    std::tm local_time = {};
    localtime_s(&local_time, &time);

    std::ostringstream timestamp;
    timestamp << std::put_time(&local_time, "%Y%m%d-%H%M%S");
    return timestamp.str();
}

ProfileTimer::ProfileTimer(ProfileSink& sink, std::string_view event_name, std::uint64_t frame_index)
    : sink_(sink), event_name_(event_name), frame_index_(frame_index), start_(std::chrono::steady_clock::now())
{
}

ProfileTimer::~ProfileTimer()
{
    const auto elapsed = std::chrono::steady_clock::now() - start_;
    const double milliseconds = std::chrono::duration<double, std::milli>(elapsed).count();
    sink_.WriteDuration(event_name_, frame_index_, milliseconds);
}

}  // namespace specforge
