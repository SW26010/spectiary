#pragma once

#include <Windows.h>

#include <string>

namespace specforge {

inline constexpr UINT32 kDisplayConfigQueryFlags =
    QDC_ONLY_ACTIVE_PATHS |
    QDC_VIRTUAL_MODE_AWARE |
    QDC_VIRTUAL_REFRESH_RATE_AWARE;

enum class DisplayRefreshDurationBasis {
    None,
    DisplayConfigPhysicalRefresh,
    CurrentDisplayMode,
};

struct Win32DisplayRefreshState {
    HMONITOR monitor = nullptr;
    bool monitor_found = false;
    std::wstring monitor_device;

    bool current_mode_found = false;
    UINT current_width = 0;
    UINT current_height = 0;
    UINT current_refresh_hz = 0;
    UINT max_available_refresh_hz = 0;

    LONG display_config_result = ERROR_NOT_FOUND;
    UINT32 display_config_query_flags = 0;
    bool virtual_refresh_rate_aware = false;
    bool path_found = false;
    UINT32 path_flags = 0;
    UINT32 virtual_refresh_numerator = 0;
    UINT32 virtual_refresh_denominator = 0;
    UINT32 physical_refresh_numerator = 0;
    UINT32 physical_refresh_denominator = 0;

    DisplayRefreshDurationBasis duration_basis = DisplayRefreshDurationBasis::None;
    UINT preferred_duration = 0;
    UINT preferred_tolerance = 0;

    [[nodiscard]] bool drr_configured() const noexcept;
    [[nodiscard]] double virtual_refresh_hz() const noexcept;
    [[nodiscard]] double physical_refresh_hz() const noexcept;
    [[nodiscard]] double requested_refresh_hz() const noexcept;
    [[nodiscard]] bool system_refresh_constrained() const noexcept;
};

[[nodiscard]] UINT PreferredPresentDuration(
    UINT32 refresh_numerator,
    UINT32 refresh_denominator) noexcept;
[[nodiscard]] UINT PreferredPresentTolerance(UINT duration) noexcept;
[[nodiscard]] const char* DisplayRefreshDurationBasisName(
    DisplayRefreshDurationBasis basis) noexcept;
[[nodiscard]] HRESULT QueryWin32DisplayRefreshState(
    HWND hwnd,
    Win32DisplayRefreshState& state) noexcept;

}  // namespace specforge
