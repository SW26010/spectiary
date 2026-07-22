#include "profile/profile_sink.h"

#include "app/runtime_paths.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace specforge {
namespace {

constexpr std::size_t kBatchTargetBytes = 64U * 1024U;
constexpr auto kBatchCollectionDelay = std::chrono::milliseconds(4);
constexpr auto kFlushInterval = std::chrono::seconds(1);

std::optional<std::string> ReadEnvironmentVariable(const char* name)
{
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, name) != 0 || raw_value == nullptr) {
        return std::nullopt;
    }

    std::unique_ptr<char, decltype(&std::free)> value(raw_value, std::free);
    return std::string(value.get());
}

bool IsProfileEnabled()
{
    std::optional<std::string> value = ReadEnvironmentVariable("SPECFORGE_PROFILE");
    if (!value) {
        return false;
    }

    std::string normalized = *value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes";
}

std::filesystem::path ProfileDirectory()
{
    std::optional<std::string> directory = ReadEnvironmentVariable("SPECFORGE_PROFILE_DIR");
    if (directory && !directory->empty()) {
        return std::filesystem::path(*directory);
    }
    return DefaultRuntimePaths().profile_log_directory;
}

bool IsFiniteJsonNumber(std::string_view value)
{
    if (value.empty()) {
        return false;
    }

    double parsed = 0.0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    return result.ec == std::errc() && result.ptr == end && std::isfinite(parsed);
}

}  // namespace

struct ProfileSink::WriterState {
    explicit WriterState(Limits session_limits) : limits(session_limits) {}

    Limits limits;
    std::unique_ptr<std::ostream> stream;
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::deque<std::string> queue;
    std::size_t queued_bytes = 0;
    std::uint64_t accepted_bytes = 0;
    std::atomic<std::uint64_t> dropped_events = 0;
    std::atomic<bool> accepting = true;
    std::atomic<bool> stop_requested = false;
    std::atomic<StopReason> stop_reason = StopReason::None;
    std::atomic<bool> writer_done = false;
    std::mutex callback_mutex;
    StateChangeCallback state_change_callback;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::thread writer;
};

ProfileSink::Field ProfileSink::Field::String(std::string_view name, std::string value)
{
    return Field{name, std::move(value), true};
}

ProfileSink::Field ProfileSink::Field::Number(std::string_view name, std::string value)
{
    return Field{name, std::move(value), false};
}

ProfileSink::Field ProfileSink::Field::Bool(std::string_view name, bool value)
{
    return Field{name, value ? "true" : "false", false};
}

ProfileSink::ProfileSink() = default;

ProfileSink::ProfileSink(std::filesystem::path path)
{
    (void)Start(std::move(path));
}

ProfileSink::ProfileSink(std::filesystem::path path, Limits limits)
{
    (void)Start(std::move(path), limits);
}

ProfileSink::~ProfileSink()
{
    Stop();
}

ProfileSink::ProfileSink(ProfileSink&& other) noexcept
    : path_(std::move(other.path_)),
      state_(std::move(other.state_)),
      last_stop_reason_(other.last_stop_reason_),
      last_dropped_event_count_(other.last_dropped_event_count_),
      error_message_(std::move(other.error_message_)),
      state_change_callback_(std::move(other.state_change_callback_))
{
}

ProfileSink& ProfileSink::operator=(ProfileSink&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    Stop();
    path_ = std::move(other.path_);
    state_ = std::move(other.state_);
    last_stop_reason_ = other.last_stop_reason_;
    last_dropped_event_count_ = other.last_dropped_event_count_;
    error_message_ = std::move(other.error_message_);
    state_change_callback_ = std::move(other.state_change_callback_);
    return *this;
}

ProfileSink ProfileSink::CreateDefault()
{
    ProfileSink sink;
    if (IsProfileEnabled()) {
        (void)sink.StartDefault();
    }
    return sink;
}

const char* ProfileSink::StopReasonName(StopReason reason) noexcept
{
    switch (reason) {
    case StopReason::None:
        return "none";
    case StopReason::Explicit:
        return "explicit";
    case StopReason::DurationLimit:
        return "duration_limit";
    case StopReason::FileSizeLimit:
        return "file_size_limit";
    case StopReason::WriteFailure:
        return "write_failure";
    }
    return "unknown";
}

