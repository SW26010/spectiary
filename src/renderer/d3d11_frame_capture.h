#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace specforge {

struct D3D11FrameCaptureResult {
    HRESULT result = E_FAIL;
    std::string_view operation;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return SUCCEEDED(result);
    }
};

using D3D11FrameCaptureFinalizer =
    std::function<HRESULT(
        const std::function<HRESULT()>& publish)>;

[[nodiscard]] D3D11FrameCaptureResult
CaptureD3D11TextureToPng(
    ID3D11Device* device,
    ID3D11DeviceContext* device_context,
    ID3D11Texture2D* source,
    const std::filesystem::path& output_path,
    const D3D11FrameCaptureFinalizer&
        finalizer = {},
    const std::optional<std::filesystem::path>&
        allowed_root = std::nullopt);

}  // namespace specforge
