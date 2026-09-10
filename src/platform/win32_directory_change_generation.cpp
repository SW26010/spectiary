#include "platform/directory_change_generation.h"

namespace specforge {

void DirectoryChangeGenerationMonitor::CloseRegistration(
    DirectoryChangeGeneration& registration) noexcept
{
    registration.Close();
}

}  // namespace specforge

#if defined(_WIN32)

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace specforge {
namespace {

using namespace std::chrono_literals;

class Win32DirectoryChangeGeneration final : public DirectoryChangeGeneration {
public:
    explicit Win32DirectoryChangeGeneration(HANDLE change_notification) noexcept
        : change_notification_(change_notification)
    {
    }

    ~Win32DirectoryChangeGeneration() override
    {
        Close();
    }

    [[nodiscard]] bool IsCurrent() const noexcept override
    {
        std::lock_guard lock(mutex_);
        return change_notification_ != INVALID_HANDLE_VALUE &&
               WaitForSingleObject(change_notification_, 0) == WAIT_TIMEOUT;
    }

private:
    void Close() noexcept override
    {
        std::lock_guard lock(mutex_);
        if (change_notification_ == INVALID_HANDLE_VALUE) {
            return;
        }
        (void)FindCloseChangeNotification(change_notification_);
        change_notification_ = INVALID_HANDLE_VALUE;
    }

    mutable std::mutex mutex_;
    HANDLE change_notification_ = INVALID_HANDLE_VALUE;
};

std::shared_ptr<DirectoryChangeGeneration> BeginDirectoryChangeGenerationOnCurrentThread(
    const std::filesystem::path& path,
    std::stop_token)
{
    constexpr DWORD kChangeFilter =
        FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES |
        FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_CREATION |
        FILE_NOTIFY_CHANGE_SECURITY;
    const HANDLE change_notification =
        FindFirstChangeNotificationW(path.c_str(), FALSE, kChangeFilter);
    if (change_notification == INVALID_HANDLE_VALUE) {
        return {};
    }
    try {
        return std::make_shared<Win32DirectoryChangeGeneration>(
            change_notification);
    } catch (...) {
        (void)FindCloseChangeNotification(change_notification);
        throw;
    }
}

}  // namespace

class DirectoryChangeGenerationMonitor::Impl {
public:
    explicit Impl(RegistrationFactory registration_factory)
        : registration_factory_(std::move(registration_factory)),
          worker_([this](std::stop_token stop_token) {
              Run(stop_token);
          })
    {
    }

    ~Impl()
    {
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] DirectoryChangeGenerationHandle Begin(
        const std::filesystem::path& path,
        const std::function<void()>& cancellation_checkpoint,
        std::stop_token cancellation_token)
    {
        cancellation_checkpoint();
        if (cancellation_token.stop_requested()) {
            return {};
        }
        auto request = std::make_shared<Request>();
        request->path = path;
        {
            std::lock_guard lock(mutex_);
            if (worker_.get_stop_token().stop_requested()) {
                return {};
            }
            requests_.push_back(request);
        }
        condition_.notify_one();

        constexpr auto kRegistrationTimeout = 5s;
        const auto deadline =
            std::chrono::steady_clock::now() + kRegistrationTimeout;
        std::unique_lock lock(request->mutex);
        const bool completed = request->condition.wait_until(
            lock, cancellation_token, deadline, [&request]() {
                return request->completed;
            });
        if (!completed || cancellation_token.stop_requested()) {
            request->abandoned = true;
            lock.unlock();
            cancellation_checkpoint();
            return {};
        }
        auto result = std::move(request->result);
        auto error = request->error;
        lock.unlock();
        cancellation_checkpoint();
        if (error) {
            std::rethrow_exception(error);
        }
        return result;
    }

private:
    struct Request {
        std::filesystem::path path;
        std::mutex mutex;
        std::condition_variable_any condition;
        DirectoryChangeGenerationHandle result;
        std::exception_ptr error;
        bool completed = false;
        bool abandoned = false;
    };

    void ReapUnusedRegistrations()
    {
        for (auto registration = registrations_.begin();
             registration != registrations_.end();) {
            if (registration->use_count() != 1) {
                ++registration;
                continue;
            }
            DirectoryChangeGenerationMonitor::CloseRegistration(
                **registration);
            registration = registrations_.erase(registration);
        }
    }

    void CloseRegistrationsForShutdown() noexcept
    {
        for (const std::shared_ptr<DirectoryChangeGeneration>& registration :
             registrations_) {
            DirectoryChangeGenerationMonitor::CloseRegistration(*registration);
        }
        registrations_.clear();
    }

    void Run(std::stop_token stop_token)
    {
        for (;;) {
            std::shared_ptr<Request> request;
            std::deque<std::shared_ptr<Request>> canceled_requests;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stop_token, [this]() {
                    return !requests_.empty();
                });
                if (stop_token.stop_requested()) {
                    canceled_requests.swap(requests_);
                } else {
                    request = std::move(requests_.front());
                    requests_.pop_front();
                }
            }
            for (const std::shared_ptr<Request>& canceled : canceled_requests) {
                {
                    std::lock_guard lock(canceled->mutex);
                    canceled->completed = true;
                }
                canceled->condition.notify_all();
            }
            if (stop_token.stop_requested()) {
                CloseRegistrationsForShutdown();
                return;
            }
            try {
                {
                    std::lock_guard lock(request->mutex);
                    if (request->abandoned) {
                        continue;
                    }
                }
                std::shared_ptr<DirectoryChangeGeneration> registration =
                    registration_factory_(request->path, stop_token);
                std::lock_guard lock(request->mutex);
                if (request->abandoned) {
                    if (registration) {
                        DirectoryChangeGenerationMonitor::CloseRegistration(
                            *registration);
                    }
                } else {
                    if (registration) {
                        registrations_.push_back(registration);
                    }
                    request->result = std::move(registration);
                }
                request->completed = true;
            } catch (...) {
                std::lock_guard lock(request->mutex);
                request->error = std::current_exception();
                request->completed = true;
            }
            request->condition.notify_all();
            request.reset();
            ReapUnusedRegistrations();
        }
    }

    RegistrationFactory registration_factory_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<std::shared_ptr<Request>> requests_;
    std::vector<std::shared_ptr<DirectoryChangeGeneration>> registrations_;
    std::jthread worker_;
};

