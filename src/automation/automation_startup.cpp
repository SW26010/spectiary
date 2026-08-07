#include "automation/automation_startup.h"

#include "app/runtime_paths.h"
#include "platform/win32_file_identity.h"
#include "platform/win32_text.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <streambuf>
#include <string_view>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

constexpr std::wstring_view kAutomationPipeOption =
    L"--automation-pipe";
constexpr std::wstring_view kAutomationNonceOption =
    L"--automation-nonce";
constexpr std::wstring_view kAutomationInstanceOption =
    L"--automation-instance";
constexpr std::wstring_view kAutomationStateRootOption =
    L"--automation-state-root";
constexpr std::wstring_view
    kAutomationAllowPersistentLabelingOutputsOption =
        L"--automation-allow-persistent-labeling-outputs";
constexpr std::wstring_view
    kAutomationRootIdentityLockName =
        L".specforge-automation-root.lock";
constexpr std::array<std::wstring_view, 4>
    kIncompatibleAutomationEnvironmentVariables = {
        L"SPECFORGE_PROFILE",
        L"SPECFORGE_PROFILE_DIR",
        L"SPECFORGE_RUNTIME_RESOURCE_WORKLOAD",
        L"SPECFORGE_RUNTIME_RESOURCE_STATE_DIR",
    };

bool IsLowerHex(std::string_view value)
{
    return std::all_of(
        value.begin(),
        value.end(),
        [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
}

std::string NarrowAscii(std::wstring_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0 || character > 0x7f) {
            return {};
        }
        result.push_back(
            static_cast<char>(character));
    }
    return result;
}

std::optional<std::wstring> TakeOptionValue(
    std::span<const std::wstring> arguments,
    std::size_t& index,
    std::string& error_message)
{
    if (index + 1 >= arguments.size()) {
        error_message =
            "Automation startup option is missing its value.";
        return std::nullopt;
    }
    ++index;
    if (arguments[index].empty()) {
        error_message =
            "Automation startup option values must not be empty.";
        return std::nullopt;
    }
    return arguments[index];
}

std::filesystem::path NormalizedAbsolutePath(
    const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::weakly_canonical(
            path,
            error);
    if (!error) {
        return normalized;
    }
    error.clear();
    normalized =
        std::filesystem::absolute(path, error);
    return (error ? path : normalized)
        .lexically_normal();
}

bool PathComponentEqual(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    return Win32PathsEqualOrdinal(left, right);
}

bool PathContains(
    const std::filesystem::path& parent,
    const std::filesystem::path& child)
{
    const std::filesystem::path normalized_parent =
        NormalizedAbsolutePath(parent);
    const std::filesystem::path normalized_child =
        NormalizedAbsolutePath(child);
    auto parent_iterator = normalized_parent.begin();
    auto child_iterator = normalized_child.begin();
    for (; parent_iterator != normalized_parent.end();
         ++parent_iterator, ++child_iterator) {
        if (child_iterator == normalized_child.end() ||
            !PathComponentEqual(
                *parent_iterator,
                *child_iterator)) {
            return false;
        }
    }
    return true;
}

bool PathHasExistingReparsePoint(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate_parent)
{
    const std::filesystem::path normalized_root =
        std::filesystem::absolute(root)
            .lexically_normal();
    const std::filesystem::path normalized_parent =
        std::filesystem::absolute(candidate_parent)
            .lexically_normal();
    if (!PathContains(
            normalized_root,
            normalized_parent)) {
        return true;
    }

    std::filesystem::path current;
    for (const auto& component :
         normalized_parent) {
        current /= component;
        if (!PathContains(normalized_root, current) &&
            !PathContains(current, normalized_root)) {
            continue;
        }
        const DWORD attributes =
            GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes &
             FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return true;
        }
    }
    return false;
}

class ScopedWin32Handle {
public:
    ScopedWin32Handle() = default;
    explicit ScopedWin32Handle(HANDLE handle)
        : handle_(handle)
    {
    }
    ~ScopedWin32Handle()
    {
        Reset();
    }
    ScopedWin32Handle(ScopedWin32Handle&& other) noexcept
        : handle_(std::exchange(
              other.handle_,
              INVALID_HANDLE_VALUE))
    {
    }
    ScopedWin32Handle& operator=(
        ScopedWin32Handle&& other) noexcept
    {
        if (this != &other) {
            Reset(std::exchange(
                other.handle_,
                INVALID_HANDLE_VALUE));
        }
        return *this;
    }
    ScopedWin32Handle(const ScopedWin32Handle&) = delete;
    ScopedWin32Handle& operator=(const ScopedWin32Handle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept
    {
        return handle_;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != nullptr &&
               handle_ != INVALID_HANDLE_VALUE;
    }
    void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (*this) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class Win32HandleStreamBuffer final : public std::streambuf {
public:
    explicit Win32HandleStreamBuffer(HANDLE handle)
        : handle_(handle)
    {
        setp(buffer_.data(),
             buffer_.data() + buffer_.size());
    }

    ~Win32HandleStreamBuffer() override
    {
        (void)sync();
    }

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
            const auto remaining =
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
                setp(buffer_.data(),
                     buffer_.data() + buffer_.size());
                return false;
            }
            offset += written;
        }
        setp(buffer_.data(),
             buffer_.data() + buffer_.size());
        return true;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::array<char, 64U * 1024U> buffer_ = {};
    bool failed_ = false;
};

