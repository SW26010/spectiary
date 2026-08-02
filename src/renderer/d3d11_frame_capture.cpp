#include "renderer/d3d11_frame_capture.h"

#include "platform/win32_file_identity.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace specforge {
namespace {

using Microsoft::WRL::ComPtr;

D3D11FrameCaptureResult Failure(
    std::string_view operation,
    HRESULT result)
{
    return {result, operation};
}

HRESULT LastErrorToHResult()
{
    return HRESULT_FROM_WIN32(
        GetLastError());
}

class ScopedComInitialization {
public:
    ScopedComInitialization()
        : result_(CoInitializeEx(
              nullptr,
              COINIT_APARTMENTTHREADED |
                  COINIT_DISABLE_OLE1DDE)),
          uninitialize_(SUCCEEDED(result_))
    {
    }

    ~ScopedComInitialization()
    {
        if (uninitialize_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const noexcept
    {
        return result_ == RPC_E_CHANGED_MODE ? S_OK : result_;
    }

private:
    HRESULT result_ = E_FAIL;
    bool uninitialize_ = false;
};

class ScopedTextureMap {
public:
    ScopedTextureMap(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture)
        : context_(context),
          texture_(texture)
    {
    }

    ~ScopedTextureMap()
    {
        if (mapped_) {
            context_->Unmap(texture_, 0);
        }
    }

    D3D11_MAPPED_SUBRESOURCE* address() noexcept
    {
        return &mapped_resource_;
    }

    void MarkMapped() noexcept
    {
        mapped_ = true;
    }

private:
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped_resource_ = {};
    bool mapped_ = false;
};

class UniqueHandle {
public:
    UniqueHandle() = default;

    explicit UniqueHandle(HANDLE handle)
        : handle_(handle)
    {
    }

    ~UniqueHandle()
    {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) =
        delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(
              other.handle_,
              INVALID_HANDLE_VALUE))
    {
    }

    UniqueHandle& operator=(
        UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(
                other.handle_,
                INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    void Reset(
        HANDLE handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE &&
            handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    [[nodiscard]] HANDLE get() const noexcept
    {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE &&
               handle_ != nullptr;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class HandleStream final : public IStream {
public:
    explicit HandleStream(HANDLE handle)
        : handle_(handle)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interface_id,
        void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_id == IID_IUnknown ||
            interface_id == IID_ISequentialStream ||
            interface_id == IID_IStream) {
            *object = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(
            InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining =
            static_cast<ULONG>(
                InterlockedDecrement(&references_));
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Read(
        void* buffer,
        ULONG byte_count,
        ULONG* bytes_read) override
    {
        DWORD transferred = 0;
        if (ReadFile(
                handle_,
                buffer,
                byte_count,
                &transferred,
                nullptr) == FALSE) {
            return LastErrorToHResult();
        }
        if (bytes_read != nullptr) {
            *bytes_read = transferred;
        }
        return transferred == byte_count
            ? S_OK
            : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(
        const void* buffer,
        ULONG byte_count,
        ULONG* bytes_written) override
    {
        DWORD transferred = 0;
        if (WriteFile(
                handle_,
                buffer,
                byte_count,
                &transferred,
                nullptr) == FALSE) {
            return LastErrorToHResult();
        }
        if (bytes_written != nullptr) {
            *bytes_written = transferred;
        }
        return transferred == byte_count
            ? S_OK
            : STG_E_MEDIUMFULL;
    }

    HRESULT STDMETHODCALLTYPE Seek(
        LARGE_INTEGER move,
        DWORD origin,
        ULARGE_INTEGER* new_position) override
    {
        DWORD method = 0;
        switch (origin) {
        case STREAM_SEEK_SET:
            method = FILE_BEGIN;
            break;
        case STREAM_SEEK_CUR:
            method = FILE_CURRENT;
            break;
        case STREAM_SEEK_END:
            method = FILE_END;
            break;
        default:
            return STG_E_INVALIDFUNCTION;
        }
        LARGE_INTEGER position = {};
        if (SetFilePointerEx(
                handle_,
                move,
                &position,
                method) == FALSE) {
            return LastErrorToHResult();
        }
        if (new_position != nullptr) {
            new_position->QuadPart =
                static_cast<ULONGLONG>(
                    position.QuadPart);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(
        ULARGE_INTEGER size) override
    {
        LARGE_INTEGER current = {};
        LARGE_INTEGER zero = {};
        if (SetFilePointerEx(
                handle_,
                zero,
                &current,
                FILE_CURRENT) == FALSE) {
            return LastErrorToHResult();
        }
        LARGE_INTEGER requested = {};
        requested.QuadPart =
            static_cast<LONGLONG>(
                size.QuadPart);
        if (SetFilePointerEx(
                handle_,
                requested,
                nullptr,
                FILE_BEGIN) == FALSE ||
            SetEndOfFile(handle_) == FALSE ||
            SetFilePointerEx(
                handle_,
                current,
                nullptr,
                FILE_BEGIN) == FALSE) {
            return LastErrorToHResult();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CopyTo(
        IStream*,
        ULARGE_INTEGER,
        ULARGE_INTEGER*,
        ULARGE_INTEGER*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override
    {
        return FlushFileBuffers(handle_) != FALSE
            ? S_OK
            : LastErrorToHResult();
    }

    HRESULT STDMETHODCALLTYPE Revert() override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE LockRegion(
        ULARGE_INTEGER,
        ULARGE_INTEGER,
        DWORD) override
    {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE UnlockRegion(
        ULARGE_INTEGER,
        ULARGE_INTEGER,
        DWORD) override
    {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE Stat(
        STATSTG* statistics,
        DWORD flags) override
    {
        if (statistics == nullptr) {
            return E_POINTER;
        }
        *statistics = {};
        if ((flags & STATFLAG_NONAME) == 0) {
            return STG_E_INVALIDFLAG;
        }
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(handle_, &size) == FALSE) {
            return LastErrorToHResult();
        }
        statistics->type = STGTY_STREAM;
        statistics->cbSize.QuadPart =
            static_cast<ULONGLONG>(
                size.QuadPart);
        statistics->grfMode =
            STGM_READWRITE | STGM_SHARE_DENY_WRITE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(
        IStream**) override
    {
        return E_NOTIMPL;
    }

private:
    ~HandleStream() = default;

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    volatile LONG references_ = 1;
};

bool PathContainsOrEquals(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    auto root_iterator = root.begin();
    auto candidate_iterator = candidate.begin();
    for (; root_iterator != root.end();
         ++root_iterator, ++candidate_iterator) {
        if (candidate_iterator == candidate.end() ||
            !Win32PathsEqualOrdinal(
                *root_iterator,
                *candidate_iterator)) {
            return false;
        }
    }
    return true;
}

HRESULT OpenAbsoluteDirectory(
    const std::filesystem::path& path,
    UniqueHandle& directory)
{
    // Temporary cleanup and publication use DELETE on the temporary file
    // handle itself; the directory does not need FILE_DELETE_CHILD.
    directory.Reset(
        CreateFileW(
            path.c_str(),
            FILE_LIST_DIRECTORY |
                FILE_ADD_FILE |
                FILE_TRAVERSE |
                FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
    if (!directory) {
        return LastErrorToHResult();
    }
    return ValidateWin32DirectoryHandleNoFollow(
        directory.get());
}

HRESULT OpenRelativeIntoHandle(
    HANDLE root,
    std::wstring_view name,
    ACCESS_MASK access,
    ULONG share_access,
    ULONG disposition,
    ULONG options,
    ULONG attributes,
    UniqueHandle& opened)
{
    NTSTATUS status = 0;
    const HANDLE handle =
        OpenWin32Relative(
            root,
            name,
            access,
            share_access,
            disposition,
            options,
            attributes,
            &status);
    if (handle == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_NT(status);
    }
    opened.Reset(handle);
    return S_OK;
}

HRESULT OpenOrCreateRelativeDirectory(
    HANDLE root,
    std::wstring_view name,
    UniqueHandle& directory)
{
    const HRESULT result =
        OpenRelativeIntoHandle(
            root,
            name,
            FILE_LIST_DIRECTORY |
                FILE_ADD_FILE |
                FILE_TRAVERSE |
                FILE_READ_ATTRIBUTES |
                SYNCHRONIZE,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            FILE_OPEN_IF,
            FILE_DIRECTORY_FILE |
                FILE_OPEN_REPARSE_POINT |
                FILE_SYNCHRONOUS_IO_NONALERT,
            FILE_ATTRIBUTE_NORMAL,
            directory);
    return FAILED(result)
        ? result
        : ValidateWin32DirectoryHandleNoFollow(
              directory.get());
}

std::wstring RandomTemporaryName()
{
    std::random_device random;
    constexpr wchar_t kHex[] =
        L"0123456789abcdef";
    std::wstring name =
        L".specforge-capture-";
    for (int index = 0; index < 32; ++index) {
        name.push_back(
            kHex[random() & 0x0fU]);
    }
    name += L".tmp";
    return name;
}

HRESULT RenameFileRelativeNoReplace(
    HANDLE file,
    HANDLE parent,
    std::wstring_view final_name)
{
    using NtSetInformationFileFunction =
        NTSTATUS(NTAPI*)(
            HANDLE,
            PIO_STATUS_BLOCK,
            PVOID,
            ULONG,
            FILE_INFORMATION_CLASS);
    const auto set_information =
        reinterpret_cast<
            NtSetInformationFileFunction>(
            GetProcAddress(
                GetModuleHandleW(L"ntdll.dll"),
                "NtSetInformationFile"));
    if (set_information == nullptr) {
        return HRESULT_FROM_WIN32(
            ERROR_PROC_NOT_FOUND);
    }
    const ULONG name_bytes =
        static_cast<ULONG>(
            final_name.size() *
            sizeof(wchar_t));
    std::vector<std::byte> buffer(
        offsetof(FILE_RENAME_INFO, FileName) +
        name_bytes);
    auto* rename =
        reinterpret_cast<FILE_RENAME_INFO*>(
            buffer.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = parent;
    rename->FileNameLength = name_bytes;
    std::copy(
        reinterpret_cast<const std::byte*>(
            final_name.data()),
        reinterpret_cast<const std::byte*>(
            final_name.data()) +
            name_bytes,
        buffer.begin() +
            offsetof(
                FILE_RENAME_INFO,
                FileName));
    IO_STATUS_BLOCK status_block = {};
    constexpr auto kFileRenameInformation =
        static_cast<FILE_INFORMATION_CLASS>(10);
    const NTSTATUS status =
        set_information(
            file,
            &status_block,
            rename,
            static_cast<ULONG>(buffer.size()),
            kFileRenameInformation);
    return status < 0
        ? HRESULT_FROM_NT(status)
        : S_OK;
}

class TemporaryOutput {
public:
    TemporaryOutput(
        const std::filesystem::path& output_path,
        const std::optional<std::filesystem::path>&
            allowed_root)
        : output_path_(Win32FullPath(output_path)),
          allowed_root_(
              allowed_root
              ? std::optional<std::filesystem::path>(
                    Win32FullPath(*allowed_root))
              : std::nullopt)
    {
    }

    ~TemporaryOutput()
    {
        if (temporary_ && !committed_) {
            (void)MarkWin32HandleForDeletion(
                temporary_.get());
        }
    }

    [[nodiscard]] HRESULT Prepare()
    {
        if (output_path_.empty() ||
            !output_path_.is_absolute() ||
            output_path_.filename().empty()) {
            return E_INVALIDARG;
        }
        const std::filesystem::path parent =
            output_path_.parent_path();
        if (allowed_root_) {
            if (allowed_root_->empty() ||
                !PathContainsOrEquals(
                    *allowed_root_,
                    parent)) {
                return E_ACCESSDENIED;
            }
            const HRESULT root_result =
                OpenAbsoluteDirectory(
                    *allowed_root_,
                    directories_.emplace_back());
            if (FAILED(root_result)) {
                return root_result;
            }
            std::wstring relative =
                parent.wstring().substr(
                    allowed_root_->wstring().size());
            while (!relative.empty() &&
                   (relative.front() == L'\\' ||
                    relative.front() == L'/')) {
                relative.erase(relative.begin());
            }
            std::filesystem::path relative_path(
                std::move(relative));
            for (const auto& component :
                 relative_path) {
                if (component.empty() ||
                    component == L"." ||
                    component == L"..") {
                    return E_ACCESSDENIED;
                }
                UniqueHandle directory;
                const HRESULT result =
                    OpenOrCreateRelativeDirectory(
                        directories_.back().get(),
                        component.wstring(),
                        directory);
                if (FAILED(result)) {
                    return result;
                }
                directories_.push_back(
                    std::move(directory));
            }
        } else {
            const HRESULT result =
                OpenAbsoluteDirectory(
                    parent,
                    directories_.emplace_back());
            if (FAILED(result)) {
                return result;
            }
        }
        parent_ = directories_.back().get();
        HRESULT last_result = E_FAIL;
        for (int attempt = 0; attempt < 8; ++attempt) {
            temporary_name_ =
                RandomTemporaryName();
            last_result =
                OpenRelativeIntoHandle(
                    parent_,
                    temporary_name_,
                    GENERIC_READ |
                        GENERIC_WRITE |
                        DELETE |
                        SYNCHRONIZE,
                    FILE_SHARE_READ,
                    FILE_CREATE,
                    FILE_NON_DIRECTORY_FILE |
                        FILE_OPEN_REPARSE_POINT |
                        FILE_SYNCHRONOUS_IO_NONALERT,
                    FILE_ATTRIBUTE_TEMPORARY,
                    temporary_);
            if (SUCCEEDED(last_result)) {
                return
                    ValidateWin32RegularFileHandleNoFollow(
                        temporary_.get());
            }
        }
        return last_result;
    }

    [[nodiscard]] HRESULT Commit()
    {
        const HRESULT boundary_before =
            ValidatePinnedBoundary();
        if (FAILED(boundary_before)) {
            return boundary_before;
        }
        const std::wstring final_name =
            output_path_.filename().wstring();
        const HRESULT result =
            RenameFileRelativeNoReplace(
                temporary_.get(),
                parent_,
                final_name);
        if (FAILED(result)) {
            return result;
        }
        const HRESULT boundary_after =
            ValidatePinnedBoundary();
        const std::filesystem::path
            published_path =
                Win32FinalPathByHandle(
                    temporary_.get());
        if (FAILED(boundary_after) ||
            published_path.empty() ||
            !Win32PathsEqualOrdinal(
                published_path,
                output_path_)) {
            (void)MarkWin32HandleForDeletion(
                temporary_.get());
            return FAILED(boundary_after)
                ? boundary_after
                : E_ACCESSDENIED;
        }
        committed_ = true;
        return S_OK;
    }

    [[nodiscard]] HANDLE handle() const noexcept
    {
        return temporary_.get();
    }

private:
    [[nodiscard]] HRESULT
    ValidatePinnedBoundary() const
    {
        if (!allowed_root_) {
            return S_OK;
        }
        if (directories_.empty()) {
            return E_UNEXPECTED;
        }
        const std::filesystem::path
            pinned_root =
                Win32FinalPathByHandle(
                    directories_.front().get());
        const std::filesystem::path
            pinned_parent =
                Win32FinalPathByHandle(parent_);
        if (pinned_root.empty() ||
            pinned_parent.empty() ||
            !Win32PathsEqualOrdinal(
                pinned_root,
                *allowed_root_) ||
            !Win32PathsEqualOrdinal(
                pinned_parent,
                output_path_.parent_path()) ||
            !PathContainsOrEquals(
                pinned_root,
                pinned_parent)) {
            return E_ACCESSDENIED;
        }
        return S_OK;
    }

    std::filesystem::path output_path_;
    std::optional<std::filesystem::path>
        allowed_root_;
    std::vector<UniqueHandle> directories_;
    UniqueHandle temporary_;
    HANDLE parent_ = INVALID_HANDLE_VALUE;
    std::wstring temporary_name_;
    bool committed_ = false;
};

}  // namespace

D3D11FrameCaptureResult CaptureD3D11TextureToPng(
    ID3D11Device* device,
    ID3D11DeviceContext* device_context,
    ID3D11Texture2D* source,
    const std::filesystem::path& output_path,
    const D3D11FrameCaptureFinalizer& finalizer,
    const std::optional<std::filesystem::path>&
        allowed_root)
{
    if (device == nullptr || device_context == nullptr ||
        source == nullptr || output_path.empty()) {
        return Failure(
            "CaptureD3D11TextureToPng arguments",
            E_INVALIDARG);
    }

    D3D11_TEXTURE2D_DESC description = {};
    source->GetDesc(&description);
    if (description.Width == 0 || description.Height == 0 ||
        description.ArraySize != 1 ||
        description.SampleDesc.Count != 1 ||
        description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        return Failure(
            "CaptureD3D11TextureToPng source format",
            DXGI_ERROR_UNSUPPORTED);
    }

    D3D11_TEXTURE2D_DESC staging_description =
        description;
    staging_description.MipLevels = 1;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.BindFlags = 0;
    staging_description.CPUAccessFlags =
        D3D11_CPU_ACCESS_READ;
    staging_description.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = device->CreateTexture2D(
        &staging_description,
        nullptr,
        staging.GetAddressOf());
    if (FAILED(result)) {
        return Failure(
            "ID3D11Device::CreateTexture2D(frame capture staging)",
            result);
    }

    device_context->CopyResource(
        staging.Get(),
        source);
    ScopedTextureMap mapped(
        device_context,
        staging.Get());
    result = device_context->Map(
        staging.Get(),
        0,
        D3D11_MAP_READ,
        0,
        mapped.address());
    if (FAILED(result)) {
        return Failure(
            "ID3D11DeviceContext::Map(frame capture staging)",
            result);
    }
    mapped.MarkMapped();

    const D3D11_MAPPED_SUBRESOURCE& pixels =
        *mapped.address();
    if (pixels.pData == nullptr ||
        pixels.RowPitch <
            description.Width * 4U ||
        description.Height >
            (std::numeric_limits<UINT>::max)() /
                pixels.RowPitch) {
        return Failure(
            "D3D11 frame capture mapped layout",
            E_UNEXPECTED);
    }
    const UINT pixel_bytes =
        pixels.RowPitch * description.Height;

    TemporaryOutput output(
        output_path,
        allowed_root);
    result = output.Prepare();
    if (FAILED(result)) {
        return Failure(
            "Prepare frame capture output",
            result);
    }
    {
        ScopedComInitialization com;
        result = com.result();
        if (FAILED(result)) {
            return Failure(
                "CoInitializeEx(frame capture)",
                result);
        }
        ComPtr<IWICImagingFactory> factory;
        result = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(result)) {
            return Failure(
                "CoCreateInstance(CLSID_WICImagingFactory)",
                result);
        }
        ComPtr<IWICBitmap> bitmap;
        result = factory->CreateBitmapFromMemory(
            description.Width,
            description.Height,
            GUID_WICPixelFormat32bppRGBA,
            pixels.RowPitch,
            pixel_bytes,
            static_cast<BYTE*>(pixels.pData),
            bitmap.GetAddressOf());
        if (FAILED(result)) {
            return Failure(
                "IWICImagingFactory::CreateBitmapFromMemory",
                result);
        }
        ComPtr<IStream> stream;
        stream.Attach(
            new (std::nothrow)
                HandleStream(output.handle()));
        if (!stream) {
            return Failure(
                "Create frame capture handle stream",
                E_OUTOFMEMORY);
        }
        ComPtr<IWICBitmapEncoder> encoder;
        result = factory->CreateEncoder(
            GUID_ContainerFormatPng,
            nullptr,
            encoder.GetAddressOf());
        if (FAILED(result)) {
            return Failure(
                "IWICImagingFactory::CreateEncoder(PNG)",
                result);
        }
        result = encoder->Initialize(
            stream.Get(),
            WICBitmapEncoderNoCache);
        if (FAILED(result)) {
            return Failure(
                "IWICBitmapEncoder::Initialize",
                result);
        }
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        result = encoder->CreateNewFrame(
            frame.GetAddressOf(),
            properties.GetAddressOf());
        if (FAILED(result)) {
            return Failure(
                "IWICBitmapEncoder::CreateNewFrame",
                result);
        }
        result = frame->Initialize(properties.Get());
        if (FAILED(result)) {
            return Failure(
                "IWICBitmapFrameEncode::Initialize",
                result);
        }
        result = frame->SetSize(
            description.Width,
            description.Height);
        if (FAILED(result)) {
            return Failure(
                "IWICBitmapFrameEncode::SetSize",
                result);
        }
        WICPixelFormatGUID output_format =
            GUID_WICPixelFormat32bppRGBA;
        result = frame->SetPixelFormat(&output_format);
        if (FAILED(result)) {
            return Failure(
                "IWICBitmapFrameEncode::SetPixelFormat",
                result);
        }
        result = frame->WriteSource(
            bitmap.Get(),
            nullptr);
        if (FAILED(result)) {
            return Failure(
                "IWICBitmapFrameEncode::WriteSource",
                result);
        }
        result = frame->Commit();
        if (FAILED(result)) {
            return Failure(
                "IWICBitmapFrameEncode::Commit",
                result);
        }
        result = encoder->Commit();
        if (FAILED(result)) {
            return Failure(
                "IWICBitmapEncoder::Commit",
                result);
        }
    }

    const std::function<HRESULT()> publish =
        [&output]() {
            return output.Commit();
        };
    result = finalizer ? finalizer(publish)
                       : publish();
    if (FAILED(result)) {
        return Failure(
            finalizer
                ? "Finalize frame capture output"
                : "Commit frame capture output",
            result);
    }
    return {S_OK, {}};
}

}  // namespace specforge