DirectoryChangeGenerationMonitor::DirectoryChangeGenerationMonitor()
    : impl_(std::make_unique<Impl>(
          BeginDirectoryChangeGenerationOnCurrentThread))
{
}

DirectoryChangeGenerationMonitor::DirectoryChangeGenerationMonitor(
    RegistrationFactory registration_factory)
    : impl_(std::make_unique<Impl>(
          registration_factory
              ? std::move(registration_factory)
              : RegistrationFactory{
                    BeginDirectoryChangeGenerationOnCurrentThread}))
{
}

DirectoryChangeGenerationMonitor::~DirectoryChangeGenerationMonitor() = default;

DirectoryChangeGenerationHandle DirectoryChangeGenerationMonitor::Begin(
    const std::filesystem::path& path,
    const std::function<void()>& cancellation_checkpoint,
    std::stop_token cancellation_token)
{
    return impl_->Begin(path, cancellation_checkpoint, cancellation_token);
}

}  // namespace specforge

#else

namespace specforge {

class DirectoryChangeGenerationMonitor::Impl {
public:
    explicit Impl(RegistrationFactory)
    {
    }

    [[nodiscard]] DirectoryChangeGenerationHandle Begin(
        const std::filesystem::path&,
        const std::function<void()>& cancellation_checkpoint,
        std::stop_token) const
    {
        cancellation_checkpoint();
        return {};
    }
};

DirectoryChangeGenerationMonitor::DirectoryChangeGenerationMonitor()
    : impl_(std::make_unique<Impl>(RegistrationFactory{}))
{
}

DirectoryChangeGenerationMonitor::DirectoryChangeGenerationMonitor(
    RegistrationFactory registration_factory)
    : impl_(std::make_unique<Impl>(std::move(registration_factory)))
{
}

DirectoryChangeGenerationMonitor::~DirectoryChangeGenerationMonitor() = default;

DirectoryChangeGenerationHandle DirectoryChangeGenerationMonitor::Begin(
    const std::filesystem::path& path,
    const std::function<void()>& cancellation_checkpoint,
    std::stop_token cancellation_token)
{
    return impl_->Begin(path, cancellation_checkpoint, cancellation_token);
}

}  // namespace specforge

#endif
