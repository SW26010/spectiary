#include "platform/win32_shell_identity.h"
#include "platform/win32_process_launcher.h"
#include "app/project_identity.h"
#include "domain/source_path_identity.h"
#include "domain/stable_sha256.h"
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

namespace spectiary {
std::wstring ShellAppUserModelId(const std::filesystem::path& config_root)
{
    StableSha256 hash;
    hash.Append(project_identity::kApplicationId);
    auto normalized = config_root.lexically_normal();
    if (normalized != normalized.root_path() && normalized.filename().empty()) normalized = normalized.parent_path();
    hash.Append(SourcePathIdentityKey(normalized));
    const auto digest = hash.FinishHex();
    return L"Spectiary." + std::wstring(digest.begin(), digest.end());
}

HRESULT ConfigureShellWindowIdentity(HWND window, const std::wstring& app_id,
    const std::filesystem::path& executable)
{
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    HRESULT result = SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties));
    if (FAILED(result)) return result;
    const auto set = [&](REFPROPERTYKEY key, const std::wstring& value) {
        PROPVARIANT property{};
        HRESULT hr = InitPropVariantFromString(value.c_str(), &property);
        if (SUCCEEDED(hr)) hr = properties->SetValue(key, property);
        PropVariantClear(&property);
        return hr;
    };
    // Set relaunch properties before the ID, as required by the shell. Preserve
    // the source-free taskbar relaunch; source destinations carry their own args.
    if (FAILED(result = set(PKEY_AppUserModel_RelaunchCommand,
            QuoteWindowsCommandLineArgument(executable.wstring())))) return result;
    if (FAILED(result = set(PKEY_AppUserModel_RelaunchDisplayNameResource, L"Spectiary"))) return result;
    if (FAILED(result = set(PKEY_AppUserModel_RelaunchIconResource, executable.wstring() + L",0"))) return result;
    if (FAILED(result = set(PKEY_AppUserModel_ID, app_id))) return result;
    return properties->Commit();
}
} // namespace spectiary
