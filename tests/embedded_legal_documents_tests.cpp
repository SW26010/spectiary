#include "app/embedded_legal_documents.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "legal source document should open");
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void TestEmbeddedDocumentsMatchSourceFiles()
{
    struct ExpectedDocument {
        specforge::LegalDocument document;
        std::string_view filename;
    };
    constexpr std::array expected_documents = {
        ExpectedDocument{
            specforge::LegalDocument::Eula,
            "EULA.txt"},
        ExpectedDocument{
            specforge::LegalDocument::ThirdPartyNotices,
            "THIRD_PARTY_NOTICES.txt"},
        ExpectedDocument{
            specforge::LegalDocument::DataSources,
            "DATA_SOURCES.txt"},
    };

    for (const ExpectedDocument& expected : expected_documents) {
        const std::string source = ReadFile(
            std::filesystem::path(
                SPECFORGE_LEGAL_SOURCE_DIRECTORY) /
            expected.filename);
        const std::string_view embedded =
            specforge::EmbeddedLegalDocumentContent(
                expected.document);
        Require(!embedded.empty(), "embedded legal document should exist");
        Require(
            embedded == source,
            "embedded legal document should byte-match its source file");
    }
}

}  // namespace

int main()
{
    try {
        TestEmbeddedDocumentsMatchSourceFiles();
        std::cout << "embedded legal document tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "embedded legal document tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
