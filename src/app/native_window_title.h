#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace specforge {

struct NativeWindowTitleContext {
    std::filesystem::path source_path;
    bool loading = false;
    std::string_view loading_text;
    bool sample_present = false;
    std::string_view sample_name;
    std::size_t sample_index = 0;
    std::size_t sample_count = 0;
};

// Non-owning view used by the App's hot reconciliation path. The caller keeps
// the referenced source path and sample text alive for the duration of the
// comparison and optional formatting operation.
struct NativeWindowTitleView {
    std::string_view product_name;
    const std::filesystem::path* source_path = nullptr;
    bool loading = false;
    std::string_view loading_text;
    bool sample_present = false;
    std::string_view sample_name;
    std::size_t sample_index = 0;
    std::size_t sample_count = 0;
};

class NativeWindowTitleSemanticKey {
public:
    explicit NativeWindowTitleSemanticKey(
        const NativeWindowTitleView& view);

    [[nodiscard]] bool Matches(
        const NativeWindowTitleView& view) const;

private:
    std::string product_name_;
    std::filesystem::path source_path_;
    bool loading_ = false;
    std::string loading_text_;
    bool sample_present_ = false;
    std::string sample_name_;
    std::size_t sample_index_ = 0;
    std::size_t sample_count_ = 0;
};

[[nodiscard]] std::wstring FormatSpecForgeNativeWindowTitle(
    const NativeWindowTitleView& view);

[[nodiscard]] std::wstring FormatSpecForgeNativeWindowTitle(
    std::string_view product_name,
    const NativeWindowTitleContext& context = {});

}  // namespace specforge
