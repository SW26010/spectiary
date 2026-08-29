#include "domain/sample_label_export.h"

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

void TestUnlabeledSentinelCannotCollideWithLabelName()
{
    specforge::SampleLabelSet labels;
    Require(
        specforge::UpsertSampleLabel(
            labels,
            specforge::SampleLabelDefinition{
                5,
                "unlabeled",
                'u'}),
        "the labeling domain should continue to accept the existing legal name");

    const std::string sentinel =
        specforge::SerializeSampleLabelValueForExport(
            labels,
            specforge::kUnlabeledSampleLabelCode);
    const std::string labeled =
        specforge::SerializeSampleLabelValueForExport(
            labels,
            5);
    Require(
        sentinel == "unlabeled" &&
            labeled == "\\unlabeled" &&
            sentinel != labeled,
        "unlabeled sentinel text must not collide with a legal label name");

    const specforge::DeserializedSampleLabelExportValue
        sentinel_value =
            specforge::DeserializeSampleLabelValueFromExport(
                sentinel);
    const specforge::DeserializedSampleLabelExportValue
        labeled_value =
            specforge::DeserializeSampleLabelValueFromExport(
                labeled);
    Require(
        sentinel_value.represents_unlabeled &&
            sentinel_value.label_text.empty() &&
            !labeled_value.represents_unlabeled &&
            labeled_value.label_text == "unlabeled",
        "shared ingestion decoding should preserve the sentinel/label distinction");
}

void TestLeadingEscapePrefixRoundTrips()
{
    const std::string label_name = "\\unlabeled";
    const std::string serialized =
        specforge::SerializeSampleLabelNameForExport(
            label_name);
    Require(
        serialized == "\\\\unlabeled",
        "an existing leading escape prefix should be escaped exactly once");

    const specforge::DeserializedSampleLabelExportValue
        deserialized =
            specforge::DeserializeSampleLabelValueFromExport(
                serialized);
    Require(
        !deserialized.represents_unlabeled &&
            deserialized.label_text == label_name,
        "ingestion decoding should remove exactly one escape prefix");
}

void TestOrdinaryLabelTextRemainsReadableAndRoundTrips()
{
    const std::string label_name = "星系,A\n可信";
    const std::string serialized =
        specforge::SerializeSampleLabelNameForExport(
            label_name);
    Require(
        serialized == label_name,
        "ordinary label text should not receive unnecessary encoding");

    const specforge::DeserializedSampleLabelExportValue
        deserialized =
            specforge::DeserializeSampleLabelValueFromExport(
                serialized);
    Require(
        !deserialized.represents_unlabeled &&
            deserialized.label_text == label_name,
        "ordinary Unicode label text should round-trip independently of CSV quoting");
}

}  // namespace

int main()
{
    try {
        TestUnlabeledSentinelCannotCollideWithLabelName();
        TestLeadingEscapePrefixRoundTrips();
        TestOrdinaryLabelTextRemainsReadableAndRoundTrips();
        std::cout << "sample label export tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
