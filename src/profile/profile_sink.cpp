#include "profile/profile_sink.h"
#include "platform/win32_file_identity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace spectiary {
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
    std::optional<std::string> value = ReadEnvironmentVariable("SPECTIARY_PROFILE");
    if (!value) {
        return false;
    }

    std::string normalized = *value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes";
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

enum class FrameAdmissionState : std::uint8_t {
    Accepting,
    Finalizing,
    Closed,
};

constexpr unsigned int kMaximumDefaultProfileNameAttempts = 10000;

class ProfileFileStreamBuffer final : public std::streambuf {
public:
    explicit ProfileFileStreamBuffer(HANDLE handle)
        : handle_(handle)
    {
        setp(
            buffer_.data(),
            buffer_.data() + buffer_.size());
    }

    ~ProfileFileStreamBuffer() override = default;

protected:
    int sync() override
    {
        if (!FlushPending()) {
            return -1;
        }
        return FlushFileBuffers(handle_) != FALSE
            ? 0
            : -1;
    }

    int_type overflow(int_type character) override
    {
        if (!FlushPending()) {
            return traits_type::eof();
        }
        if (!traits_type::eq_int_type(
                character,
                traits_type::eof())) {
            *pptr() = traits_type::to_char_type(character);
            pbump(1);
        }
        return traits_type::not_eof(character);
    }

private:
    [[nodiscard]] bool FlushPending()
    {
        if (failed_) {
            return false;
        }
        const std::ptrdiff_t pending =
            pptr() - pbase();
        std::ptrdiff_t offset = 0;
        while (offset < pending) {
            const DWORD remaining =
                static_cast<DWORD>(pending - offset);
            DWORD written = 0;
            if (WriteFile(
                    handle_,
                    pbase() + offset,
                    remaining,
                    &written,
                    nullptr) == FALSE ||
                written == 0) {
                failed_ = true;
                setp(
                    buffer_.data(),
                    buffer_.data() + buffer_.size());
                return false;
            }
            offset += written;
        }
        setp(
            buffer_.data(),
            buffer_.data() + buffer_.size());
        return true;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::array<char, 64U * 1024U> buffer_ = {};
    bool failed_ = false;
};

class ProfileFileOutputStream final : public std::ostream {
public:
    explicit ProfileFileOutputStream(HANDLE handle)
        : std::ostream(nullptr),
          handle_(handle),
          buffer_(handle_)
    {
        rdbuf(&buffer_);
        clear();
    }

    ~ProfileFileOutputStream() override
    {
        (void)flush();
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    ProfileFileStreamBuffer buffer_;
};

struct ProfileFileOpenResult {
    std::unique_ptr<std::ostream> stream;
    bool collision = false;
    std::string error_message;
};

ProfileFileOpenResult OpenProfileFileExclusively(
    const std::filesystem::path& path)
{
    const std::filesystem::path extended_path =
        Win32ExtendedLengthPath(path);
    if (extended_path.empty()) {
        return {
            .error_message =
                "Could not resolve the profile output path.",
        };
    }

    const HANDLE handle =
        CreateFileW(
            extended_path.c_str(),
            GENERIC_WRITE |
                FILE_READ_ATTRIBUTES |
                SYNCHRONIZE,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL |
                FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS ||
            error == ERROR_ALREADY_EXISTS) {
            return {
                .collision = true,
            };
        }
        return {
            .error_message =
                "Could not create the profile output file: " +
                std::error_code(
                    static_cast<int>(error),
                    std::system_category())
                    .message(),
        };
    }

