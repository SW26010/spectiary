#pragma once

#include <chrono>
#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace specforge {

class CanonicalTimestamp final {
public:
    using TimePoint = std::chrono::sys_time<std::chrono::milliseconds>;

    [[nodiscard]] static std::optional<CanonicalTimestamp> FromTimePoint(
        TimePoint value) noexcept;

    [[nodiscard]] TimePoint time_point() const noexcept;

    [[nodiscard]] auto operator<=>(
        const CanonicalTimestamp&) const noexcept = default;

private:
    explicit CanonicalTimestamp(TimePoint value) noexcept;

    TimePoint value_;
};

[[nodiscard]] CanonicalTimestamp CurrentCanonicalTimestamp();
[[nodiscard]] std::string FormatCanonicalTimestamp(
    CanonicalTimestamp value);
[[nodiscard]] std::optional<CanonicalTimestamp> ParseCanonicalTimestamp(
    std::string_view value) noexcept;

}  // namespace specforge
