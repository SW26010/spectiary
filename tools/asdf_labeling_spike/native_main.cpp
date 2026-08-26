#include "native_codec.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

template <typename Value>
[[nodiscard]] Value ParseInteger(std::string_view text, std::string_view field)
{
    Value value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid " + std::string(field));
    }
    return value;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string_view(argv[1]) == "read") {
            const specforge::asdf_labeling_spike::LabelingDocument document =
                specforge::asdf_labeling_spike::ReadLabelingDocument(std::filesystem::path(argv[2]));
            std::cout << specforge::asdf_labeling_spike::SemanticJson(document) << '\n';
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "write-fixture") {
            specforge::asdf_labeling_spike::WriteLabelingDocument(
                std::filesystem::path(argv[2]),
                specforge::asdf_labeling_spike::NativeFixture());
            return 0;
        }
        if (argc == 6 && std::string_view(argv[1]) == "rewrite-value") {
            specforge::asdf_labeling_spike::RewriteLabelValuePreservingRosterBlock(
                std::filesystem::path(argv[2]),
                std::filesystem::path(argv[3]),
                ParseInteger<std::size_t>(argv[4], "value index"),
                ParseInteger<std::int32_t>(argv[5], "label value"));
            return 0;
        }
        std::cerr
            << "usage: specforge_asdf_labeling_spike_native read <path>\n"
            << "       specforge_asdf_labeling_spike_native write-fixture <path>\n"
            << "       specforge_asdf_labeling_spike_native rewrite-value <input> <output> <index> <value>\n";
        return 2;
    }
    catch (const std::exception& error) {
        std::cerr << "controlled ASDF error: " << error.what() << '\n';
        return 2;
    }
}
