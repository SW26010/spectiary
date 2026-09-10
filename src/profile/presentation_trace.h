#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace specforge::presentation_trace {

// UI-thread diagnostics only. No scheduling, waits, or presentation decisions.
using Clock = std::chrono::steady_clock;
struct Window {
    std::uintptr_t hwnd = 0;
    std::uint64_t lifetime = 0;
    std::uint64_t size_move = 0;
    bool in_size_move = false;
    unsigned width = 0, height = 0;
    bool main = false;
};
struct Event {
    std::string_view name, phase;
    Window window;
    std::uint64_t operation = 0, parent = 0, frame = 0;
    unsigned new_width = 0, new_height = 0;
    std::string_view backend = "none";
    std::int64_t result = 0;
    bool result_valid = false;
    std::uint64_t count = 0;
    std::uint64_t timeout_ms = 0;
    std::string_view present_mode = "none";
    double duration_ms = 0;
};
using Callback = void (*)(void*, const Event&);
using Enabled = bool (*)(void*);
inline thread_local Callback callback = nullptr;
inline thread_local Enabled enabled = nullptr;
inline thread_local void* context = nullptr;
inline thread_local std::unordered_map<std::uintptr_t, Window> windows;
inline thread_local std::uint64_t sequence = 0, frame = 0;
inline thread_local std::uintptr_t main_hwnd = 0;
inline thread_local Event current;
inline thread_local Clock::time_point invalidated_at;
inline thread_local std::uint64_t invalidation_count = 0;
inline thread_local std::uint64_t invalidation_id = 0;

inline bool Active() noexcept { return callback && enabled && enabled(context); }
inline void Emit(const Event& event) noexcept
{
    // Diagnostics must never change a renderer result or escape a Win32 hook.
    try { if (Active()) callback(context, event); } catch (...) {}
}
inline Window Lookup(std::uintptr_t hwnd) noexcept
{
    const auto it = windows.find(hwnd);
    return it == windows.end() ? Window{.hwnd = hwnd} : it->second;
}
inline const char* Role(const Window& window) noexcept
{
    return window.hwnd == 0 ? "application" : window.main ? "main" : "detached";
}
inline void Register(std::uintptr_t hwnd, unsigned width, unsigned height) noexcept
{
    try {
        Window window{hwnd, ++sequence, 0, false, width, height};
        window.main = hwnd != 0 && hwnd == main_hwnd;
        windows.insert_or_assign(hwnd, window);
        Emit(Event{.name = "viewport_lifecycle", .phase = "begin", .window = window});
    } catch (...) {}
}
inline void Unregister(std::uintptr_t hwnd) noexcept
{
    if (windows.contains(hwnd)) {
        Emit(Event{.name = "viewport_lifecycle", .phase = "end", .window = Lookup(hwnd)});
        windows.erase(hwnd);
    }
}
inline void SizeMove(std::uintptr_t hwnd, bool entering) noexcept
{
    const auto it = windows.find(hwnd);
    if (it == windows.end()) return;
    auto& window = it->second;
    if (entering) { window.size_move = ++sequence; window.in_size_move = true; }
    Emit(Event{.name = "viewport_size_move", .phase = entering ? "begin" : "end",
               .window = window, .frame = frame});
    if (!entering) window.in_size_move = false;
}

class Span {
public:
    explicit Span(Event event) noexcept : active_(Active())
    {
        if (!active_) return;
        previous_ = current;
        event_ = event;
        event_.operation = ++sequence;
        event_.parent = current.operation;
        event_.frame = frame;
        event_.phase = "begin";
        current = event_;
        Emit(event_);
        start_ = Clock::now();
    }
    ~Span()
    {
        if (!active_) return;
        event_.duration_ms = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        event_.phase = "end";
        current = previous_;
        Emit(event_);
    }
    void Result(std::int64_t result) noexcept { event_.result = result; event_.result_valid = true; }
    void Count(std::uint64_t count) noexcept { event_.count = count; }
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
private:
    bool active_;
    Event event_, previous_;
    Clock::time_point start_;
};
template<class F> auto Measure(Event event, F&& operation)
{
    Span span(event);
    auto result = std::forward<F>(operation)();
    span.Result(static_cast<std::int64_t>(result));
    return result;
}
template<class F> auto Measure(std::string_view name, F&& operation)
{
    Event event = current;
    event.name = name;
    return Measure(event, std::forward<F>(operation));
}
template<class F> auto Measure(std::string_view name, std::string_view backend, F&& operation)
{
    Event event = current;
    event.name = name;
    event.backend = backend;
    return Measure(event, std::forward<F>(operation));
}
inline void Invalidate() noexcept
{
    if (!Active()) { invalidation_count = 0; return; }
    if (invalidation_count++ == 0) {
        invalidated_at = Clock::now();
        invalidation_id = ++sequence;
        Emit(Event{.name = "render_invalidation_latency", .phase = "begin",
                   .operation = invalidation_id});
    }
}
inline void RenderFrame(std::uint64_t index) noexcept
{
    frame = index;
    Event event{.name = "render_invalidation_latency", .phase = "end",
                .operation = invalidation_count ? invalidation_id : 0, .frame = frame};
    event.count = invalidation_count;
    if (invalidation_count) event.duration_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - invalidated_at).count();
    Emit(event);
    invalidation_count = 0;
}
inline void CancelInvalidation() noexcept
{
    if (invalidation_count) {
        Emit(Event{.name = "render_invalidation_latency", .phase = "cancel",
            .operation = invalidation_id, .count = invalidation_count});
        invalidation_count = 0;
    }
}
} // namespace specforge::presentation_trace
