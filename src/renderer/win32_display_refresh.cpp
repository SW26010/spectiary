#include "renderer/win32_display_refresh.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace specforge {
namespace {

constexpr double kInterruptTimeUnitsPerSecond = 10'000'000.0;

double RatioHz(UINT32 numerator, UINT32 denominator) noexcept
{
    return denominator != 0 ? static_cast<double>(numerator) / denominator : 0.0;
}

LONG QueryMatchingDisplayPath(
    const std::wstring& monitor_device,
    UINT32 query_flags,
    Win32DisplayRefreshState& state)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        LONG result = GetDisplayConfigBufferSizes(query_flags, &path_count, &mode_count);
        if (result != ERROR_SUCCESS) {
            return result;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        result = QueryDisplayConfig(
            query_flags,
            &path_count,
            paths.data(),
            &mode_count,
            modes.data(),
            nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (result != ERROR_SUCCESS) {
            return result;
        }

        paths.resize(path_count);
        modes.resize(mode_count);
        for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name = {};
            source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source_name.header.size = sizeof(source_name);
            source_name.header.adapterId = path.sourceInfo.adapterId;
            source_name.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS ||
                _wcsicmp(source_name.viewGdiDeviceName, monitor_device.c_str()) != 0) {
                continue;
            }

            state.path_found = true;
            state.path_flags = path.flags;
            state.virtual_refresh_numerator = path.targetInfo.refreshRate.Numerator;
            state.virtual_refresh_denominator = path.targetInfo.refreshRate.Denominator;

            const UINT32 target_mode_index =
                (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0
                    ? path.targetInfo.targetModeInfoIdx
                    : path.targetInfo.modeInfoIdx;
            if (target_mode_index != DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID &&
                target_mode_index != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
                target_mode_index < modes.size() &&
                modes[target_mode_index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                const DISPLAYCONFIG_RATIONAL physical_refresh =
                    modes[target_mode_index].targetMode.targetVideoSignalInfo.vSyncFreq;
                state.physical_refresh_numerator = physical_refresh.Numerator;
                state.physical_refresh_denominator = physical_refresh.Denominator;
            }
            return ERROR_SUCCESS;
        }
        return ERROR_NOT_FOUND;
    }
    return ERROR_INSUFFICIENT_BUFFER;
}

}  // namespace

bool Win32DisplayRefreshState::drr_configured() const noexcept
{
    return path_found &&
           (path_flags & DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE) != 0;
}

double Win32DisplayRefreshState::virtual_refresh_hz() const noexcept
{
    return RatioHz(virtual_refresh_numerator, virtual_refresh_denominator);
}

double Win32DisplayRefreshState::physical_refresh_hz() const noexcept
{
    return RatioHz(physical_refresh_numerator, physical_refresh_denominator);
}

double Win32DisplayRefreshState::requested_refresh_hz() const noexcept
{
    return preferred_duration != 0
               ? kInterruptTimeUnitsPerSecond / preferred_duration
               : 0.0;
}

bool Win32DisplayRefreshState::system_refresh_constrained() const noexcept
{
    constexpr double kRefreshComparisonToleranceHz = 0.5;
    return !drr_configured() && current_refresh_hz > 0 &&
           max_available_refresh_hz > current_refresh_hz &&
           static_cast<double>(max_available_refresh_hz - current_refresh_hz) >
               kRefreshComparisonToleranceHz;
}

UINT PreferredPresentDuration(
    UINT32 refresh_numerator,
    UINT32 refresh_denominator) noexcept
{
    if (refresh_numerator == 0 || refresh_denominator == 0) {
        return 0;
    }
    const double duration =
        kInterruptTimeUnitsPerSecond * refresh_denominator / refresh_numerator;
    if (!std::isfinite(duration) || duration < 1.0 || duration > MAXUINT) {
        return 0;
    }
    return static_cast<UINT>(std::llround(duration));
}

UINT PreferredPresentTolerance(UINT duration) noexcept
{
    if (duration == 0) {
        return 0;
    }
    return std::max(
        1'000U,
        static_cast<UINT>(std::llround(static_cast<double>(duration) * 0.01)));
}

