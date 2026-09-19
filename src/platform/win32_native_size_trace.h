#pragma once

#include "profile/presentation_trace.h"

#include <Windows.h>
#include <array>
#include <optional>

namespace spectiary::native_size_trace {

// Hooks observe the existing UI-thread call path; they never dispatch messages.
inline thread_local bool hook_available = false;
inline thread_local unsigned scopes = 0, depth = 0, suppressed_depth = 0;
inline thread_local std::uint64_t errors = 0;
struct Message {
    HWND hwnd = nullptr;
    UINT id = 0;
    std::optional<presentation_trace::Span> span;
};
inline thread_local std::array<Message, 32> messages;

inline void Enter(HWND hwnd, UINT id) noexcept
{
    if (!scopes || !hook_available) return;
    if (suppressed_depth || depth == messages.size()) {
        ++suppressed_depth;
        ++errors;
        return;
    }
    auto& message = messages[depth++];
    message.hwnd = hwnd;
    message.id = id;
    auto event = presentation_trace::current;
    event.name = "native_size_message";
    event.message_id = id;
    event.message_hwnd = reinterpret_cast<std::uintptr_t>(hwnd);
    message.span.emplace(event);
}

inline void Leave(HWND hwnd, UINT id) noexcept
{
    if (!scopes || !hook_available) return;
    if (suppressed_depth) { --suppressed_depth; return; }
    if (!depth || messages[depth - 1].hwnd != hwnd || messages[depth - 1].id != id) {
        ++errors;
        return;
    }
    messages[--depth].span.reset();
}

inline bool ThreadCpu(std::uint64_t& ticks) noexcept
{
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return false;
    const auto value = [](FILETIME time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    ticks = value(kernel) + value(user);
    return true;
}

class Scope {
public:
    Scope() noexcept : active_(presentation_trace::Active())
    {
        if (!active_) return;
        const DWORD saved_error = GetLastError();
        start_depth_ = depth;
        start_errors_ = errors;
        cpu_valid_ = ThreadCpu(cpu_start_);
        ++scopes;
        auto event = presentation_trace::current;
        event.name = "native_size_callback";
        span_.emplace(event);
        SetLastError(saved_error);
    }
    ~Scope()
    {
        if (!active_) return;
        const DWORD saved_error = GetLastError();
        std::uint64_t cpu_end = 0;
        const bool valid = ThreadCpu(cpu_end) && cpu_valid_ && cpu_end >= cpu_start_;
        if (depth != start_depth_ || suppressed_depth) ++errors;
        while (depth > start_depth_) messages[--depth].span.reset();
        auto event = presentation_trace::current;
        event.name = "native_size_thread_cpu";
        event.phase = "end";
        event.parent = event.operation;
        event.operation = 0;
        event.result_valid = valid;
        event.result = valid ? 0 : -1;
        event.duration_ms = valid ? static_cast<double>(cpu_end - cpu_start_) / 10000.0 : 0;
        presentation_trace::Emit(event);
        event.name = "native_size_observation";
        event.duration_ms = 0;
        event.result_valid = true;
        event.result = hook_available ? 0 : -1;
        event.count = errors - start_errors_;
        presentation_trace::Emit(event);
        span_.reset();
        if (--scopes == 0) { suppressed_depth = 0; errors = 0; }
        SetLastError(saved_error);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    bool active_, cpu_valid_ = false;
    unsigned start_depth_ = 0;
    std::uint64_t start_errors_ = 0, cpu_start_ = 0;
    std::optional<presentation_trace::Span> span_;
};
} // namespace spectiary::native_size_trace
