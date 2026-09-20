#include "platform/win32_external_open_router.h"
#include "app/project_identity.h"

#include "domain/source_path_identity.h"
#include "domain/stable_sha256.h"
#include "domain/uuid_v4.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace spectiary {
namespace {
constexpr wchar_t kProtocolProperty[] = L"ExternalOpen.Protocol";
constexpr wchar_t kRecencyProperty[] = L"ExternalOpen.Recency";
constexpr ULONG_PTR kProtocol = 1;
constexpr ULONG_PTR kMessageTag = 0x53454f31;
constexpr LONG kPending = 0, kAccepted = 1, kCanceled = 2;
constexpr std::size_t kMaxPath = 32768;

ULONG_PTR Recency() noexcept
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<ULONG_PTR>(now.QuadPart);
}

// Fixed-width wire layout, independent of the executable's build/version.
struct SharedRequest {
    std::uint32_t protocol;
    volatile LONG state;
    std::uint64_t deadline;
    std::uint32_t as_folder;
    std::uint32_t path_length;
    wchar_t path[kMaxPath];
};
static_assert(sizeof(wchar_t) == 2 && offsetof(SharedRequest, path) == 24);

struct Mapping {
    HANDLE handle = nullptr;
    SharedRequest* data = nullptr;
    ~Mapping() { if (data) UnmapViewOfFile(data); if (handle) CloseHandle(handle); }
    bool Map() {
        data = static_cast<SharedRequest*>(MapViewOfFile(handle,
            FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedRequest)));
        return data != nullptr;
    }
};

std::wstring NamespaceName(const std::filesystem::path& root)
{
    StableSha256 hash;
    hash.Append(SourcePathIdentityKey(root));
    const auto digest = hash.FinishHex();
    return project_identity::kExternalOpenNamespacePrefix + std::wstring(digest.begin(), digest.end());
}

HWND FindCompatibleInstance(const std::wstring& name, ULONGLONG deadline) noexcept
{
    HWND target = nullptr, current = nullptr;
    ULONG_PTR latest = 0;
    // Discovery cannot wait on another application's UI or enumerate files.
    for (unsigned count = 0; count < 256 && GetTickCount64() < deadline; ++count) {
        current = FindWindowExW(HWND_MESSAGE, current, name.c_str(), nullptr);
        if (!current) break;
        DWORD pid = 0;
        GetWindowThreadProcessId(current, &pid);
        if (!pid || pid == GetCurrentProcessId() ||
            reinterpret_cast<ULONG_PTR>(GetPropW(current, kProtocolProperty)) != kProtocol) continue;
        const auto recency = reinterpret_cast<ULONG_PTR>(GetPropW(current, kRecencyProperty));
        if (!target || recency > latest) { target = current; latest = recency; }
    }
    return target;
}
}

class Win32ExternalOpenRouter::Impl {
public:
    HWND endpoint = nullptr;
    std::wstring class_name;
    std::wstring mapping_prefix;
    std::function<bool()> activate;
    std::function<void()> wake;
    std::vector<ExternalOpenRequest> requests;
    bool receiving = false;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && message == WM_COPYDATA) {
            try { return self->Receive(reinterpret_cast<const COPYDATASTRUCT*>(lparam)); }
            catch (...) { return FALSE; } // Never unwind through a Windows callback.
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT Receive(const COPYDATASTRUCT* packet)
    {
        if (receiving) return FALSE;
        receiving = true;
        struct Reset { bool& value; ~Reset() { value = false; } } reset{receiving};
        if (!endpoint || !packet || packet->dwData != kMessageTag ||
            !packet->lpData || packet->cbData < sizeof(wchar_t) ||
            packet->cbData > 512 * sizeof(wchar_t) || packet->cbData % sizeof(wchar_t)) return FALSE;
        const auto* text = static_cast<const wchar_t*>(packet->lpData);
        const auto count = packet->cbData / sizeof(wchar_t);
        if (text[count - 1] != L'\0') return FALSE;
        const std::wstring name(text, count - 1);
        if (!name.starts_with(mapping_prefix) || name.find(L'\0') != std::wstring::npos) return FALSE;
        Mapping mapping;
        mapping.handle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
        if (!mapping.handle || !mapping.Map()) return FALSE;
        auto& data = *mapping.data;
        const auto length = data.path_length;
        const auto as_folder = data.as_folder;
        const auto deadline = data.deadline;
        if (data.protocol != kProtocol || length == 0 ||
            length >= kMaxPath || as_folder > 1 ||
            GetTickCount64() >= deadline ||
            InterlockedCompareExchange(&data.state, kPending, kPending) != kPending) return FALSE;
        const std::wstring path(data.path, length);
        if (path.find(L'\0') != std::wstring::npos || !std::filesystem::path(path).is_absolute()) return FALSE;
        // Bound queue growth even if many invocations arrive before the UI drains.
        if (requests.size() >= 64) return FALSE;
        requests.push_back({std::filesystem::path(path), as_folder != 0});
        // Focus is part of acceptance: refusal/timeout leaves the open untouched.
        bool focused = false;
        try { focused = activate && activate(); } catch (...) {}
        if (!focused || !endpoint || GetTickCount64() >= deadline ||
            InterlockedCompareExchange(&data.state, kAccepted, kPending) != kPending) {
            requests.pop_back();
            return FALSE;
        }
        SetPropW(endpoint, kRecencyProperty, reinterpret_cast<HANDLE>(Recency()));
        // Ownership has transferred even if the reply is lost. Wake is best
        // effort: the queued request is also drained every GUI pump iteration.
        try { if (wake) wake(); } catch (...) {}
        return TRUE;
    }
};

