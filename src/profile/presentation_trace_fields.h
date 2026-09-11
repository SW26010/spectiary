#pragma once
#include "profile/profile_sink.h"
#include "profile/presentation_trace.h"
#ifdef _WIN32
#include <Windows.h>
#endif

namespace specforge {
inline void WritePresentationTrace(ProfileSink& sink,
    const presentation_trace::Event& event, std::string_view role,
    std::uint64_t viewport_id)
{
    using F = ProfileSink::Field;
#ifdef _WIN32
    const bool sample_style = event.name == "platform_window_size" ||
        ((event.name == "viewport_lifecycle" || event.name == "viewport_capture_boundary") && event.phase == "begin");
    const auto native_hwnd = sample_style ? reinterpret_cast<HWND>(event.window.hwnd) : nullptr;
    const auto actual_ex_style = native_hwnd ? static_cast<DWORD>(GetWindowLongPtrW(native_hwnd, GWL_EXSTYLE)) : 0;
#else
    const unsigned long actual_ex_style = 0;
#endif
#ifdef _WIN32
    if ((event.name == "viewport_lifecycle" || event.name == "viewport_capture_boundary") && event.phase == "begin" && sink.is_frame_recording_active()) {
        const DWORD saved_error = GetLastError();
        LARGE_INTEGER frequency{}, before{}, after{};
        const bool frequency_valid = QueryPerformanceFrequency(&frequency) != FALSE;
        const bool before_valid = QueryPerformanceCounter(&before) != FALSE;
        const auto steady = std::chrono::duration_cast<std::chrono::nanoseconds>(
            presentation_trace::Clock::now().time_since_epoch()).count();
        const bool after_valid = QueryPerformanceCounter(&after) != FALSE;
        sink.WriteEvent("presentation_clock_sync", {
            F::Number("schema_version", "1"),
            F::Number("process_id", std::to_string(GetCurrentProcessId())),
            F::Number("thread_id", std::to_string(GetCurrentThreadId())),
            F::Number("steady_sample_ns", std::to_string(steady)),
            F::Number("qpc_before", std::to_string(before.QuadPart)),
            F::Number("qpc_after", std::to_string(after.QuadPart)),
            F::Number("qpc_frequency", std::to_string(frequency.QuadPart)),
            F::Bool("valid", frequency_valid && before_valid && after_valid &&
                frequency.QuadPart > 0 && after.QuadPart >= before.QuadPart),
        });
        SetLastError(saved_error);
    }
#endif
    sink.WriteEvent(event.name, {
        F::Number("schema_version", "1"),
        F::String("phase", std::string(event.phase)),
        F::String("viewport_role", std::string(role)),
        F::Number("viewport_id", std::to_string(viewport_id)),
        F::Number("hwnd", std::to_string(event.window.hwnd)),
        F::Number("viewport_lifetime", std::to_string(event.window.lifetime)),
        F::Number("size_move_id", std::to_string(event.window.size_move)),
        F::Bool("in_size_move", event.window.in_size_move),
        F::Number("operation_id", std::to_string(event.operation)),
        F::Number("parent_operation_id", std::to_string(event.parent)),
        F::Number("frame", std::to_string(event.frame)),
        F::Number("old_width", std::to_string(event.window.width)),
        F::Number("old_height", std::to_string(event.window.height)),
        F::Number("new_width", std::to_string(event.new_width)),
        F::Number("new_height", std::to_string(event.new_height)),
        F::String("backend", std::string(event.backend)),
        F::Number("result", std::to_string(event.result)),
        F::Bool("result_valid", event.result_valid),
        F::Bool("present_completed", event.name == "viewport_present" &&
            event.phase == "end" && event.result_valid && event.result == 0),
        F::Number("count", std::to_string(event.count)),
        F::Number("timeout_ms", std::to_string(event.timeout_ms)),
        F::String("present_mode", std::string(event.present_mode)),
        F::Number("duration_ms", std::to_string(event.duration_ms)),
        F::Number("message_id", std::to_string(event.message_id)),
        F::Number("message_hwnd", std::to_string(event.message_hwnd)),
        F::Number("hwnd_ex_style", std::to_string(actual_ex_style)),
        F::Number("buffer_slot", std::to_string(event.buffer_slot)),
        F::String("resource_kind", std::string(event.resource_kind)),
    });
}
} // namespace specforge