bool ProfileSink::StartDefault()
{
    Stop();
    error_message_.clear();
    const std::filesystem::path directory = ProfileDirectory();
    const std::string stem = "specforge-profile-" + TimestampForFileName();
    std::filesystem::path path = directory / (stem + ".jsonl");
    std::error_code exists_error;
    for (unsigned int suffix = 2; std::filesystem::exists(path, exists_error) && !exists_error; ++suffix) {
        path = directory / (stem + "-" + std::to_string(suffix) + ".jsonl");
    }
    if (exists_error) {
        error_message_ = "Could not inspect the profile output directory: " + exists_error.message();
        return false;
    }
    return Start(std::move(path));
}

bool ProfileSink::Start(std::filesystem::path path)
{
    return Start(std::move(path), Limits{});
}

bool ProfileSink::Start(std::filesystem::path path, Limits limits)
{
    return StartWithOutputStreamFactory(
        std::move(path),
        limits,
        [](const std::filesystem::path& output_path) {
            return std::make_unique<std::ofstream>(
                output_path,
                std::ios::binary | std::ios::out | std::ios::trunc);
        });
}

bool ProfileSink::StartWithOutputStreamFactory(
    std::filesystem::path path,
    Limits limits,
    OutputStreamFactory output_stream_factory)
{
    Stop();
    path_ = std::move(path);
    last_stop_reason_ = StopReason::None;
    last_dropped_event_count_ = 0;
    error_message_.clear();

    if (path_.empty()) {
        error_message_ = "The profile output path is empty.";
        return false;
    }
    if (limits.max_queue_bytes == 0 || limits.max_file_bytes == 0) {
        error_message_ = "Profile queue and file limits must be greater than zero.";
        return false;
    }

    const std::filesystem::path parent = path_.parent_path();
    if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            error_message_ = "Could not create the profile output directory: " + directory_error.message();
            return false;
        }
    }

    auto state = std::make_unique<WriterState>(limits);
    state->state_change_callback = state_change_callback_;
    state->stream = output_stream_factory(path_);
    if (state->stream == nullptr || !state->stream->good()) {
        error_message_ = "Could not open the profile output file.";
        return false;
    }

    try {
        WriterState* state_pointer = state.get();
        state->writer = std::thread([state_pointer]() { WriterMain(state_pointer); });
    } catch (const std::system_error& error) {
        error_message_ = "Could not start the profile writer thread: " + std::string(error.what());
        return false;
    }

    state_ = std::move(state);
    return true;
}

void ProfileSink::SetStateChangeCallback(StateChangeCallback callback)
{
    state_change_callback_ = std::move(callback);
    if (state_ == nullptr) {
        return;
    }

    std::lock_guard lock(state_->callback_mutex);
    state_->state_change_callback = state_change_callback_;
}

void ProfileSink::RequestStop()
{
    WriterState* state = state_.get();
    if (state == nullptr) {
        return;
    }

    {
        std::lock_guard lock(state->queue_mutex);
        StopReason expected = StopReason::None;
        (void)state->stop_reason.compare_exchange_strong(expected, StopReason::Explicit);
        state->accepting.store(false, std::memory_order_release);
        state->stop_requested.store(true, std::memory_order_release);
    }
    state->queue_ready.notify_one();
    NotifyStateChange(state);
}

bool ProfileSink::TryFinalizeStop()
{
    if (state_ == nullptr || !state_->writer_done.load(std::memory_order_acquire)) {
        return false;
    }
    FinalizeStoppedState();
    return true;
}

void ProfileSink::Stop()
{
    if (state_ == nullptr) {
        return;
    }

    RequestStop();
    FinalizeStoppedState();
}

void ProfileSink::FinalizeStoppedState()
{
    if (state_->writer.joinable()) {
        state_->writer.join();
    }

    last_stop_reason_ = state_->stop_reason.load(std::memory_order_acquire);
    last_dropped_event_count_ = state_->dropped_events.load(std::memory_order_relaxed);
    if (last_stop_reason_ == StopReason::WriteFailure) {
        error_message_ = "Writing the profile output file failed.";
    }
    state_.reset();
}

bool ProfileSink::is_open() const noexcept
{
    return state_ && state_->accepting.load(std::memory_order_acquire);
}

bool ProfileSink::is_stopping() const noexcept
{
    return state_ != nullptr && !state_->accepting.load(std::memory_order_acquire);
}

ProfileSink::StopReason ProfileSink::stop_reason() const noexcept
{
    if (state_) {
        return state_->stop_reason.load(std::memory_order_acquire);
    }
    return last_stop_reason_;
}

