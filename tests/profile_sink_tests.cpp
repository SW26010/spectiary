#include "profile/profile_sink.h"
#include "profile/profile_recording_status.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace specforge {

struct ProfileSinkTestAccess {
    static bool StartWithOutputStreamFactory(
        ProfileSink& sink,
        std::filesystem::path path,
        ProfileSink::Limits limits,
        ProfileSink::OutputStreamFactory output_stream_factory)
    {
        return sink.StartWithOutputStreamFactory(
            std::move(path),
            limits,
            std::move(output_stream_factory));
    }
};

}  // namespace specforge

namespace {

using namespace std::chrono_literals;

class FailingFlushBuffer : public std::streambuf {
protected:
    std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
    int_type overflow(int_type character) override { return traits_type::not_eof(character); }
    int sync() override { return -1; }
};

class FailingFlushStream : public std::ostream {
public:
    FailingFlushStream() : std::ostream(nullptr)
    {
        rdbuf(&buffer_);
        clear();
    }

private:
    FailingFlushBuffer buffer_;
};

struct BlockingFlushState {
    std::atomic<bool> entered = false;
    std::atomic<bool> release = false;
};

class BlockingFlushBuffer : public std::streambuf {
public:
    explicit BlockingFlushBuffer(std::shared_ptr<BlockingFlushState> state) : state_(std::move(state)) {}

protected:
    std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
    int_type overflow(int_type character) override { return traits_type::not_eof(character); }
    int sync() override
    {
        state_->entered.store(true, std::memory_order_release);
        while (!state_->release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        return 0;
    }

private:
    std::shared_ptr<BlockingFlushState> state_;
};

class BlockingFlushStream : public std::ostream {
public:
    explicit BlockingFlushStream(std::shared_ptr<BlockingFlushState> state)
        : std::ostream(nullptr), buffer_(std::move(state))
    {
        rdbuf(&buffer_);
        clear();
    }

private:
    BlockingFlushBuffer buffer_;
};

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("specforge_profile_sink_tests_" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "profile output should be readable");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

specforge::ProfileSink::Limits GenerousLimits()
{
    specforge::ProfileSink::Limits limits;
    limits.max_queue_bytes = 1024 * 1024;
    limits.max_file_bytes = 1024 * 1024;
    limits.max_duration = 1h;
    return limits;
}

void TestStopDrainsEventsAndWritesSummary()
{
    TemporaryDirectory temporary;
    const std::filesystem::path path = temporary.path() / "profile.jsonl";
    specforge::ProfileSink sink(path, GenerousLimits());
    Require(sink.is_open(), "an explicit writable path should start recording");

    sink.WriteEvent(
        "event\"name",
        {
            specforge::ProfileSink::Field::String("text", "line 1\nline 2"),
            specforge::ProfileSink::Field::Number("finite", "1.25"),
            specforge::ProfileSink::Field::Number("invalid", "nan"),
        });
    sink.Stop();

    Require(!sink.is_open(), "Stop should stop accepting profile events");
    Require(
        sink.stop_reason() == specforge::ProfileSink::StopReason::Explicit,
        "an ordinary Stop should retain an explicit stop reason");
    const std::string text = ReadTextFile(path);
    Require(text.find("\"event\":\"event\\\"name\"") != std::string::npos, "event names should be escaped");
    Require(text.find("\"text\":\"line 1\\nline 2\"") != std::string::npos, "string fields should be escaped");
    Require(text.find("\"finite\":1.25") != std::string::npos, "finite numeric fields should stay numeric");
    Require(text.find("\"invalid\":null") != std::string::npos, "invalid numeric fields should become null");
    Require(
        text.find("\"event\":\"profile_recorder_summary\"") != std::string::npos,
        "a drained recording should end with a recorder summary");
    Require(text.ends_with('\n'), "JSONL output should end at a complete record boundary");
}

void TestQueueLimitDropsInsteadOfGrowingWithoutBound()
{
    TemporaryDirectory temporary;
    specforge::ProfileSink::Limits limits = GenerousLimits();
    limits.max_queue_bytes = 1;
    specforge::ProfileSink sink(temporary.path() / "bounded.jsonl", limits);

    Require(!sink.WriteEvent("too_large_for_queue"), "an event outside the queue budget should report rejection");
    Require(sink.is_open(), "queue pressure should not stop the recording session");
    Require(sink.dropped_event_count() == 1, "an event larger than the queue budget should be dropped");
    sink.Stop();

    const std::string text = ReadTextFile(sink.path());
    Require(text.find("\"dropped_events\":1") != std::string::npos, "the summary should disclose dropped events");
}

void TestNormalCapacityDoesNotDropOnWriterContention()
{
    TemporaryDirectory temporary;
    specforge::ProfileSink::Limits limits;
    limits.max_duration = 1h;
    specforge::ProfileSink sink(temporary.path() / "normal-capacity.jsonl", limits);

    constexpr int kEventCount = 30000;
    for (int index = 0; index < kEventCount; ++index) {
        (void)sink.WriteEvent(
            "normal_capacity_event",
            {specforge::ProfileSink::Field::Number("index", std::to_string(index))});
    }
    sink.Stop();

    Require(
        sink.dropped_event_count() == 0,
        "ordinary writer/producer lock contention must not drop events below the queue capacity");
}

void TestFileLimitStopsRecording()
{
    TemporaryDirectory temporary;
    specforge::ProfileSink::Limits limits = GenerousLimits();
    limits.max_file_bytes = 1;
    specforge::ProfileSink sink(temporary.path() / "size-limited.jsonl", limits);

    for (int attempt = 0; attempt < 100 && sink.is_open(); ++attempt) {
        sink.WriteEvent("larger_than_session_budget");
        std::this_thread::yield();
    }
    Require(!sink.is_open(), "the session byte budget should stop further recording");
    Require(
        sink.stop_reason() == specforge::ProfileSink::StopReason::FileSizeLimit,
        "the recorder should expose that its file limit was reached");
    sink.Stop();
    Require(
        sink.stop_reason() == specforge::ProfileSink::StopReason::FileSizeLimit,
        "joining a limited recorder should preserve the original stop reason");
}

void TestDurationLimitStopsAnIdleRecording()
{
    TemporaryDirectory temporary;
    specforge::ProfileSink::Limits limits = GenerousLimits();
    limits.max_duration = 20ms;
    specforge::ProfileSink sink(temporary.path() / "duration-limited.jsonl", limits);

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (sink.is_open() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    Require(!sink.is_open(), "the duration budget should stop an idle recording without another producer event");
    Require(
        sink.stop_reason() == specforge::ProfileSink::StopReason::DurationLimit,
        "an idle timeout should expose the duration-limit reason");
    sink.Stop();
}

void TestStoppedSinkCanStartASecondSession()
{
    TemporaryDirectory temporary;
    const std::filesystem::path first_path = temporary.path() / "first.jsonl";
    const std::filesystem::path second_path = temporary.path() / "second.jsonl";
    specforge::ProfileSink sink(first_path, GenerousLimits());
    sink.WriteEvent("first_session");
    sink.Stop();

    Require(sink.Start(second_path, GenerousLimits()), "a stopped sink should start a new session");
    sink.WriteEvent("second_session");
    sink.Stop();

    Require(ReadTextFile(first_path).find("first_session") != std::string::npos, "the first session should persist");
    Require(ReadTextFile(second_path).find("second_session") != std::string::npos, "the second session should persist");
}

void TestBackgroundStopCanBeFinalizedWithoutBlockingTheRequest()
{
    TemporaryDirectory temporary;
    specforge::ProfileSink sink(temporary.path() / "background-stop.jsonl", GenerousLimits());
    sink.WriteEvent("queued_before_stop");

    sink.RequestStop();
    Require(!sink.is_open(), "RequestStop should stop accepting new events immediately");
    Require(sink.is_stopping(), "RequestStop should leave the writer drain observable");

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!sink.TryFinalizeStop() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    Require(!sink.is_stopping(), "a completed background drain should finalize without a blocking Stop call");
    Require(
        ReadTextFile(sink.path()).find("profile_recorder_summary") != std::string::npos,
        "a background stop should still finish with a recorder summary");
}

void TestIdleDurationStopNotifiesStateChange()
{
    TemporaryDirectory temporary;
    specforge::ProfileSink::Limits limits = GenerousLimits();
    limits.max_duration = 20ms;
    specforge::ProfileSink sink;
    std::atomic<int> notifications = 0;
    sink.SetStateChangeCallback([&notifications]() { notifications.fetch_add(1, std::memory_order_relaxed); });
    Require(
        sink.Start(temporary.path() / "duration-callback.jsonl", limits),
        "the duration callback recording should start");

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (notifications.load(std::memory_order_relaxed) < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    Require(
        notifications.load(std::memory_order_relaxed) >= 2,
        "an idle automatic stop should notify both the stop transition and completed drain");
    sink.Stop();
}

void TestRequestStopDoesNotWaitForSlowFinalFlush()
{
    const auto flush_state = std::make_shared<BlockingFlushState>();
    specforge::ProfileSink sink;
    Require(
        specforge::ProfileSinkTestAccess::StartWithOutputStreamFactory(
            sink,
            "injected-slow-final-flush.jsonl",
            GenerousLimits(),
            [flush_state](const std::filesystem::path&) {
                return std::make_unique<BlockingFlushStream>(flush_state);
            }),
        "the injected slow output stream should start in a writable state");
    sink.WriteEvent("event_before_slow_final_flush");

    std::atomic<bool> request_returned = false;
    std::thread watchdog([flush_state, &request_returned]() {
        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        while (!request_returned.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        if (!request_returned.load(std::memory_order_acquire)) {
            flush_state->release.store(true, std::memory_order_release);
        }
    });
    const auto request_started = std::chrono::steady_clock::now();
    sink.RequestStop();
    const auto request_elapsed = std::chrono::steady_clock::now() - request_started;
    request_returned.store(true, std::memory_order_release);

    const auto flush_deadline = std::chrono::steady_clock::now() + 1s;
    while (!flush_state->entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < flush_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool flush_entered = flush_state->entered.load(std::memory_order_acquire);
    flush_state->release.store(true, std::memory_order_release);
    watchdog.join();
    sink.Stop();

    Require(flush_entered, "the test writer should reach the injected slow final flush");
    Require(request_elapsed < 250ms, "RequestStop must not join or wait for a slow final flush");
}

void TestWriteFailureStatusIsNeverReportedAsSaved()
{
    specforge::ProfileSink sink;
    Require(
        specforge::ProfileSinkTestAccess::StartWithOutputStreamFactory(
            sink,
            "injected-final-flush-failure.jsonl",
            GenerousLimits(),
            [](const std::filesystem::path&) { return std::make_unique<FailingFlushStream>(); }),
        "the injected output stream should start in a writable state");
    sink.WriteEvent("event_before_final_flush");
    sink.Stop();

    Require(
        sink.stop_reason() == specforge::ProfileSink::StopReason::WriteFailure,
        "a failed final flush should become the recorder stop reason");
    const std::string message = specforge::ProfileRecordingStatusMessage(
        sink.stop_reason(),
        sink.dropped_event_count(),
        sink.error_message());
    Require(message.find("failed") != std::string::npos, "a final write failure should produce an error status");
    Require(message.find("saved") == std::string::npos, "a final write failure must never be reported as saved");
}

}  // namespace

int main()
{
    try {
        TestStopDrainsEventsAndWritesSummary();
        TestQueueLimitDropsInsteadOfGrowingWithoutBound();
        TestNormalCapacityDoesNotDropOnWriterContention();
        TestFileLimitStopsRecording();
        TestDurationLimitStopsAnIdleRecording();
        TestStoppedSinkCanStartASecondSession();
        TestBackgroundStopCanBeFinalizedWithoutBlockingTheRequest();
        TestIdleDurationStopNotifiesStateChange();
        TestRequestStopDoesNotWaitForSlowFinalFlush();
        TestWriteFailureStatusIsNeverReportedAsSaved();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