struct Win32ProfileOutputHandleState {
    Win32ProfileOutputHandleState(
        ScopedWin32Handle file_handle,
        ScopedWin32Handle root_handle,
        std::vector<ScopedWin32Handle> directory_handles)
        : root(std::move(root_handle)),
          directories(std::move(directory_handles)),
          file(std::move(file_handle))
    {
    }

    ScopedWin32Handle root;
    std::vector<ScopedWin32Handle> directories;
    ScopedWin32Handle file;
};

class Win32HandleOutputStream final : public std::ostream {
public:
    Win32HandleOutputStream(
        std::shared_ptr<Win32ProfileOutputHandleState> handles)
        : std::ostream(nullptr),
          handles_(std::move(handles)),
          buffer_(handles_->file.get())
    {
        rdbuf(&buffer_);
        clear();
    }

private:
    std::shared_ptr<Win32ProfileOutputHandleState>
        handles_;
    Win32HandleStreamBuffer buffer_;
};

std::wstring ProfileOutputFileName()
{
    SYSTEMTIME utc = {};
    GetSystemTime(&utc);
    static std::atomic_uint32_t sequence = 0;
    std::wostringstream name;
    name << L"specforge-profile-"
         << std::setfill(L'0')
         << std::setw(4) << utc.wYear
         << std::setw(2) << utc.wMonth
         << std::setw(2) << utc.wDay
         << L'-'
         << std::setw(2) << utc.wHour
         << std::setw(2) << utc.wMinute
         << std::setw(2) << utc.wSecond
         << L'-'
         << std::setw(3) << utc.wMilliseconds
         << L'-' << GetCurrentProcessId()
         << L'-' << ++sequence
         << L".jsonl";
    return name.str();
}

bool AppendRelativeComponents(
    const std::filesystem::path& root,
    const std::filesystem::path& child,
    std::vector<std::filesystem::path>& components)
{
    auto root_component = root.begin();
    auto child_component = child.begin();
    for (;
         root_component != root.end();
         ++root_component, ++child_component) {
        if (child_component == child.end() ||
            !Win32PathsEqualOrdinal(
                *root_component,
                *child_component)) {
            return false;
        }
    }
    for (;
         child_component != child.end();
         ++child_component) {
        if (child_component->empty() ||
            *child_component == L"." ||
            *child_component == L"..") {
            return false;
        }
        components.push_back(*child_component);
    }
    return !components.empty();
}

template <typename T>
bool SetOnce(
    std::optional<T>& target,
    T value,
    std::string_view option_name,
    std::string& error_message)
{
    if (target) {
        error_message =
            "Automation startup option '" +
            std::string(option_name) +
            "' was provided more than once.";
        return false;
    }
    target = std::move(value);
    return true;
}

}  // namespace

AutomationPreparedProfileOutput
AutomationProfileOutputFactory::Create(
    const std::filesystem::path& automation_root,
    const std::filesystem::path& output_directory)
{
    return CreateWithOpenCheckpoint(
        automation_root,
        output_directory,
        {});
}

