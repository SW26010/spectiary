#include "profile/presentation_trace_fields.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace trace = specforge::presentation_trace;
namespace {
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
std::vector<trace::Event> events;
bool recording = true;
void TestLifecycleAndFailure()
{
    trace::enabled = [](void*) { return recording; };
    trace::callback = [](void*, const trace::Event& event) { events.push_back(event); };
    trace::main_hwnd = 42;
    trace::Register(42, 320, 240);
    auto first = trace::Lookup(42);
    trace::SizeMove(42, true);
    trace::RenderFrame(7);
    const auto result = trace::Measure(trace::Event{
        .name = "viewport_resize", .window = trace::Lookup(42),
        .new_width = 640, .new_height = 480, .backend = "composition"}, [] {
        return trace::Measure("presentation_buffer_rebuild", [] { return -123; });
    });
    Require(result == -123, "tracing must preserve failure results");
    const auto& resize = events[3];
    const auto& rebuild = events[4];
    Require(resize.name == "viewport_resize" && resize.phase == "begin", "resize begin missing");
    Require(rebuild.parent == resize.operation && rebuild.window.lifetime == first.lifetime,
        "nested buffer work must retain viewport and parent identity");
    Require(resize.window.width == 320 && resize.new_width == 640 && resize.frame == 7 &&
        resize.window.in_size_move && resize.window.size_move != 0, "resize context missing");
    Require(events[5].result == -123 && events[6].result == -123 && events[6].duration_ms >= 0,
        "failure must complete both operations");
    Require(trace::current.operation == 0, "nested context must unwind");
    Require(trace::Measure("presentation_available_wait", [] { return 258UL; }) == 258UL &&
        events.back().result == 258 && events.back().result_valid,
        "timeout status must be returned and recorded without synthesizing success");
    trace::SizeMove(42, false);
    Require(!trace::Lookup(42).in_size_move && events.back().window.size_move == resize.window.size_move,
        "size-move exit must match entry");
    trace::main_hwnd = 0; // Native window has already been destroyed at application shutdown.
    trace::Unregister(42);
    Require(std::string_view(trace::Role(events.back().window)) == "main",
        "main role must survive native HWND teardown");
    trace::Register(42, 1, 1);
    Require(std::string_view(trace::Role(trace::Lookup(42))) == "detached",
        "a reused handle must get the new lifetime's role");
    Require(trace::Lookup(42).lifetime != first.lifetime, "reused HWND requires a new lifetime");
    trace::Unregister(42);
    Require(trace::windows.empty(), "window state must remain bounded by live windows");
}
void TestDisabledAndInvalidation()
{
    events.clear();
    recording = false;
    int calls = 0;
    const auto before = trace::sequence;
    Require(trace::Measure("viewport_present", [&] { ++calls; return 17; }) == 17 && calls == 1,
        "disabled trace must call operation exactly once");
    Require(events.empty() && before == trace::sequence, "disabled trace must not allocate operations");
    trace::Invalidate();
    recording = true;
    trace::Invalidate();
    trace::Invalidate();
    trace::RenderFrame(8);
    Require(events.size() == 2 && events[0].operation == events[1].operation &&
        events[1].count == 2 && events[1].duration_ms >= 0 && events[1].frame == 8,
        "coalesced invalidations must correlate to the next render frame");
    trace::RenderFrame(9);
    Require(events.back().count == 0 && events.back().operation == 0,
        "scheduler follow-up frames must not invent invalidations");
    trace::callback = [](void*, const trace::Event&) { throw std::runtime_error("sink"); };
    Require(trace::Measure("viewport_present", [] { return -7; }) == -7,
        "sink failures must not alter presentation");
}
void TestProductionSerialization()
{
    const auto path = std::filesystem::temp_directory_path() /
        ("specforge-presentation-trace-" + std::to_string(trace::Clock::now().time_since_epoch().count()) + ".jsonl");
    {
        specforge::ProfileSink sink(path);
        Require(sink.is_open(), "test recorder must open");
#ifdef _WIN32
        specforge::WritePresentationTrace(sink, trace::Event{
            .name = "viewport_lifecycle", .phase = "begin", .window = {42, 3}}, "detached", 22);
#endif
        specforge::WritePresentationTrace(sink, trace::Event{
            .name = "viewport_present", .phase = "end", .window = {42, 3},
            .operation = 5, .frame = 8, .backend = "dxgi", .result = -1}, "detached", 22);
        sink.Stop();
    }
    std::ifstream input(path);
    std::string content((std::istreambuf_iterator<char>(input)), {});
    input.close();
    std::filesystem::remove(path);
#ifdef _WIN32
    for (const auto field : {"\"event\":\"presentation_clock_sync\"", "\"qpc_before\":",
        "\"qpc_after\":", "\"qpc_frequency\":", "\"steady_sample_ns\":", "\"valid\":true"}) {
        Require(content.find(field) != std::string::npos, "native clock correlation field missing");
    }
#endif
    for (const auto field : {"\"schema_version\":1", "\"viewport_role\":\"detached\"",
        "\"viewport_id\":22", "\"viewport_lifetime\":3", "\"operation_id\":5",
        "\"frame\":8", "\"result\":-1", "\"duration_ms\":", "\"phase\":\"end\""}) {
        Require(content.find(field) != std::string::npos, "production JSONL schema field missing");
    }
}
}
void TestCaptureBoundaryPreservesWindows()
{
    recording = true;
    trace::enabled = [](void*) { return recording; };
    trace::callback = [](void*, const trace::Event& event) { events.push_back(event); };
    trace::Register(123, 640, 480);
    trace::SizeMove(123, true);
    const auto original = trace::Lookup(123);
    events.clear();
    trace::CaptureBoundary(true);
    trace::CaptureBoundary(false);
    Require(events.size() == 2 && events[0].name == "viewport_capture_boundary" &&
        events[0].phase == "begin" && events[1].phase == "end" &&
        events[0].window.lifetime == original.lifetime && events[1].window.in_size_move &&
        events[1].window.size_move == original.size_move && events[1].operation == 0,
        "capture boundaries must snapshot existing identities and active size moves");
    Require(trace::Lookup(123).lifetime == original.lifetime && trace::Lookup(123).in_size_move,
        "ending recording must not destroy or end the native window lifecycle");
    trace::SizeMove(123, false);
    trace::Unregister(123);
}
int main()
{
    try {
        events.reserve(64);
        TestLifecycleAndFailure();
        TestDisabledAndInvalidation();
        TestCaptureBoundaryPreservesWindows();
        TestProductionSerialization();
        trace::callback = nullptr;
        std::cout << "presentation trace tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