    return {
        .stream = std::make_unique<ProfileFileOutputStream>(handle),
    };
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
    std::atomic<FrameAdmissionState> frame_admission = FrameAdmissionState::Accepting;
    bool frame_in_progress = false;  // Protected by queue_mutex.
    std::atomic<bool> stop_requested = false;
    std::atomic<StopReason> stop_reason = StopReason::None;
    std::atomic<bool> writer_done = false;
    StopTransitionCheckpoint
        stop_transition_checkpoint;
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

ProfileSink ProfileSink::CreateDefault(
    const std::filesystem::path& output_directory)
{
    return CreateDefault(output_directory, Limits{});
}

ProfileSink ProfileSink::CreateDefault(
    const std::filesystem::path& output_directory,
    Limits limits)
{
    ProfileSink sink;
    if (IsProfileEnabled()) {
        (void)sink.StartDefault(
            output_directory,
            limits);
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

bool ProfileSink::StartDefault(
    const std::filesystem::path& output_directory)
{
    return StartDefault(output_directory, Limits{});
}

bool ProfileSink::StartDefault(
    const std::filesystem::path& output_directory,
    Limits limits)
{
    return StartDefaultWithNameFactory(
        output_directory,
        limits,
        []() {
            return TimestampForFileName();
        });
}

bool ProfileSink::StartDefaultWithNameFactory(
    const std::filesystem::path& output_directory,
    Limits limits,
    DefaultProfileNameFactory name_factory,
    DefaultProfileOpenCheckpoint before_open)
{
    Stop();
    ResetStoppedOutcome();

    if (!name_factory) {
        error_message_ =
            "The default profile name factory is empty.";
        return false;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(
        output_directory,
        directory_error);
    if (directory_error) {
        error_message_ =
            "Could not create the profile output directory: " +
            directory_error.message();
        return false;
    }

    const std::string stem =
        "spectiary-profile-" + name_factory();
    for (unsigned int suffix = 0;
         suffix < kMaximumDefaultProfileNameAttempts;
         ++suffix) {
        std::string filename = stem;
        if (suffix != 0) {
            filename += "-" +
                std::to_string(suffix + 1);
        }
        filename += ".jsonl";
        const std::filesystem::path path =
            output_directory / filename;
        if (before_open && suffix == 0) {
            before_open(path);
        }

        ProfileFileOpenResult opened =
            OpenProfileFileExclusively(path);
        if (opened.collision) {
            continue;
        }
        if (!opened.stream) {
            error_message_ =
                opened.error_message.empty()
                ? "Could not open the profile output file."
                : std::move(opened.error_message);
            return false;
        }

        const std::filesystem::path discard_path =
            path;
        return StartPrepared(
            path,
            limits,
            std::move(opened.stream),
            [discard_path]() noexcept {
                std::error_code remove_error;
                (void)std::filesystem::remove(
                    discard_path,
                    remove_error);
            });
    }

    error_message_ =
        "Could not allocate a collision-free profile output filename.";
    return false;
}

bool ProfileSink::Start(std::filesystem::path path)
{
    return Start(std::move(path), Limits{});
}

bool ProfileSink::Start(std::filesystem::path path, Limits limits)
{
    std::string open_error;
    const bool started =
        StartWithOutputStreamFactory(
        std::move(path),
        limits,
        [&open_error](const std::filesystem::path& output_path) {
            ProfileFileOpenResult opened =
                OpenProfileFileExclusively(output_path);
            if (opened.collision) {
                open_error =
                    "The explicit profile output path already exists.";
            } else if (!opened.stream) {
                open_error =
                    opened.error_message;
            }
            return std::move(opened.stream);
        });
    if (!started && !open_error.empty()) {
        error_message_ = std::move(open_error);
    }
    return started;
}

bool ProfileSink::StartPrepared(
    std::filesystem::path path,
    Limits limits,
    std::unique_ptr<std::ostream> stream,
    OutputDiscard discard_output)
{
    Stop();
    ResetStoppedOutcome();
    const auto reject_prepared_output =
        [&](std::string message) {
            error_message_ = std::move(message);
            stream.reset();
            try {
                if (discard_output) {
                    discard_output();
                }
            } catch (...) {
            }
            path_.clear();
            return false;
        };
    if (path.empty()) {
        return reject_prepared_output(
            "The profile output path is empty.");
    }
    if (limits.max_queue_bytes == 0 ||
        limits.max_file_bytes == 0) {
        return reject_prepared_output(
            "Profile queue and file limits must be greater than zero.");
    }
    if (stream == nullptr || !stream->good()) {
        return reject_prepared_output(
            "Could not open the profile output file.");
    }

    auto stream_holder =
        std::make_shared<
            std::unique_ptr<std::ostream>>(
                std::move(stream));
    return StartWithOutputStreamFactory(
        std::move(path),
        limits,
        [stream_holder](
            const std::filesystem::path&) mutable {
            return std::move(*stream_holder);
        },
        false,
        std::move(discard_output));
}

void ProfileSink::ResetStoppedOutcome() noexcept
{
    if (state_ != nullptr) {
        return;
    }
    path_.clear();
    last_stop_reason_ = StopReason::None;
    last_dropped_event_count_ = 0;
    error_message_.clear();
}

bool ProfileSink::StartWithOutputStreamFactory(
    std::filesystem::path path,
    Limits limits,
    OutputStreamFactory output_stream_factory,
    bool create_parent_directories,
    OutputDiscard discard_output,
    WriterThreadStarter writer_thread_starter,
    StopTransitionCheckpoint
        stop_transition_checkpoint)
{
    Stop();
    ResetStoppedOutcome();
    path_ = std::move(path);
    const auto discard_without_stream = [&]() noexcept {
        try {
            if (discard_output) {
                discard_output();
            }
        } catch (...) {
        }
        path_.clear();
    };

    if (path_.empty()) {
        error_message_ = "The profile output path is empty.";
        discard_without_stream();
        return false;
    }
    if (limits.max_queue_bytes == 0 || limits.max_file_bytes == 0) {
        error_message_ = "Profile queue and file limits must be greater than zero.";
        discard_without_stream();
        return false;
    }

    const std::filesystem::path parent = path_.parent_path();
    if (create_parent_directories &&
        !parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            error_message_ = "Could not create the profile output directory: " + directory_error.message();
            path_.clear();
            return false;
        }
    }

    auto state = std::make_unique<WriterState>(limits);
    state->state_change_callback = state_change_callback_;
    state->stop_transition_checkpoint =
        std::move(stop_transition_checkpoint);
    state->stream = output_stream_factory(path_);
    const std::filesystem::path opened_path = path_;
    const auto discard_opened_output = [&]() noexcept {
        state->stream.reset();
        try {
            if (discard_output) {
                discard_output();
            }
        } catch (...) {
        }
        if (!discard_output) {
            std::error_code remove_error;
            (void)std::filesystem::remove(
                opened_path,
                remove_error);
        }
        path_.clear();
    };
    if (state->stream == nullptr || !state->stream->good()) {
        error_message_ = "Could not open the profile output file.";
        if (discard_output) {
            discard_opened_output();
        } else {
            state->stream.reset();
            path_.clear();
        }
        return false;
    }

    try {
        WriterState* state_pointer = state.get();
        state->writer = writer_thread_starter
            ? writer_thread_starter(state_pointer)
            : std::thread([state_pointer]() {
                  WriterMain(state_pointer);
              });
    } catch (const std::system_error& error) {
        error_message_ = "Could not start the profile writer thread: " + std::string(error.what());
        discard_opened_output();
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
        state->frame_in_progress = false;
        state->frame_admission.store(FrameAdmissionState::Closed, std::memory_order_release);
        state->stop_requested.store(true, std::memory_order_release);
    }
    state->queue_ready.notify_one();
    NotifyStateChange(state);
}

void ProfileSink::BeginFrame()
{
    WriterState* state = state_.get();
    if (state == nullptr) {
        return;
    }

    std::lock_guard lock(state->queue_mutex);
    if (state->frame_admission.load(std::memory_order_acquire) ==
            FrameAdmissionState::Accepting &&
        !state->stop_requested.load(std::memory_order_acquire)) {
        state->frame_in_progress = true;
    }
}

void ProfileSink::RequestStopAfterFrame()
{
    WriterState* state = state_.get();
    if (state == nullptr) {
        return;
    }

    bool changed = false;
    {
        std::lock_guard lock(state->queue_mutex);
        StopReason expected = StopReason::None;
        (void)state->stop_reason.compare_exchange_strong(expected, StopReason::Explicit);
        if (!state->stop_requested.load(std::memory_order_acquire)) {
            FrameAdmissionState admission = FrameAdmissionState::Accepting;
            if (state->frame_admission.compare_exchange_strong(
                    admission,
                    state->frame_in_progress
                        ? FrameAdmissionState::Finalizing
                        : FrameAdmissionState::Closed,
                    std::memory_order_acq_rel)) {
                changed = true;
                if (!state->frame_in_progress) {
                    state->stop_requested.store(true, std::memory_order_release);
                }
            }
        }
    }
    if (changed) {
        state->queue_ready.notify_one();
        NotifyStateChange(state);
    }
}

void ProfileSink::CompleteFrameFinalization()
{
    WriterState* state = state_.get();
    if (state == nullptr) {
        return;
    }

    bool changed = false;
    {
        std::lock_guard lock(state->queue_mutex);
        state->frame_in_progress = false;
        FrameAdmissionState expected = FrameAdmissionState::Finalizing;
        changed = state->frame_admission.compare_exchange_strong(
            expected,
            FrameAdmissionState::Closed,
            std::memory_order_acq_rel);
        if (changed) {
            state->stop_requested.store(true, std::memory_order_release);
        }
    }
    if (changed) {
        state->queue_ready.notify_one();
    }
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
    return state_ &&
        state_->frame_admission.load(std::memory_order_acquire) ==
            FrameAdmissionState::Accepting;
}

bool ProfileSink::is_stopping() const noexcept
{
    return state_ != nullptr &&
        state_->frame_admission.load(std::memory_order_acquire) !=
            FrameAdmissionState::Accepting;
}

bool ProfileSink::is_frame_finalization_pending() const noexcept
{
    return state_ != nullptr &&
        state_->frame_admission.load(std::memory_order_acquire) ==
            FrameAdmissionState::Finalizing;
}

bool ProfileSink::is_frame_recording_active() const noexcept
{
    return state_ != nullptr &&
        state_->frame_admission.load(std::memory_order_acquire) !=
            FrameAdmissionState::Closed;
}

ProfileSink::LifecycleSnapshot
ProfileSink::lifecycle_snapshot() const
{
    if (state_ == nullptr) {
        return {
            .stop_reason = last_stop_reason_,
            .dropped_events =
                last_dropped_event_count_,
        };
    }
    std::lock_guard lock(state_->queue_mutex);
    const FrameAdmissionState admission =
        state_->frame_admission.load(std::memory_order_acquire);
    return {
        .open = admission == FrameAdmissionState::Accepting,
        .stopping = admission != FrameAdmissionState::Accepting,
        .frame_finalization_pending = admission == FrameAdmissionState::Finalizing,
        .frame_recording_active = admission != FrameAdmissionState::Closed,
        .stop_reason = state_->stop_reason.load(
            std::memory_order_acquire),
        .dropped_events = state_->dropped_events.load(
            std::memory_order_relaxed),
    };
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
    return Enqueue(BuildEventLine(event_name, fields));
}

void ProfileSink::WriteDuration(std::string_view event_name, std::uint64_t frame_index, double milliseconds)
{
    if (!is_frame_recording_active()) {
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
    if (state == nullptr) {
        return false;
    }

    // This mutex only protects the bounded in-memory queue. The writer never holds it during
    // file I/O, so ordinary producer/writer scheduling contention must not discard evidence.
    std::unique_lock lock(state->queue_mutex);
    const FrameAdmissionState admission =
        state->frame_admission.load(std::memory_order_acquire);
    if (admission == FrameAdmissionState::Closed) {
        return false;
    }
    bool state_changed = false;
    if (admission == FrameAdmissionState::Accepting) {
        const auto now = std::chrono::steady_clock::now();
        if (state->limits.max_duration > std::chrono::steady_clock::duration::zero() &&
            now - state->started_at >= state->limits.max_duration) {
            StopReason expected = StopReason::None;
            (void)state->stop_reason.compare_exchange_strong(
                expected,
                StopReason::DurationLimit);
            state->frame_admission.store(
                state->frame_in_progress
                    ? FrameAdmissionState::Finalizing
                    : FrameAdmissionState::Closed,
                std::memory_order_release);
            if (!state->frame_in_progress) {
                state->stop_requested.store(true, std::memory_order_release);
            }
            state_changed = true;
        } else if (state->accepted_bytes + line.size() > state->limits.max_file_bytes) {
            StopReason expected = StopReason::None;
            (void)state->stop_reason.compare_exchange_strong(
                expected,
                StopReason::FileSizeLimit);
            state->frame_admission.store(
                state->frame_in_progress
                    ? FrameAdmissionState::Finalizing
                    : FrameAdmissionState::Closed,
                std::memory_order_release);
            if (!state->frame_in_progress) {
                state->stop_requested.store(true, std::memory_order_release);
            }
            state_changed = true;
        }
    }
    if (line.size() > state->limits.max_queue_bytes ||
        state->queued_bytes > state->limits.max_queue_bytes - line.size()) {
        state->dropped_events.fetch_add(1, std::memory_order_relaxed);
        lock.unlock();
        if (state_changed) {
            state->queue_ready.notify_one();
            NotifyStateChange(state);
        }
        return false;
    }

    state->accepted_bytes += line.size();
    state->queued_bytes += line.size();
    state->queue.push_back(std::move(line));
    lock.unlock();
    state->queue_ready.notify_one();
    if (state_changed) {
        NotifyStateChange(state);
    }
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
        {
            std::lock_guard lock(state->queue_mutex);
            state->stop_reason.store(
                StopReason::WriteFailure,
                std::memory_order_release);
            if (state->stop_transition_checkpoint) {
                state->stop_transition_checkpoint();
            }
            state->frame_admission.store(
                FrameAdmissionState::Closed,
                std::memory_order_release);
            state->stop_requested.store(
                true,
                std::memory_order_release);
            state->queue.clear();
            state->queued_bytes = 0;
        }
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
            if (has_duration_limit &&
                state->frame_admission.load(std::memory_order_acquire) ==
                    FrameAdmissionState::Accepting) {
                if (!state->queue_ready.wait_until(lock, duration_deadline, work_ready)) {
                    StopReason expected = StopReason::None;
                    state_changed = state->stop_reason.compare_exchange_strong(expected, StopReason::DurationLimit);
                    if (state_changed &&
                        state->stop_transition_checkpoint) {
                        state->stop_transition_checkpoint();
                    }
                    state->frame_admission.store(
                        state->frame_in_progress
                            ? FrameAdmissionState::Finalizing
                            : FrameAdmissionState::Closed,
                        std::memory_order_release);
                    if (!state->frame_in_progress) {
                        state->stop_requested.store(true, std::memory_order_release);
                    }
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
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush >= kFlushInterval) {
            state->stream->flush();
            last_flush = now;
            if (!state->stream->good()) {
                mark_write_failure();
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
    static std::atomic_uint32_t process_sequence = 0;
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm local_time = {};
    localtime_s(&local_time, &time);

    std::ostringstream timestamp;
    timestamp << std::put_time(&local_time, "%Y%m%d-%H%M%S") << '-' << std::setw(3) << std::setfill('0')
              << milliseconds << '-' << GetCurrentProcessId()
              << '-' << ++process_sequence;
    return timestamp.str();
}

ProfileTimer::ProfileTimer(ProfileSink& sink, std::string_view event_name, std::uint64_t frame_index) : sink_(sink)
{
    if (!sink_.is_frame_recording_active()) {
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

}  // namespace spectiary