AutomationPreparedProfileOutput
AutomationProfileOutputFactory::CreateWithOpenCheckpoint(
    const std::filesystem::path& automation_root,
    const std::filesystem::path& output_directory,
    BeforeOpenCheckpoint before_open)
{
    const auto failure =
        [](std::string message) {
            return AutomationPreparedProfileOutput{
                .error_message = std::move(message),
            };
        };
    const AutomationStateOwnedPathValidation validation =
        ValidateAutomationStateOwnedPath(
            automation_root,
            output_directory);
    const std::filesystem::path normalized_root =
        Win32FullPath(automation_root);
    if (!validation.valid ||
        normalized_root.empty()) {
        return failure(
            "The profile output directory is outside the isolated automation state root.");
    }

    std::vector<std::filesystem::path> components;
    if (!AppendRelativeComponents(
            normalized_root,
            validation.normalized_path,
            components)) {
        return failure(
            "The profile output directory is not a strict child of the isolated automation state root.");
    }
    const std::filesystem::path final_path =
        validation.normalized_path /
        ProfileOutputFileName();
    if (before_open) {
        before_open(final_path);
    }

    ScopedWin32Handle root(CreateFileW(
        normalized_root.c_str(),
        FILE_LIST_DIRECTORY |
            FILE_ADD_FILE |
            FILE_ADD_SUBDIRECTORY |
            FILE_TRAVERSE |
            FILE_READ_ATTRIBUTES |
            SYNCHRONIZE,
        FILE_SHARE_READ |
            FILE_SHARE_WRITE |
            FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS |
            FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!root ||
        FAILED(ValidateWin32DirectoryHandleNoFollow(root.get())) ||
        !Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(root.get()),
            normalized_root)) {
        return failure(
            "The automation state-root identity changed before profile creation.");
    }

    std::vector<ScopedWin32Handle> directories;
    directories.reserve(components.size());
    HANDLE parent = root.get();
    std::filesystem::path expected_parent =
        normalized_root;
    for (const std::filesystem::path& component :
         components) {
        const HANDLE opened =
            OpenWin32Relative(
                parent,
                component.wstring(),
                FILE_LIST_DIRECTORY |
                    FILE_ADD_FILE |
                    FILE_ADD_SUBDIRECTORY |
                    FILE_TRAVERSE |
                    FILE_READ_ATTRIBUTES |
                    SYNCHRONIZE,
                FILE_SHARE_READ |
                    FILE_SHARE_WRITE,
                FILE_OPEN_IF,
                FILE_DIRECTORY_FILE |
                    FILE_OPEN_REPARSE_POINT |
                    FILE_SYNCHRONOUS_IO_NONALERT,
                FILE_ATTRIBUTE_NORMAL);
        ScopedWin32Handle directory(opened);
        expected_parent /= component;
        if (!directory ||
            FAILED(
                ValidateWin32DirectoryHandleNoFollow(
                    directory.get())) ||
            !Win32PathsEqualOrdinal(
                Win32FinalPathByHandle(
                    directory.get()),
                expected_parent)) {
            return failure(
                "The profile output directory changed identity or traversed a reparse point.");
        }
        parent = directory.get();
        directories.push_back(
            std::move(directory));
    }

    if (!Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(root.get()),
            normalized_root) ||
        !Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(parent),
            validation.normalized_path)) {
        return failure(
            "The pinned profile output boundary changed before final file creation.");
    }

    ScopedWin32Handle file(
        OpenWin32Relative(
            parent,
            final_path.filename().wstring(),
            GENERIC_WRITE |
                DELETE |
                FILE_READ_ATTRIBUTES |
                SYNCHRONIZE,
            FILE_SHARE_READ,
            FILE_CREATE,
            FILE_NON_DIRECTORY_FILE |
                FILE_OPEN_REPARSE_POINT |
                FILE_SYNCHRONOUS_IO_NONALERT,
            FILE_ATTRIBUTE_NORMAL));
    if (!file ||
        FAILED(
            ValidateWin32RegularFileHandleNoFollow(
                file.get()))) {
        return failure(
            "Could not CREATE_NEW the isolated profile output file without following reparse points.");
    }

    const bool identity_valid =
        Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(root.get()),
            normalized_root) &&
        Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(parent),
            validation.normalized_path) &&
        Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(file.get()),
            final_path);
    if (!identity_valid) {
        (void)MarkWin32HandleForDeletion(file.get());
        return failure(
            "The final profile file identity escaped its pinned automation directory.");
    }

    AutomationPreparedProfileOutput result;
    std::shared_ptr<Win32ProfileOutputHandleState>
        handle_state;
    try {
        result.path = final_path;
        handle_state =
            std::make_shared<
                Win32ProfileOutputHandleState>(
                std::move(file),
                std::move(root),
                std::move(directories));
        result.stream =
            std::make_unique<Win32HandleOutputStream>(
                handle_state);
        result.discard_output =
            [handle_state]() noexcept {
                if (handle_state->file) {
                    (void)MarkWin32HandleForDeletion(
                        handle_state->file.get());
                }
            };
        return result;
    } catch (const std::bad_alloc&) {
        if (handle_state && handle_state->file) {
            (void)MarkWin32HandleForDeletion(
                handle_state->file.get());
        } else if (file) {
            (void)MarkWin32HandleForDeletion(
                file.get());
        }
        return failure(
            "Could not allocate the isolated profile output stream.");
    }
}

bool IsValidAutomationInstanceId(
    std::string_view value) noexcept
{
    return value.size() == 32U && IsLowerHex(value);
}

