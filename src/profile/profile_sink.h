#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>

namespace specforge {

class ProfileSink {
public:
    struct Field {
        std::string_view name;
        std::string value;
        bool quoted = true;

        static Field String(std::string_view name, std::string value);
        static Field Number(std::string_view name, std::string value);
        static Field Bool(std::string_view name, bool value);
    };

    ProfileSink() = default;
    explicit ProfileSink(std::filesystem::path path);

    ProfileSink(ProfileSink&&) noexcept = default;
    ProfileSink& operator=(ProfileSink&&) noexcept = default;

    ProfileSink(const ProfileSink&) = delete;
    ProfileSink& operator=(const ProfileSink&) = delete;

    static ProfileSink CreateDefault();

    [[nodiscard]] bool is_open() const noexcept { return stream_.is_open(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    void WriteEvent(std::string_view event_name, std::initializer_list<Field> fields = {});
    void WriteDuration(std::string_view event_name, std::uint64_t frame_index, double milliseconds);

private:
    static std::int64_t SteadyNanoseconds();
    static std::string EscapeJson(std::string_view value);
    static std::string TimestampForFileName();

    std::filesystem::path path_;
    std::ofstream stream_;
};

class ProfileTimer {
public:
    ProfileTimer(ProfileSink& sink, std::string_view event_name, std::uint64_t frame_index);
    ~ProfileTimer();

    ProfileTimer(const ProfileTimer&) = delete;
    ProfileTimer& operator=(const ProfileTimer&) = delete;

private:
    ProfileSink& sink_;
    std::string event_name_;
    std::uint64_t frame_index_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace specforge
