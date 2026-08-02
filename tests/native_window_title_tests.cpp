#include "app/native_window_title.h"
#include "platform/win32_text.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::wstring ProductAndVersion()
{
    return L"SpecForge " +
           specforge::Utf8ToWide(
               SPECFORGE_EXPECTED_VERSION);
}

void TestIdleTitleContainsOnlyProductIdentity()
{
    Require(
        specforge::FormatSpecForgeNativeWindowTitle(
            "SpecForge") == ProductAndVersion(),
        "idle title should contain the product name and build version");
}

void TestActiveTitleUsesFilenameAndOneBasedPosition()
{
    const std::wstring title =
        specforge::FormatSpecForgeNativeWindowTitle(
            "SpecForge",
            {
                .source_path =
                    std::filesystem::path(
                        LR"(C:\observations\night-1\source.fits)"),
                .sample_present = true,
                .sample_name = "HD 12345",
                .sample_index = 2,
                .sample_count = 12,
            });
    Require(
        title == ProductAndVersion() +
                     L" | source.fits | 3/12 | HD 12345",
        "active title should use the filename, one-based position, and sample name in order");
    Require(
        title.find(L"observations") == std::wstring::npos,
        "active title should not expose the full source path");
}

void TestLoadingTitleDoesNotRetainSampleContext()
{
    const std::wstring title =
        specforge::FormatSpecForgeNativeWindowTitle(
            "SpecForge",
            {
                .source_path = L"next-source.npy",
                .loading = true,
                .loading_text = "\u6b63\u5728\u52a0\u8f7d\u6e90\u2026",
                .sample_present = true,
                .sample_name = "stale sample",
                .sample_index = 8,
                .sample_count = 20,
            });
    Require(
        title == ProductAndVersion() +
                     L" | next-source.npy | \u6b63\u5728\u52a0\u8f7d\u6e90\u2026",
        "loading title should contain only the target source and loading status");
    Require(
        title.find(L"stale sample") == std::wstring::npos,
        "loading title should omit stale sample context");
}

void TestLongNamesAreCompressedPredictably()
{
    const std::wstring title =
        specforge::FormatSpecForgeNativeWindowTitle(
            "SpecForge",
            {
                .source_path =
                    L"an_extremely_long_observation_source_filename_that_should_keep_its_extension.fits",
                .sample_present = true,
                .sample_name =
                    "A very long sample name that should be shortened at the end before it overwhelms the native title bar",
                .sample_index = 0,
                .sample_count = 1,
            });
    Require(
        title.find(L'\u2026') != std::wstring::npos,
        "long title components should use an ellipsis");
    Require(
        title.find(L".fits") != std::wstring::npos,
        "source compression should preserve the filename extension");
    Require(
        title.find(L"overwhelms") == std::wstring::npos,
        "sample compression should discard the end of a long name");
    Require(
        title.find(L" | 1/1 | ") != std::wstring::npos,
        "name compression should retain the sample position before the sample name");
}

void TestSemanticKeyTracksOnlyTitleInputs()
{
    const std::filesystem::path source_path =
        L"source.npy";
    specforge::NativeWindowTitleView view{
        .product_name = "SpecForge",
        .source_path = &source_path,
        .sample_present = true,
        .sample_name = "HD 12345",
        .sample_index = 2,
        .sample_count = 12,
    };
    const specforge::NativeWindowTitleSemanticKey key(
        view);
    Require(
        key.Matches(view),
        "semantic key should match an unchanged title view");

    view.sample_index = 3;
    Require(
        !key.Matches(view),
        "semantic key should reject a changed visible sample position");
    view.sample_index = 2;
    view.loading = true;
    view.loading_text = "Loading source...";
    Require(
        !key.Matches(view),
        "semantic key should reject a changed visible loading state");
}

}  // namespace

int main()
{
    TestIdleTitleContainsOnlyProductIdentity();
    TestActiveTitleUsesFilenameAndOneBasedPosition();
    TestLoadingTitleDoesNotRetainSampleContext();
    TestLongNamesAreCompressedPredictably();
    TestSemanticKeyTracksOnlyTitleInputs();
    return 0;
}