bool IsValidAutomationNonce(
    std::string_view value) noexcept
{
    return value.size() == 64U && IsLowerHex(value);
}

std::wstring AutomationPipeNameForInstance(
    std::string_view instance_id)
{
    const std::wstring wide_instance =
        Utf8ToWide(instance_id);
    return L"\\\\.\\pipe\\SpecForge.Automation." +
           wide_instance;
}

bool IsIncompatibleAutomationEnvironmentVariable(
    std::wstring_view name) noexcept
{
    return std::any_of(
        kIncompatibleAutomationEnvironmentVariables.begin(),
        kIncompatibleAutomationEnvironmentVariables.end(),
        [name](std::wstring_view candidate) {
            return CompareStringOrdinal(
                       name.data(),
                       static_cast<int>(name.size()),
                       candidate.data(),
                       static_cast<int>(candidate.size()),
                       TRUE) == CSTR_EQUAL;
        });
}

std::optional<std::wstring>
ActiveIncompatibleAutomationEnvironmentVariable()
{
    for (const std::wstring_view name :
         kIncompatibleAutomationEnvironmentVariables) {
        if (GetEnvironmentVariableW(
                std::wstring(name).c_str(),
                nullptr,
                0) != 0) {
            return std::wstring(name);
        }
    }
    return std::nullopt;
}

std::filesystem::path
OrdinaryUserStateRootForExecutable(
    const std::filesystem::path& executable_path)
{
    RuntimePathInputs inputs =
        CurrentProcessRuntimePathInputs(
            executable_path);
    inputs.local_user_state_root_override.reset();
    const SpecForgeStartup startup =
        PrepareSpecForgeStartup(
            std::move(inputs));
    return startup.runtime_paths()
        .local_user_state_root;
}

bool AutomationStateRootIsIndependent(
    const std::filesystem::path& automation_root,
    const std::filesystem::path& ordinary_root)
{
    return !PathContains(
               automation_root,
               ordinary_root) &&
           !PathContains(
               ordinary_root,
               automation_root);
}

std::optional<bool> PathEntryExists(
    const std::filesystem::path& path)
{
    WIN32_FIND_DATAW data = {};
    const HANDLE found =
        FindFirstFileW(path.c_str(), &data);
    if (found != INVALID_HANDLE_VALUE) {
        FindClose(found);
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND ||
        error == ERROR_PATH_NOT_FOUND) {
        return false;
    }
    return std::nullopt;
}

AutomationStateRootLease::~AutomationStateRootLease()
{
    Close();
}

AutomationStateRootLease::
AutomationStateRootLease(
    AutomationStateRootLease&& other) noexcept
    : path_(std::move(other.path_)),
      parent_handle_(std::exchange(
          other.parent_handle_,
          nullptr)),
      root_handle_(std::exchange(
          other.root_handle_,
          nullptr)),
      identity_lock_handle_(std::exchange(
          other.identity_lock_handle_,
          nullptr)),
      materialized_entries_(
          std::move(
              other.materialized_entries_))
{
}

AutomationStateRootLease&
AutomationStateRootLease::operator=(
    AutomationStateRootLease&& other) noexcept
{
    if (this != &other) {
        Close();
        path_ = std::move(other.path_);
        parent_handle_ = std::exchange(
            other.parent_handle_,
            nullptr);
        root_handle_ = std::exchange(
            other.root_handle_,
            nullptr);
        identity_lock_handle_ =
            std::exchange(
                other.identity_lock_handle_,
                nullptr);
        materialized_entries_ =
            std::move(
                other.materialized_entries_);
    }
    return *this;
}

bool AutomationStateRootLease::valid() const noexcept
{
    return root_handle_ != nullptr;
}

const std::filesystem::path&
AutomationStateRootLease::path() const noexcept
{
    return path_;
}

void AutomationStateRootLease::Close() noexcept
{
    if (identity_lock_handle_ != nullptr) {
        CloseHandle(
            static_cast<HANDLE>(
                identity_lock_handle_));
        identity_lock_handle_ = nullptr;
    }
    if (root_handle_ != nullptr) {
        CloseHandle(
            static_cast<HANDLE>(
                root_handle_));
        root_handle_ = nullptr;
    }
    if (parent_handle_ != nullptr) {
        CloseHandle(
            static_cast<HANDLE>(
                parent_handle_));
        parent_handle_ = nullptr;
    }
}

AutomationReadOnlyFileLease::
~AutomationReadOnlyFileLease()
{
    Close();
}

AutomationReadOnlyFileLease::
AutomationReadOnlyFileLease(
    AutomationReadOnlyFileLease&& other) noexcept
    : path_(std::move(other.path_)),
      handle_(std::exchange(
          other.handle_,
          nullptr))
{
}