std::uint64_t ProfileSink::dropped_event_count() const noexcept
{
    if (state_) {
        return state_->dropped_events.load(std::memory_order_relaxed);
    }
    return last_dropped_event_count_;
}

bool ProfileSink::WriteEvent(std::string_view event_name, std::initializer_list<Field> fields)
{
    if (!is_open()) {
        return false;
    }
    return Enqueue(BuildEventLine(event_name, fields));
}

void ProfileSink::WriteDuration(std::string_view event_name, std::uint64_t frame_index, double milliseconds)
{
    if (!is_open()) {
        return;
    }

    char value_buffer[64] = {};
    const auto value_result = std::to_chars(
        value_buffer,
        value_buffer + sizeof(value_buffer),
        milliseconds,
        std::chars_format::fixed,
        4);
    const std::string value = value_result.ec == std::errc()
                                  ? std::string(value_buffer, value_result.ptr)
                                  : std::string();

    (void)WriteEvent(event_name, {
                                     Field::Number("frame", std::to_string(frame_index)),
                                     Field::Number("duration_ms", value),
                                 });
}

bool ProfileSink::Enqueue(std::string line)
{
    WriterState* state = state_.get();
    if (state == nullptr || !state->accepting.load(std::memory_order_acquire)) {
        return false;
    }

    // This mutex only protects the bounded in-memory queue. The writer never holds it during
    // file I/O, so ordinary producer/writer scheduling contention must not discard evidence.
    std::unique_lock lock(state->queue_mutex);
    if (!state->accepting.load(std::memory_order_acquire)) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (state->limits.max_duration > std::chrono::steady_clock::duration::zero() &&
        now - state->started_at >= state->limits.max_duration) {
        StopReason expected = StopReason::None;
        const bool changed = state->stop_reason.compare_exchange_strong(expected, StopReason::DurationLimit);
        state->accepting.store(false, std::memory_order_release);
        state->stop_requested.store(true, std::memory_order_release);
        lock.unlock();
        state->queue_ready.notify_one();
        if (changed) {
            NotifyStateChange(state);
        }
        return false;
    }
    if (state->accepted_bytes + line.size() > state->limits.max_file_bytes) {
        StopReason expected = StopReason::None;
        const bool changed = state->stop_reason.compare_exchange_strong(expected, StopReason::FileSizeLimit);
        state->accepting.store(false, std::memory_order_release);
        state->stop_requested.store(true, std::memory_order_release);
        lock.unlock();
        state->queue_ready.notify_one();
        if (changed) {
            NotifyStateChange(state);
        }
        return false;
    }
    if (line.size() > state->limits.max_queue_bytes ||
        state->queued_bytes > state->limits.max_queue_bytes - line.size()) {
        state->dropped_events.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    state->accepted_bytes += line.size();
    state->queued_bytes += line.size();
    state->queue.push_back(std::move(line));
    lock.unlock();
    state->queue_ready.notify_one();
    return true;
}

void ProfileSink::WriterMain(WriterState* state)
{
    std::deque<std::string> batch;
    auto last_flush = std::chrono::steady_clock::now();
    const bool has_duration_limit =
        state->limits.max_duration > std::chrono::steady_clock::duration::zero();
    const auto duration_deadline = state->started_at + state->limits.max_duration;
    const auto mark_write_failure = [state]() {
        state->stop_reason.store(StopReason::WriteFailure, std::memory_order_release);
        state->accepting.store(false, std::memory_order_release);
        state->stop_requested.store(true, std::memory_order_release);
        NotifyStateChange(state);
    };

    while (true) {
        bool state_changed = false;
        bool should_stop = false;
        {
            std::unique_lock lock(state->queue_mutex);
            const auto work_ready = [state]() {
                return !state->queue.empty() || state->stop_requested.load(std::memory_order_acquire);
            };
            if (has_duration_limit) {
                if (!state->queue_ready.wait_until(lock, duration_deadline, work_ready)) {
                    StopReason expected = StopReason::None;
                    state_changed = state->stop_reason.compare_exchange_strong(expected, StopReason::DurationLimit);
                    state->accepting.store(false, std::memory_order_release);
                    state->stop_requested.store(true, std::memory_order_release);
                }
            } else {
                state->queue_ready.wait(lock, work_ready);
            }
            if (state->queue.empty() && state->stop_requested.load(std::memory_order_acquire)) {
                should_stop = true;
            }
            if (!should_stop && !state->stop_requested.load(std::memory_order_acquire) &&
                state->queued_bytes < kBatchTargetBytes) {
                (void)state->queue_ready.wait_for(lock, kBatchCollectionDelay, [state]() {
                    return state->stop_requested.load(std::memory_order_acquire) ||
                           state->queued_bytes >= kBatchTargetBytes;
                });
            }
            if (!should_stop) {
                batch.swap(state->queue);
                state->queued_bytes = 0;
            }
        }
        if (state_changed) {
            NotifyStateChange(state);
        }
        if (should_stop) {
            break;
        }

        for (const std::string& line : batch) {
            state->stream->write(line.data(), static_cast<std::streamsize>(line.size()));
        }
        batch.clear();

        if (!state->stream->good()) {
            mark_write_failure();
            std::lock_guard lock(state->queue_mutex);
            state->queue.clear();
            state->queued_bytes = 0;
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush >= kFlushInterval) {
            state->stream->flush();
            last_flush = now;
            if (!state->stream->good()) {
                mark_write_failure();
                std::lock_guard lock(state->queue_mutex);
                state->queue.clear();
                state->queued_bytes = 0;
                break;
            }
        }
    }

    if (state->stream->good()) {
        const StopReason reason = state->stop_reason.load(std::memory_order_acquire);
        const std::string summary = BuildEventLine(
            "profile_recorder_summary",
            {
                Field::String("stop_reason", StopReasonName(reason)),
                Field::Number("accepted_bytes", std::to_string(state->accepted_bytes)),
                Field::Number("dropped_events", std::to_string(state->dropped_events.load(std::memory_order_relaxed))),
            });
        state->stream->write(summary.data(), static_cast<std::streamsize>(summary.size()));
        state->stream->flush();
    }
    if (!state->stream->good()) {
        mark_write_failure();
    }
    state->writer_done.store(true, std::memory_order_release);
    NotifyStateChange(state);
}

void ProfileSink::NotifyStateChange(WriterState* state) noexcept
{
    StateChangeCallback callback;
    try {
        std::lock_guard lock(state->callback_mutex);
        callback = state->state_change_callback;
    } catch (...) {
        return;
    }

    if (callback) {
        try {
            callback();
        } catch (...) {
        }
    }
}

std::int64_t ProfileSink::SteadyNanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string ProfileSink::BuildEventLine(std::string_view event_name, std::initializer_list<Field> fields)
{
    std::string line;
    line.reserve(192);
    line += "{\"steady_ns\":";
    line += std::to_string(SteadyNanoseconds());
    line += ",\"event\":\"";
    AppendEscapedJson(line, event_name);
    line += '"';
    for (const Field& field : fields) {
        line += ",\"";
        AppendEscapedJson(line, field.name);
        line += "\":";
        if (field.quoted) {
            line += '"';
            AppendEscapedJson(line, field.value);
            line += '"';
        } else if (field.value == "true" || field.value == "false" || field.value == "null" ||
                   IsFiniteJsonNumber(field.value)) {
            line += field.value;
        } else {
            line += "null";
        }
    }
    line += "}\n";
    return line;
}

void ProfileSink::AppendEscapedJson(std::string& output, std::string_view value)
{
    constexpr char kHexDigits[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (character < 0x20) {
                output += "\\u00";
                output += kHexDigits[(character >> 4U) & 0x0FU];
                output += kHexDigits[character & 0x0FU];
            } else {
                output += static_cast<char>(character);
            }
            break;
        }
    }
}

std::string ProfileSink::TimestampForFileName()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm local_time = {};
    localtime_s(&local_time, &time);

    std::ostringstream timestamp;
    timestamp << std::put_time(&local_time, "%Y%m%d-%H%M%S") << '-' << std::setw(3) << std::setfill('0')
              << milliseconds;
    return timestamp.str();
}

ProfileTimer::ProfileTimer(ProfileSink& sink, std::string_view event_name, std::uint64_t frame_index) : sink_(sink)
{
    if (!sink_.is_open()) {
        return;
    }

    active_ = true;
    event_name_ = event_name;
    frame_index_ = frame_index;
    start_ = std::chrono::steady_clock::now();
}

ProfileTimer::~ProfileTimer()
{
    if (!active_) {
        return;
    }

    const auto elapsed = std::chrono::steady_clock::now() - start_;
    const double milliseconds = std::chrono::duration<double, std::milli>(elapsed).count();
    sink_.WriteDuration(event_name_, frame_index_, milliseconds);
}

}  // namespace specforge
