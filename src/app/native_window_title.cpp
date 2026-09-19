#include "app/native_window_title.h"

#include "platform/win32_text.h"
#include "spectiary/spectiary_build_identity.h"

#include <algorithm>

namespace spectiary {
namespace {

constexpr std::size_t kMaxSourceNameLength = 48;
constexpr std::size_t kMaxSampleNameLength = 64;
constexpr std::wstring_view kTitleSeparator = L" | ";
constexpr wchar_t kEllipsis = L'\u2026';

bool IsHighSurrogate(wchar_t value)
{
    return value >= 0xD800 && value <= 0xDBFF;
}

bool IsLowSurrogate(wchar_t value)
{
    return value >= 0xDC00 && value <= 0xDFFF;
}

std::wstring PrefixWithoutSplitSurrogate(
    std::wstring_view value,
    std::size_t length)
{
    length = std::min(length, value.size());
    if (length > 0 && length < value.size() &&
        IsHighSurrogate(value[length - 1]) &&
        IsLowSurrogate(value[length])) {
        --length;
    }
    return std::wstring(value.substr(0, length));
}

std::wstring SuffixWithoutSplitSurrogate(
    std::wstring_view value,
    std::size_t length)
{
    length = std::min(length, value.size());
    std::size_t start = value.size() - length;
    if (start > 0 && start < value.size() &&
        IsLowSurrogate(value[start]) &&
        IsHighSurrogate(value[start - 1])) {
        ++start;
    }
    return std::wstring(value.substr(start));
}

std::wstring TruncateMiddle(
    std::wstring_view value,
    std::size_t maximum_length)
{
    if (value.size() <= maximum_length || maximum_length < 2) {
        return std::wstring(value);
    }
    const std::size_t retained = maximum_length - 1;
    const std::size_t prefix_length = (retained + 1) / 2;
    const std::size_t suffix_length = retained - prefix_length;
    std::wstring result = PrefixWithoutSplitSurrogate(
        value,
        prefix_length);
    result.push_back(kEllipsis);
    result += SuffixWithoutSplitSurrogate(
        value,
        suffix_length);
    return result;
}

std::wstring TruncateEnd(
    std::wstring_view value,
    std::size_t maximum_length)
{
    if (value.size() <= maximum_length || maximum_length < 2) {
        return std::wstring(value);
    }
    std::wstring result = PrefixWithoutSplitSurrogate(
        value,
        maximum_length - 1);
    result.push_back(kEllipsis);
    return result;
}

std::wstring SourceDisplayName(
    const std::filesystem::path& path)
{
    if (path.empty()) {
        return {};
    }
    std::filesystem::path name = path.filename();
    if (name.empty()) {
        name = path.parent_path().filename();
    }
    return TruncateMiddle(
        name.native(),
        kMaxSourceNameLength);
}

void AppendTitleComponent(
    std::wstring& title,
    std::wstring_view component)
{
    if (component.empty()) {
        return;
    }
    title += kTitleSeparator;
    title += component;
}

}  // namespace

NativeWindowTitleSemanticKey::NativeWindowTitleSemanticKey(
    const NativeWindowTitleView& view)
    : product_name_(view.product_name),
      source_path_(
          view.source_path == nullptr
              ? std::filesystem::path{}
              : *view.source_path),
      loading_(
          !source_path_.empty() && view.loading),
      loading_text_(
          loading_ ? view.loading_text
                   : std::string_view{}),
      sample_present_(
          !source_path_.empty() && !loading_ &&
          view.sample_present),
      sample_name_(
          sample_present_ ? view.sample_name
                          : std::string_view{}),
      sample_index_(
          sample_present_ ? view.sample_index : 0),
      sample_count_(
          sample_present_ ? view.sample_count : 0)
{
}

bool NativeWindowTitleSemanticKey::Matches(
    const NativeWindowTitleView& view) const
{
    const bool source_present =
        view.source_path != nullptr &&
        !view.source_path->empty();
    if (product_name_ != view.product_name ||
        source_present != !source_path_.empty() ||
        (source_present &&
         source_path_ != *view.source_path)) {
        return false;
    }

    const bool loading =
        source_present && view.loading;
    if (loading_ != loading ||
        (loading && loading_text_ != view.loading_text)) {
        return false;
    }

    const bool sample_present =
        source_present && !loading &&
        view.sample_present;
    return sample_present_ == sample_present &&
           (!sample_present ||
            (sample_name_ == view.sample_name &&
             sample_index_ == view.sample_index &&
             sample_count_ == view.sample_count));
}

std::wstring FormatSpectiaryNativeWindowTitle(
    const NativeWindowTitleView& view)
{
    std::wstring title = Utf8ToWide(view.product_name);
    const std::wstring version = Utf8ToWide(
        build_info::kSpectiaryVersion);
    if (!version.empty()) {
        if (!title.empty()) {
            title.push_back(L' ');
        }
        title += version;
    }

    const std::wstring source_name =
        view.source_path == nullptr
            ? std::wstring{}
            : SourceDisplayName(*view.source_path);
    if (source_name.empty()) {
        return title;
    }
    AppendTitleComponent(title, source_name);

    if (view.loading) {
        AppendTitleComponent(
            title,
            Utf8ToWide(view.loading_text));
        return title;
    }
    if (!view.sample_present) {
        return title;
    }

    if (view.sample_count > 0 &&
        view.sample_index < view.sample_count) {
        AppendTitleComponent(
            title,
            std::to_wstring(view.sample_index + 1) +
                L"/" +
                std::to_wstring(view.sample_count));
    }
    AppendTitleComponent(
        title,
        TruncateEnd(
            Utf8ToWide(view.sample_name),
            kMaxSampleNameLength));
    return title;
}

std::wstring FormatSpectiaryNativeWindowTitle(
    std::string_view product_name,
    const NativeWindowTitleContext& context)
{
    return FormatSpectiaryNativeWindowTitle({
        .product_name = product_name,
        .source_path = &context.source_path,
        .loading = context.loading,
        .loading_text = context.loading_text,
        .sample_present = context.sample_present,
        .sample_name = context.sample_name,
        .sample_index = context.sample_index,
        .sample_count = context.sample_count,
    });
}

}  // namespace spectiary
