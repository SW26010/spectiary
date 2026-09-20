#include "platform/win32_jump_list.h"
#include "app/project_identity.h"
#include "platform/win32_process_launcher.h"
#include "platform/atomic_file.h"
#include "domain/source_path_identity.h"
#include "domain/stable_sha256.h"

#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <fstream>
#include <set>
#include <shellapi.h>

namespace spectiary {
using Microsoft::WRL::ComPtr;

namespace {
std::string SourceDigest(const std::filesystem::path& path)
{
    StableSha256 hash;
    auto normalized = path.lexically_normal();
    if (normalized != normalized.root_path() && normalized.filename().empty()) normalized = normalized.parent_path();
    hash.Append(SourcePathIdentityKey(normalized));
    return hash.FinishHex();
}

// Negative shell preferences only: these hashes can never supply a source path
// or add a Files entry. Windows clears its removal list after CommitList.
constexpr char kExclusionsHeader[] = "spectiary-jump-list-exclusions-v1";
bool ReadExclusions(const std::filesystem::path& path, std::set<std::string>& keys)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return !error;
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line) || line != kExclusionsHeader) return false;
    while (std::getline(input, line)) {
        if (line.size() != 64 || line.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
        keys.insert(line);
    }
    return input.eof() && !input.bad();
}
}

std::vector<JumpListSource> EligibleJumpListSources(std::span<const JumpListSource> sources, std::stop_token stop)
{
    std::vector<JumpListSource> result;
    for (const auto& source : sources) {
        if (stop.stop_requested()) break;
        if (!source.path.is_absolute() || source.path.native().find(L'\0') != std::wstring::npos) continue;
        std::error_code error;
        const auto status = std::filesystem::status(source.path, error);
        if (error || (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status))) continue;
        auto normalized = source.path.lexically_normal();
        if (normalized != normalized.root_path() && normalized.filename().empty()) normalized = normalized.parent_path();
        if (std::any_of(result.begin(), result.end(), [&](const auto& item) {
                return CompareStringOrdinal(item.path.c_str(), -1, normalized.c_str(), -1, TRUE) == CSTR_EQUAL;
            })) continue;
        result.push_back({normalized, source.title.empty() ? normalized.filename().wstring() : source.title});
        if (result.back().title.empty()) result.back().title = normalized.wstring();
    }
    return result;
}

HRESULT CreateSourceJumpListLink(const std::filesystem::path& executable,
    const JumpListSource& source, IShellLinkW** output)
{
    if (!output) return E_POINTER;
    *output = nullptr;
    ComPtr<IShellLinkW> link;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(result)) return result;
    if (FAILED(result = link->SetPath(executable.c_str()))) return result;
    if (FAILED(result = link->SetArguments(NewInstanceSourceArguments(source.path).c_str()))) return result;
    if (FAILED(result = link->SetIconLocation(executable.c_str(), 0))) return result;
    ComPtr<IPropertyStore> properties;
    if (FAILED(result = link.As(&properties))) return result;
    PROPVARIANT title{};
    result = InitPropVariantFromString(source.title.c_str(), &title);
    if (FAILED(result)) return result;
    result = properties->SetValue(PKEY_Title, title);
    PropVariantClear(&title);
    if (FAILED(result)) return result;
    if (FAILED(result = properties->Commit())) return result;
    return link.CopyTo(output);
}