AutomationReadOnlyFileLease&
AutomationReadOnlyFileLease::operator=(
    AutomationReadOnlyFileLease&& other) noexcept
{
    if (this != &other) {
        Close();
        path_ = std::move(other.path_);
        handle_ = std::exchange(
            other.handle_,
            nullptr);
    }
    return *this;
}

bool AutomationReadOnlyFileLease::valid() const noexcept
{
    return handle_ != nullptr;
}

const std::filesystem::path&
AutomationReadOnlyFileLease::path() const noexcept
{
    return path_;
}

void AutomationReadOnlyFileLease::Close() noexcept
{
    if (handle_ != nullptr) {
        CloseHandle(
            static_cast<HANDLE>(
                handle_));
        handle_ = nullptr;
    }
}

AutomationStateRootLease
CreatePinnedAutomationStateRoot(
    const std::filesystem::path& path,
    std::string& error_message)
{
    AutomationStateRootLease lease;
    const std::filesystem::path normalized =
        Win32FullPath(path);
    const std::filesystem::path parent =
        normalized.parent_path();
    const std::filesystem::path filename =
        normalized.filename();
    if (normalized.empty() ||
        !normalized.is_absolute() ||
        parent.empty() ||
        filename.empty() ||
        filename == L"." ||
        filename == L"..") {
        error_message =
            "Automation state root is not a valid absolute directory path.";
        return lease;
    }
    const std::optional<bool> root_exists =
        PathEntryExists(normalized);
    if (!root_exists || *root_exists) {
        error_message =
            root_exists.value_or(false)
            ? "Automation state root must not already exist."
            : "Automation state root could not be inspected.";
        return lease;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(
        parent,
        directory_error);
    if (directory_error) {
        error_message =
            "Could not create the automation state-root parent directory.";
        return lease;
    }

    const HANDLE parent_handle =
        CreateFileW(
            parent.c_str(),
            FILE_LIST_DIRECTORY |
                FILE_ADD_SUBDIRECTORY |
                FILE_READ_ATTRIBUTES |
                SYNCHRONIZE,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
    if (parent_handle == INVALID_HANDLE_VALUE ||
        FAILED(
            ValidateWin32DirectoryHandleNoFollow(
                parent_handle)) ||
        !Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(
                parent_handle),
            parent)) {
        if (parent_handle !=
            INVALID_HANDLE_VALUE) {
            CloseHandle(parent_handle);
        }
        error_message =
            "Automation state-root parent identity changed or traversed a reparse point.";
        return lease;
    }

    const HANDLE root_handle =
        OpenWin32Relative(
            parent_handle,
            filename.wstring(),
            FILE_LIST_DIRECTORY |
                FILE_ADD_FILE |
                FILE_ADD_SUBDIRECTORY |
                FILE_TRAVERSE |
                FILE_READ_ATTRIBUTES |
                DELETE |
                SYNCHRONIZE,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE,
            FILE_CREATE,
            FILE_DIRECTORY_FILE |
                FILE_OPEN_REPARSE_POINT |
                FILE_SYNCHRONOUS_IO_NONALERT,
            FILE_ATTRIBUTE_NORMAL);
    if (root_handle == INVALID_HANDLE_VALUE ||
        FAILED(
            ValidateWin32DirectoryHandleNoFollow(
                root_handle)) ||
        !Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(
                root_handle),
            normalized)) {
        if (root_handle !=
            INVALID_HANDLE_VALUE) {
            (void)MarkWin32HandleForDeletion(
                root_handle);
            CloseHandle(root_handle);
        }
        CloseHandle(parent_handle);
        error_message =
            "Could not create and pin the automation state root.";
        return lease;
    }

    const HANDLE identity_lock =
        OpenWin32Relative(
            root_handle,
            kAutomationRootIdentityLockName,
            DELETE |
                FILE_READ_ATTRIBUTES |
                SYNCHRONIZE,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            FILE_CREATE,
            FILE_NON_DIRECTORY_FILE |
                FILE_DELETE_ON_CLOSE |
                FILE_OPEN_REPARSE_POINT |
                FILE_SYNCHRONOUS_IO_NONALERT,
            FILE_ATTRIBUTE_HIDDEN |
                FILE_ATTRIBUTE_TEMPORARY);
    if (identity_lock ==
        INVALID_HANDLE_VALUE) {
        (void)MarkWin32HandleForDeletion(
            root_handle);
        CloseHandle(root_handle);
        CloseHandle(parent_handle);
        error_message =
            "Could not create the automation state-root identity lock.";
        return lease;
    }

    if (!Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(
                root_handle),
            normalized)) {
        CloseHandle(identity_lock);
        (void)MarkWin32HandleForDeletion(
            root_handle);
        CloseHandle(root_handle);
        CloseHandle(parent_handle);
        error_message =
            "Automation state-root identity changed while acquiring its persistent lease.";
        return lease;
    }
    const HANDLE operational_root =
        OpenWin32Relative(
            parent_handle,
            filename.wstring(),
            FILE_LIST_DIRECTORY |
                FILE_ADD_FILE |
                FILE_ADD_SUBDIRECTORY |
                FILE_TRAVERSE |
                FILE_READ_ATTRIBUTES |
                SYNCHRONIZE,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            FILE_OPEN,
            FILE_DIRECTORY_FILE |
                FILE_OPEN_REPARSE_POINT |
                FILE_SYNCHRONOUS_IO_NONALERT,
            FILE_ATTRIBUTE_NORMAL);
    if (operational_root ==
            INVALID_HANDLE_VALUE ||
        FAILED(
            ValidateWin32DirectoryHandleNoFollow(
                operational_root)) ||
        !Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(
                operational_root),
            normalized)) {
        if (operational_root !=
            INVALID_HANDLE_VALUE) {
            CloseHandle(operational_root);
        }
        CloseHandle(identity_lock);
        (void)MarkWin32HandleForDeletion(
            root_handle);
        CloseHandle(root_handle);
        CloseHandle(parent_handle);
        error_message =
            "Could not hand the automation state root from creation to its persistent identity lease.";
        return lease;
    }
    CloseHandle(root_handle);

    lease.path_ = normalized;
    lease.parent_handle_ = parent_handle;
    lease.root_handle_ = operational_root;
    lease.identity_lock_handle_ =
        identity_lock;
    return lease;
}

