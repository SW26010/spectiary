#pragma once
#include "profile/profile_sink.h"
#include "profile/presentation_trace.h"

namespace specforge {
inline void WritePresentationTrace(ProfileSink& sink,
    const presentation_trace::Event& event, std::string_view role,
    std::uint64_t viewport_id)
{
    using F = ProfileSink::Field;
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
    });
}
} // namespace specforge