const char* DisplayRefreshDurationBasisName(
    DisplayRefreshDurationBasis basis) noexcept
{
    switch (basis) {
    case DisplayRefreshDurationBasis::DisplayConfigPhysicalRefresh:
        return "display_config_physical_refresh";
    case DisplayRefreshDurationBasis::CurrentDisplayMode:
        return "current_display_mode";
    case DisplayRefreshDurationBasis::None:
    default:
        return "none";
    }
}

HRESULT QueryWin32DisplayRefreshState(
    HWND hwnd,
    Win32DisplayRefreshState& state) noexcept
{
    state = {};
    if (hwnd == nullptr) {
        return E_INVALIDARG;
    }

    try {
        state.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW monitor_info = {};
        monitor_info.cbSize = sizeof(monitor_info);
        state.monitor_found =
            state.monitor != nullptr && GetMonitorInfoW(state.monitor, &monitor_info);
        if (!state.monitor_found) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(
                error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE);
        }
        state.monitor_device = monitor_info.szDevice;

        DEVMODEW current_mode = {};
        current_mode.dmSize = sizeof(current_mode);
        state.current_mode_found = EnumDisplaySettingsW(
            state.monitor_device.c_str(),
            ENUM_CURRENT_SETTINGS,
            &current_mode);
        if (state.current_mode_found) {
            state.current_width = current_mode.dmPelsWidth;
            state.current_height = current_mode.dmPelsHeight;
            state.current_refresh_hz = current_mode.dmDisplayFrequency;

            for (DWORD mode_index = 0;; ++mode_index) {
                DEVMODEW available_mode = {};
                available_mode.dmSize = sizeof(available_mode);
                if (!EnumDisplaySettingsW(
                        state.monitor_device.c_str(),
                        mode_index,
                        &available_mode)) {
                    break;
                }
                if (available_mode.dmPelsWidth == state.current_width &&
                    available_mode.dmPelsHeight == state.current_height) {
                    state.max_available_refresh_hz = std::max(
                        state.max_available_refresh_hz,
                        static_cast<UINT>(available_mode.dmDisplayFrequency));
                }
            }
        }

        const HMONITOR monitor = state.monitor;
        const bool monitor_found = state.monitor_found;
        const std::wstring monitor_device = state.monitor_device;
        const bool current_mode_found = state.current_mode_found;
        const UINT current_width = state.current_width;
        const UINT current_height = state.current_height;
        const UINT current_refresh_hz = state.current_refresh_hz;
        const UINT max_available_refresh_hz = state.max_available_refresh_hz;
        state.display_config_query_flags = kDisplayConfigQueryFlags;
        state.display_config_result = QueryMatchingDisplayPath(
            state.monitor_device,
            state.display_config_query_flags,
            state);
        state.virtual_refresh_rate_aware =
            state.display_config_result == ERROR_SUCCESS;
        if (state.display_config_result == ERROR_INVALID_PARAMETER) {
            state = Win32DisplayRefreshState{
                .monitor = monitor,
                .monitor_found = monitor_found,
                .monitor_device = monitor_device,
                .current_mode_found = current_mode_found,
                .current_width = current_width,
                .current_height = current_height,
                .current_refresh_hz = current_refresh_hz,
                .max_available_refresh_hz = max_available_refresh_hz,
            };
            state.display_config_query_flags =
                QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
            state.display_config_result = QueryMatchingDisplayPath(
                state.monitor_device,
                state.display_config_query_flags,
                state);
        }

        state.preferred_duration = PreferredPresentDuration(
            state.physical_refresh_numerator,
            state.physical_refresh_denominator);
        state.duration_basis =
            DisplayRefreshDurationBasis::DisplayConfigPhysicalRefresh;
        if (state.preferred_duration == 0 && state.current_refresh_hz > 1) {
            state.preferred_duration = PreferredPresentDuration(
                state.current_refresh_hz,
                1);
            state.duration_basis = DisplayRefreshDurationBasis::CurrentDisplayMode;
        }
        state.preferred_tolerance =
            PreferredPresentTolerance(state.preferred_duration);

        if (state.preferred_duration == 0) {
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        return S_OK;
    } catch (const std::bad_alloc&) {
        state = {};
        return E_OUTOFMEMORY;
    } catch (...) {
        state = {};
        return E_FAIL;
    }
}

}  // namespace specforge