HRESULT PublishSourceJumpList(const std::filesystem::path& executable,
    std::span<const JumpListSource> sources, const std::wstring& category,
    const std::filesystem::path& exclusions_path, ICustomDestinationList& destinations, std::stop_token stop)
{
    const auto cancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (stop.stop_requested()) return cancelled;
    // Serialize publication across our processes without assigning a shell identity.
    struct PublicationLock {
        HANDLE handle = nullptr;
        bool acquired = false;
        ~PublicationLock() { if (acquired) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
    } lock;
    lock.handle = CreateMutexW(nullptr, FALSE, project_identity::kJumpListPublicationMutex);
    if (!lock.handle) return HRESULT_FROM_WIN32(GetLastError());
    const DWORD wait = WaitForSingleObject(lock.handle, 3000);
    lock.acquired = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (!lock.acquired) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (stop.stop_requested()) return cancelled;
    std::set<std::string> exclusions;
    if (!ReadExclusions(exclusions_path, exclusions)) return E_FAIL;
    if (stop.stop_requested()) return cancelled;
    HRESULT result = S_OK;
    UINT slots = 0;
    ComPtr<IObjectArray> removed;
    if (FAILED(result = destinations.BeginList(&slots, IID_PPV_ARGS(&removed)))) return result;
    struct AbortUnlessCommitted {
        ICustomDestinationList* list;
        bool committed = false;
        ~AbortUnlessCommitted() { if (!committed) list->AbortList(); }
    } transaction{&destinations};
    if (stop.stop_requested()) return cancelled;
    const auto previous_exclusions = exclusions;
    UINT removed_count = 0;
    if (FAILED(result = removed->GetCount(&removed_count))) return result;
    for (UINT index = 0; index < removed_count; ++index) {
        if (stop.stop_requested()) return cancelled;
        ComPtr<IShellLinkW> link;
        if (SUCCEEDED(removed->GetAt(index, IID_PPV_ARGS(&link)))) {
            std::wstring arguments(32768, L'\0');
            if (FAILED(result = link->GetArguments(arguments.data(), static_cast<int>(arguments.size())))) return result;
            arguments.resize(wcslen(arguments.c_str()));
            int argc = 0;
            const auto command = L"Spectiary.exe " + arguments;
            auto argv = CommandLineToArgvW(command.c_str(), &argc);
            if (argv) {
                if (argc == 3 && std::wstring_view(argv[1]) == L"--new-instance" &&
                    std::filesystem::path(argv[2]).is_absolute()) {
                    exclusions.insert(SourceDigest(argv[2]));
                }
                LocalFree(argv);
            }
        }
    }
    ComPtr<IObjectCollection> collection;
    if (FAILED(result = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection)))) return result;
    UINT count = 0;
    for (const auto& source : EligibleJumpListSources(sources, stop)) {
        if (stop.stop_requested()) return cancelled;
        if (count >= slots) break;
        if (exclusions.contains(SourceDigest(source.path))) continue;
        ComPtr<IShellLinkW> link;
        if (FAILED(result = CreateSourceJumpListLink(executable, source, &link))) return result;
        if (FAILED(result = collection->AddObject(link.Get()))) return result;
        ++count;
    }
    if (stop.stop_requested()) return cancelled;
    if (count != 0) {
        result = destinations.AppendCategory(category.c_str(), collection.Get());
        // Windows privacy policy may disable destinations. Commit an empty list
        // to retire our previous category, while preserving shell-owned tasks.
        if (FAILED(result) && result != E_ACCESSDENIED) return result;
    }
    if (stop.stop_requested()) return cancelled;
    if (exclusions != previous_exclusions) {
        if (!WriteFileAtomically(exclusions_path, {}, [&](std::ostream& output, std::string&) {
                output << kExclusionsHeader << '\n';
                for (const auto& key : exclusions) output << key << '\n';
                return output.good();
            })) return E_FAIL;
    }
    if (stop.stop_requested()) return cancelled;
    result = destinations.CommitList();
    transaction.committed = SUCCEEDED(result);
    return result;
}

struct Win32JumpList::Impl {
    Impl(std::filesystem::path root, Publisher publish)
        : exclusions_path(std::move(root) / "shell-jump-list-exclusions.txt"), publisher(std::move(publish)) {}
    std::filesystem::path exclusions_path;
    Publisher publisher;
    struct Request { std::vector<JumpListSource> sources; std::wstring category; };
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::stop_source stop;

    void Stop()
    {
        stop.request_stop();
        { std::lock_guard lock(mutex); pending.reset(); }
        wake.notify_one();
    }
    void Run()
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return;
        const auto executable = ResolveCurrentExecutablePath();
        for (;;) {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stop.stop_requested() || pending.has_value(); });
            if (stop.stop_requested()) break;
            auto request = std::move(*pending);
            pending.reset();
            lock.unlock();
            if (executable.resolved()) {
                HRESULT result = E_FAIL;
                try {
                    if (stop.stop_requested()) break;
                    if (publisher) {
                        publisher(request.sources, request.category, stop.get_token());
                        result = S_OK;
                    } else {
                        ComPtr<ICustomDestinationList> destinations;
                        result = CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&destinations));
                        if (SUCCEEDED(result)) result = PublishSourceJumpList(executable.path, request.sources,
                            request.category, exclusions_path, *destinations.Get(), stop.get_token());
                    }
                } catch (...) {
                    // Shell projection failure must never terminate the GUI.
                }
                if (FAILED(result) && !stop.stop_requested()) OutputDebugStringW(L"Spectiary: Jump List refresh failed; source state is unchanged.\n");
            }
        }
        CoUninitialize();
    }
};

Win32JumpList::Win32JumpList(std::filesystem::path config_root, Publisher publisher)
    : impl_(std::make_shared<Impl>(std::move(config_root), std::move(publisher)))
{
    // Windows/COM I/O cannot be bounded by joining or by a stop request. The
    // worker owns its state and apartment until it returns; no GUI object is
    // captured, and process exit need not wait for an unavailable network path.
    std::thread([state = impl_] { state->Run(); }).detach();
}
Win32JumpList::~Win32JumpList() { impl_->Stop(); }
void Win32JumpList::Refresh(std::vector<JumpListSource> sources, std::wstring category)
{
    { std::lock_guard lock(impl_->mutex); impl_->pending = Impl::Request{std::move(sources), std::move(category)}; }
    impl_->wake.notify_one();
}
} // namespace spectiary
