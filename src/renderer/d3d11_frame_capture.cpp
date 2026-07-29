#include "renderer/d3d11_frame_capture.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <filesystem>
#include <limits>
#include <system_error>

namespace specforge {
namespace {

using Microsoft::WRL::ComPtr;

D3D11FrameCaptureResult Failure(
    std::string_view operation,
    HRESULT result)
{
    return {result, operation};
}

HRESULT ErrorCodeToHResult(const std::error_code& error)
{
    if (!error) {
        return E_FAIL;
    }
    return HRESULT_FROM_WIN32(
        static_cast<unsigned long>(error.value()));
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

class TemporaryOutput {
public:
    explicit TemporaryOutput(
        const std::filesystem::path& output_path)
        : output_path_(output_path),
          temporary_path_(output_path)
    {
        temporary_path_ += L".tmp";
    }

    ~TemporaryOutput()
    {
        if (!committed_) {
            std::error_code ignored;
            std::filesystem::remove(
                temporary_path_,
                ignored);
        }
    }

    [[nodiscard]] HRESULT Prepare()
    {
        std::error_code error;
        if (std::filesystem::exists(output_path_, error)) {
            return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        }
        if (error) {
            return ErrorCodeToHResult(error);
        }
        (void)std::filesystem::remove(temporary_path_, error);
        return error ? ErrorCodeToHResult(error) : S_OK;
    }

    [[nodiscard]] HRESULT Commit()
    {
        std::error_code error;
        std::filesystem::rename(
            temporary_path_,
            output_path_,
            error);
        if (error) {
            return ErrorCodeToHResult(error);
        }
        committed_ = true;
        return S_OK;
    }

    [[nodiscard]] const std::filesystem::path&
    path() const noexcept
    {
        return temporary_path_;
    }

private:
    std::filesystem::path output_path_;
    std::filesystem::path temporary_path_;
    bool committed_ = false;
};

}  // namespace

D3D11FrameCaptureResult CaptureD3D11TextureToPng(
    ID3D11Device* device,
    ID3D11DeviceContext* device_context,
    ID3D11Texture2D* source,
    const std::filesystem::path& output_path)
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

    TemporaryOutput output(output_path);
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
        ComPtr<IWICStream> stream;
        result = factory->CreateStream(
            stream.GetAddressOf());
        if (FAILED(result)) {
            return Failure(
                "IWICImagingFactory::CreateStream",
                result);
        }
        result = stream->InitializeFromFilename(
            output.path().c_str(),
            GENERIC_WRITE);
        if (FAILED(result)) {
            return Failure(
                "IWICStream::InitializeFromFilename",
                result);
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

    result = output.Commit();
    if (FAILED(result)) {
        return Failure(
            "Commit frame capture output",
            result);
    }
    return {S_OK, {}};
}

}  // namespace specforge
