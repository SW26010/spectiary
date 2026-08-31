#include "ui/sample_labeling_filename.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string_view Utf8Bytes(std::u8string_view text)
{
    return std::string_view(
        reinterpret_cast<const char*>(text.data()), text.size());
}

void TestSafeStemRules()
{
    struct FilenameCase {
        std::string_view task_name;
        std::string_view expected;
    };
    const std::array cases = {
        FilenameCase{"Galaxy review", "Galaxy review.asdf"},
        FilenameCase{Utf8Bytes(u8"中文 日本語 🧪"),
            Utf8Bytes(u8"中文 日本語 🧪.asdf")},
        FilenameCase{"a/b:c*", "a_b_c_.asdf"},
        FilenameCase{"review.  ", "review.asdf"},
        FilenameCase{"CON", "_CON.asdf"},
        FilenameCase{"con.txt", "_con.txt.asdf"},
        FilenameCase{"LPT9", "_LPT9.asdf"},
        FilenameCase{"COM0", "COM0.asdf"},
        FilenameCase{"COM10", "COM10.asdf"},
        FilenameCase{"<>:\"/\\|?*", "_________.asdf"},
        FilenameCase{"...   ", "labeling-task.asdf"},
    };

    for (const FilenameCase& test : cases) {
        Require(
            specforge::SuggestedSampleLabelingFilename(
                test.task_name,
                specforge::SampleLabelingFilePurpose::CanonicalOutput) ==
                test.expected,
            "safe filename stem rule should produce the expected ASDF name");
    }
}

void TestControlCharactersAndInvalidUtf8AreSafe()
{
    const std::string controls =
        std::string{"a\x01", 2} +
        std::string{"\xc2\x85", 2} + "b";
    Require(
        specforge::SuggestedSampleLabelingFilename(
            controls,
            specforge::SampleLabelingFilePurpose::CanonicalOutput) ==
            "a__b.asdf",
        "ASCII and Unicode control characters should be replaced");

    Require(
        specforge::SuggestedSampleLabelingFilename(
            std::string_view{"\xc0\xaf", 2},
            specforge::SampleLabelingFilePurpose::CanonicalOutput) ==
            "labeling-task.asdf",
        "invalid UTF-8 should fall back instead of entering the platform dialog");
}

void TestPurposeExtensions()
{
    struct PurposeCase {
        specforge::SampleLabelingFilePurpose purpose;
        std::string_view expected;
    };
    constexpr std::array cases = {
        PurposeCase{
            specforge::SampleLabelingFilePurpose::CanonicalOutput,
            "Task.asdf"},
        PurposeCase{
            specforge::SampleLabelingFilePurpose::NpyExport,
            "Task.npy"},
        PurposeCase{
            specforge::SampleLabelingFilePurpose::CsvExport,
            "Task.csv"},
    };

    for (const PurposeCase& test : cases) {
        Require(
            specforge::SuggestedSampleLabelingFilename(
                "Task", test.purpose) == test.expected,
            "file purpose should select exactly one expected extension");
    }
}

}  // namespace

int main()
{
    try {
        TestSafeStemRules();
        TestControlCharactersAndInvalidUtf8AreSafe();
        TestPurposeExtensions();
    } catch (const std::exception& error) {
        std::cerr << "sample labeling filename test failed: "
                  << error.what() << '\n';
        return 1;
    }
    return 0;
}