AutomationReadOnlyFileLease
PinAutomationReadOnlyFile(
    const std::filesystem::path& path,
    std::string& error_message)
{
    AutomationReadOnlyFileLease lease;
    const std::filesystem::path normalized =
        Win32FullPath(path);
    if (normalized.empty() ||
        !normalized.is_absolute()) {
        error_message =
            "Automation seed must be a valid absolute file path.";
        return lease;
    }
    const HANDLE handle =
        CreateFileW(
            normalized.c_str(),
            GENERIC_READ |
                FILE_READ_ATTRIBUTES |
                SYNCHRONIZE,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL |
                FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
    if (handle == INVALID_HANDLE_VALUE ||
        FAILED(
            ValidateWin32RegularFileHandleNoFollow(
                handle)) ||
        !Win32PathsEqualOrdinal(
            Win32FinalPathByHandle(handle),
            normalized)) {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        error_message =
            "Automation seed must be a pinned regular file without reparse points.";
        return lease;
    }
    lease.path_ = normalized;
    lease.handle_ = handle;
    return lease;
}

bool MaterializePinnedAutomationSeed(
    AutomationStateRootLease& root,
    const AutomationReadOnlyFileLease& seed,
    std::wstring_view destination_name,
    std::string& error_message)
{
    const std::filesystem::path relative(
        destination_name);
    if (!root.valid() ||
        !seed.valid() ||
        destination_name.empty() ||
        relative.filename() != relative ||
        relative == L"." ||
        relative == L"..") {
        error_message =
            "Pinned automation seed materialization arguments are invalid.";
        return false;
    }
    const HANDLE output =
        OpenWin32Relative(
            static_cast<HANDLE>(
                root.root_handle_),
            destination_name,
            GENERIC_WRITE |
                DELETE |
                SYNCHRONIZE |
                FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            FILE_CREATE,
            FILE_NON_DIRECTORY_FILE |
                FILE_OPEN_REPARSE_POINT |
                FILE_SYNCHRONOUS_IO_NONALERT,
            FILE_ATTRIBUTE_NORMAL);
    if (output == INVALID_HANDLE_VALUE ||
        FAILED(
            ValidateWin32RegularFileHandleNoFollow(
                output))) {
        if (output != INVALID_HANDLE_VALUE) {
            (void)MarkWin32HandleForDeletion(
                output);
            CloseHandle(output);
        }
        error_message =
            "Could not create the pinned automation seed inside the automation root.";
        return false;
    }

    LARGE_INTEGER beginning = {};
    bool copied =
        SetFilePointerEx(
            static_cast<HANDLE>(
                seed.handle_),
            beginning,
            nullptr,
            FILE_BEGIN) != FALSE;
    std::array<std::byte, 64U * 1024U>
        buffer = {};
    while (copied) {
        DWORD read = 0;
        if (ReadFile(
                static_cast<HANDLE>(
                    seed.handle_),
                buffer.data(),
                static_cast<DWORD>(
                    buffer.size()),
                &read,
                nullptr) == FALSE) {
            copied = false;
            break;
        }
        if (read == 0) {
            break;
        }
        DWORD offset = 0;
        while (offset < read) {
            DWORD written = 0;
            if (WriteFile(
                    output,
                    buffer.data() + offset,
                    read - offset,
                    &written,
                    nullptr) == FALSE ||
                written == 0) {
                copied = false;
                break;
            }
            offset += written;
        }
    }
    if (copied) {
        copied =
            FlushFileBuffers(output) != FALSE;
    }
    if (!copied) {
        (void)MarkWin32HandleForDeletion(
            output);
        CloseHandle(output);
        error_message =
            "Could not copy the pinned automation seed into the automation root.";
        return false;
    }
    CloseHandle(output);
    root.materialized_entries_.emplace_back(
        destination_name);
    return true;
}

void RemovePinnedAutomationStateRootBeforeLaunch(
    AutomationStateRootLease& root) noexcept
{
    if (!root.valid()) {
        return;
    }
    for (const std::wstring& entry :
         root.materialized_entries_) {
        const HANDLE file =
            OpenWin32Relative(
                static_cast<HANDLE>(
                    root.root_handle_),
                entry,
                DELETE |
                    SYNCHRONIZE,
                FILE_SHARE_READ |
                    FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE,
                FILE_OPEN,
                FILE_NON_DIRECTORY_FILE |
                    FILE_OPEN_REPARSE_POINT |
                    FILE_SYNCHRONOUS_IO_NONALERT,
                FILE_ATTRIBUTE_NORMAL);
        if (file != INVALID_HANDLE_VALUE) {
            (void)MarkWin32HandleForDeletion(
                file);
            CloseHandle(file);
        }
    }
    root.materialized_entries_.clear();
    if (root.identity_lock_handle_ != nullptr) {
        CloseHandle(
            static_cast<HANDLE>(
                root.identity_lock_handle_));
        root.identity_lock_handle_ =
            nullptr;
    }
    root.Close();
    (void)RemoveDirectoryW(
        root.path_.c_str());
}

AutomationStateOwnedPathValidation
ValidateAutomationStateOwnedPath(
    const std::filesystem::path& automation_root,
    const std::filesystem::path& candidate_path)
{
    const auto failure =
        [](std::string message) {
            return AutomationStateOwnedPathValidation{
                .error_message =
                    std::move(message),
            };
        };
    if (!candidate_path.is_absolute()) {
        return failure(
            "The path must be absolute.");
    }

    const std::filesystem::path normalized_root =
        NormalizedAbsolutePath(automation_root);
    const std::filesystem::path normalized_candidate =
        NormalizedAbsolutePath(candidate_path);
    if (PathComponentEqual(
            normalized_root,
            normalized_candidate) ||
        !PathContains(
            normalized_root,
            normalized_candidate) ||
        PathHasExistingReparsePoint(
            normalized_root,
            candidate_path.parent_path())) {
        return failure(
            "The path must remain strictly below the automation state root without reparse points.");
    }
    return {
        .valid = true,
        .normalized_path =
            normalized_candidate,
    };
}

AutomationCapturePathValidation
ValidateAutomationCapturePath(
    const std::filesystem::path& automation_root,
    const std::filesystem::path& output_path)
{
    const auto failure =
        [](std::string code,
           std::string message) {
            return AutomationCapturePathValidation{
                .error_code = std::move(code),
                .error_message =
                    std::move(message),
            };
        };
    const AutomationStateOwnedPathValidation
        owned_path =
            ValidateAutomationStateOwnedPath(
                automation_root,
                output_path);
    if (!output_path.is_absolute()) {
        return failure(
            "capture_path_not_absolute",
            "frame.capture path must be absolute.");
    }
    std::wstring extension =
        output_path.extension().wstring();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(
                std::towlower(character));
        });
    if (extension != L".png") {
        return failure(
            "capture_path_invalid",
            "frame.capture path must use the .png extension.");
    }

    if (!owned_path.valid) {
        return failure(
            "capture_path_outside_state_root",
            "frame.capture path must remain below the automation state root without reparse points.");
    }

    const std::filesystem::path& normalized_output =
        owned_path.normalized_path;
    const std::optional<bool> output_exists =
        PathEntryExists(normalized_output);
    if (output_exists.value_or(false)) {
        return failure(
            "capture_output_exists",
            "frame.capture does not overwrite an existing output.");
    }
    if (!output_exists) {
        return failure(
            "capture_path_invalid",
            "frame.capture path could not be inspected.");
    }
    return {
        .valid = true,
        .normalized_path = normalized_output,
    };
}

