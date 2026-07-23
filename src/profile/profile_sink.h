#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace specforge {

class ProfileSink {
public:
    using StateChangeCallback = std::function<void()>;

    struct Field {
        std::string_view name;
        std::string value;
        bool quoted = true;

        static Field String(std::string_view name, std::string value);
        static Field Number(std::string_view name, std::string value);
        static Field Bool(std::string_view name, bool value);
    };

    struct Limits {
        std::size_t max_queue_bytes = 4U * 1024U * 1024U;
        std::uint64_t max_file_bytes = 100ULL * 1024ULL * 1024ULL;
        std::chrono::steady_clock::duration max_duration = std::chrono::minutes(5);
    };

    struct StateSnapshot {
        bool open = false;
        bool stopping = false;
        bool frame_finalization_pending = false;
        bool frame_recording_active = false;
    };

    enum class StopReason : std::uint8_t {
        None,
        Explicit,
        DurationLimit,
        FileSizeLimit,
        WriteFailure,
    };

    ProfileSink();
    explicit ProfileSink(std::filesystem::path path);
    ProfileSink(std::filesystem::path path, Limits limits);
    ~ProfileSink();

    ProfileSink(ProfileSink&& other) noexcept;
    ProfileSink& operator=(ProfileSink&& other) noexcept;

    ProfileSink(const ProfileSink&) = delete;
    ProfileSink& operator=(const ProfileSink&) = delete;

    static ProfileSink CreateDefault();
    static const char* StopReasonName(StopReason reason) noexcept;
    [[nodiscard]] static std::filesystem::path EffectiveOutputDirectory();

    [[nodiscard]] bool StartDefault();
    [[nodiscard]] bool Start(std::filesystem::path path);
    [[nodiscard]] bool Start(std::filesystem::path path, Limits limits);
    void SetStateChangeCallback(StateChangeCallback callback);
    // Marks a real render frame as in progress. Automatic limits retain a tail only while
    // such a frame is active; idle/minimized sessions seal immediately.
    void BeginFrame();
    // Stops accepting events and asks the writer to drain without joining it.
    void RequestStop();
    // Stops the ordinary recording session while retaining this frame's final events.
    // CompleteFrameFinalization seals that tail and lets the writer finish.
    void RequestStopAfterFrame();
    void CompleteFrameFinalization();
    // Finalizes a completed background stop without waiting for file I/O.
    [[nodiscard]] bool TryFinalizeStop();
    // Stops accepting events, drains the bounded queue, writes the summary, and joins the writer.
    void Stop();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;
    [[nodiscard]] bool is_frame_finalization_pending() const noexcept;
    [[nodiscard]] bool is_frame_recording_active() const noexcept;
    [[nodiscard]] StateSnapshot state_snapshot() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] StopReason stop_reason() const noexcept;
    [[nodiscard]] std::uint64_t dropped_event_count() const noexcept;
    [[nodiscard]] const std::string& error_message() const noexcept { return error_message_; }

    // Returns false when recording is inactive or the event was rejected by a recorder limit.
    bool WriteEvent(std::string_view event_name, std::initializer_list<Field> fields = {});
    void WriteDuration(std::string_view event_name, std::uint64_t frame_index, double milliseconds);

private:
    struct WriterState;
    using OutputStreamFactory =
        std::function<std::unique_ptr<std::ostream>(const std::filesystem::path&)>;

    friend struct ProfileSinkTestAccess;

    static std::int64_t SteadyNanoseconds();
    static std::string BuildEventLine(std::string_view event_name, std::initializer_list<Field> fields);
    static void AppendEscapedJson(std::string& output, std::string_view value);
    static std::string TimestampForFileName();
    static void WriterMain(WriterState* state);
    static void NotifyStateChange(WriterState* state) noexcept;

    bool Enqueue(std::string line);
    bool StartWithOutputStreamFactory(
        std::filesystem::path path,
        Limits limits,
        OutputStreamFactory output_stream_factory);
    void FinalizeStoppedState();

    std::filesystem::path path_;
    std::unique_ptr<WriterState> state_;
    StopReason last_stop_reason_ = StopReason::None;
    std::uint64_t last_dropped_event_count_ = 0;
    std::string error_message_;
    StateChangeCallback state_change_callback_;
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
    std::uint64_t frame_index_ = 0;
    std::chrono::steady_clock::time_point start_;
    bool active_ = false;
};

}  // namespace specforge