Win32ExternalOpenRouter::Win32ExternalOpenRouter() : impl_(std::make_unique<Impl>()) {}
Win32ExternalOpenRouter::~Win32ExternalOpenRouter() { Stop(); }

bool ActivateExternalOpenWindow(HWND window) noexcept
{
    if (!IsWindow(window)) return false;
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    return SetForegroundWindow(window) != FALSE;
}

bool Win32ExternalOpenRouter::Start(const std::filesystem::path& root,
    std::function<bool()> activate, std::function<void()> wake) noexcept
{
    try {
    Stop();
    impl_->class_name = NamespaceName(root);
    impl_->mapping_prefix = L"Local\\" + impl_->class_name + L".request.";
    impl_->activate = std::move(activate);
    impl_->wake = std::move(wake);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = Impl::WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = impl_->class_name.c_str();
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    impl_->endpoint = CreateWindowExW(0, impl_->class_name.c_str(), L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, window_class.hInstance, impl_.get());
    if (!impl_->endpoint) return false;
    if (!SetPropW(impl_->endpoint, kProtocolProperty, reinterpret_cast<HANDLE>(kProtocol))) {
        Stop(); return false;
    }
    MarkUsed();
    return true;
    } catch (...) {
        Stop();
        return false;
    }
}

void Win32ExternalOpenRouter::Stop() noexcept
{
    if (impl_->endpoint) {
        const HWND endpoint = std::exchange(impl_->endpoint, nullptr);
        RemovePropW(endpoint, kProtocolProperty);
        RemovePropW(endpoint, kRecencyProperty);
        DestroyWindow(endpoint);
    }
    if (!impl_->class_name.empty()) UnregisterClassW(impl_->class_name.c_str(), GetModuleHandleW(nullptr));
}

void Win32ExternalOpenRouter::MarkUsed() noexcept
{
    if (impl_->endpoint) SetPropW(impl_->endpoint, kRecencyProperty,
        reinterpret_cast<HANDLE>(Recency()));
}

std::vector<ExternalOpenRequest> Win32ExternalOpenRouter::TakeRequests()
{
    std::vector<ExternalOpenRequest> result;
    result.swap(impl_->requests);
    return result;
}

bool Win32ExternalOpenRouter::HasCompatibleInstance(const std::filesystem::path& root) noexcept
{
    try {
        return FindCompatibleInstance(NamespaceName(root), GetTickCount64() + 1500) != nullptr;
    } catch (...) { return false; }
}

bool Win32ExternalOpenRouter::Forward(const std::filesystem::path& root,
    const ExternalOpenRequest& request, unsigned timeout_ms) noexcept
{
    try {
        const auto deadline = GetTickCount64() + std::min(timeout_ms, 1500U);
        const auto name = NamespaceName(root);
        const HWND target = FindCompatibleInstance(name, deadline);
        if (!target || GetTickCount64() >= deadline) return false;
        const auto path = request.path.wstring();
        if (!request.path.is_absolute() || path.empty() || path.size() >= kMaxPath ||
            path.find(L'\0') != std::wstring::npos) return false;
        const auto uuid = GenerateUuidV4();
        if (!uuid) return false;
        const auto mapping_name = L"Local\\" + name + L".request." + std::wstring(uuid->begin(), uuid->end());
        Mapping mapping;
        mapping.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(SharedRequest), mapping_name.c_str());
        if (!mapping.handle || GetLastError() == ERROR_ALREADY_EXISTS || !mapping.Map()) return false;
        auto& data = *mapping.data;
        data.protocol = kProtocol;
        data.deadline = deadline;
        data.as_folder = request.as_folder ? 1 : 0;
        data.path_length = static_cast<std::uint32_t>(path.size());
        std::copy(path.begin(), path.end(), data.path);
        InterlockedExchange(&data.state, kPending);
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        if (pid) AllowSetForegroundWindow(pid);
        COPYDATASTRUCT packet{kMessageTag,
            static_cast<DWORD>((mapping_name.size() + 1) * sizeof(wchar_t)),
            const_cast<wchar_t*>(mapping_name.c_str())};
        const auto now = GetTickCount64();
        if (now >= deadline) return false;
        DWORD_PTR reply = 0;
        SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&packet),
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
            static_cast<UINT>(deadline - now), &reply);
        // A response timeout is not proof of rejection. This CAS settles the
        // race with receiver acceptance and prevents delayed duplicate opens.
        return InterlockedCompareExchange(&data.state, kCanceled, kPending) == kAccepted;
    } catch (...) { return false; }
}
} // namespace spectiary