SpecForgeCommandLine ParseSpecForgeCommandLine(
    std::span<const std::wstring> arguments)
{
    SpecForgeCommandLine result;
    std::optional<std::wstring> pipe_name;
    std::optional<std::wstring> nonce;
    std::optional<std::wstring> instance_id;
    std::optional<std::filesystem::path> state_root;
    bool allow_persistent_labeling_outputs = false;

    for (std::size_t index = 1;
         index < arguments.size();
         ++index) {
        const std::wstring_view argument =
            arguments[index];
        if (argument ==
            kAutomationAllowPersistentLabelingOutputsOption) {
            result.automation_requested = true;
            if (allow_persistent_labeling_outputs) {
                result.error_message =
                    "Automation startup option '--automation-allow-persistent-labeling-outputs' may not be repeated.";
                return result;
            }
            allow_persistent_labeling_outputs = true;
            continue;
        }
        const bool automation_option =
            argument == kAutomationPipeOption ||
            argument == kAutomationNonceOption ||
            argument == kAutomationInstanceOption ||
            argument == kAutomationStateRootOption;
        if (automation_option) {
            result.automation_requested = true;
            std::optional<std::wstring> value =
                TakeOptionValue(
                    arguments,
                    index,
                    result.error_message);
            if (!value) {
                return result;
            }
            if (argument == kAutomationPipeOption) {
                if (!SetOnce(
                        pipe_name,
                        std::move(*value),
                        "--automation-pipe",
                        result.error_message)) {
                    return result;
                }
            } else if (argument ==
                       kAutomationNonceOption) {
                if (!SetOnce(
                        nonce,
                        std::move(*value),
                        "--automation-nonce",
                        result.error_message)) {
                    return result;
                }
            } else if (argument ==
                       kAutomationInstanceOption) {
                if (!SetOnce(
                        instance_id,
                        std::move(*value),
                        "--automation-instance",
                        result.error_message)) {
                    return result;
                }
            } else {
                const std::filesystem::path path(*value);
                if (!SetOnce(
                        state_root,
                        path,
                        "--automation-state-root",
                        result.error_message)) {
                    return result;
                }
            }
            continue;
        }

        if (!result.initial_source &&
            !argument.empty() &&
            argument.front() != L'-') {
            result.initial_source =
                std::filesystem::path(argument);
        }
    }

    if (!result.automation_requested) {
        return result;
    }
    if (!pipe_name || !nonce || !instance_id ||
        !state_root) {
        result.error_message =
            "Automation startup requires --automation-pipe, --automation-nonce, --automation-instance and --automation-state-root.";
        return result;
    }

    const std::string nonce_utf8 =
        NarrowAscii(*nonce);
    const std::string instance_utf8 =
        NarrowAscii(*instance_id);
    if (!IsValidAutomationNonce(nonce_utf8)) {
        result.error_message =
            "Automation nonce must be 64 lowercase hexadecimal characters.";
        return result;
    }
    if (!IsValidAutomationInstanceId(instance_utf8)) {
        result.error_message =
            "Automation instance ID must be 32 lowercase hexadecimal characters.";
        return result;
    }
    if (*pipe_name !=
        AutomationPipeNameForInstance(instance_utf8)) {
        result.error_message =
            "Automation pipe name does not match the launcher instance ID.";
        return result;
    }
    if (!state_root->is_absolute()) {
        result.error_message =
            "Automation state root must be an absolute path.";
        return result;
    }
    std::error_code state_root_error;
    if (!std::filesystem::is_directory(
            *state_root,
            state_root_error) ||
        state_root_error) {
        result.error_message =
            "Automation state root must already exist as a directory created by the launcher.";
        return result;
    }

    result.automation = AutomationStartupConfiguration{
        .pipe_name = std::move(*pipe_name),
        .nonce = nonce_utf8,
        .instance_id = instance_utf8,
        .state_root = std::move(*state_root),
        .allow_persistent_labeling_outputs =
            allow_persistent_labeling_outputs,
    };
    return result;
}

SpecForgeCommandLine
ParseCurrentProcessSpecForgeCommandLine()
{
    int argument_count = 0;
    LPWSTR* raw_arguments =
        CommandLineToArgvW(
            GetCommandLineW(),
            &argument_count);
    if (raw_arguments == nullptr) {
        return {
            .error_message =
                "Could not parse the SpecForge process command line.",
        };
    }

    std::vector<std::wstring> arguments;
    arguments.reserve(
        static_cast<std::size_t>(argument_count));
    for (int index = 0;
         index < argument_count;
         ++index) {
        arguments.emplace_back(
            raw_arguments[index] != nullptr
                ? raw_arguments[index]
                : L"");
    }
    LocalFree(raw_arguments);
    return ParseSpecForgeCommandLine(arguments);
}

}  // namespace specforge
