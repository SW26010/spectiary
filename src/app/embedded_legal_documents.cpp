#include "app/embedded_legal_documents.h"

#include "platform/specforge_resource.h"

#include <Windows.h>

#include <cstddef>

namespace specforge {
namespace {

int ResourceId(LegalDocument document) noexcept
{
    switch (document) {
    case LegalDocument::Eula:
        return SPECFORGE_RESOURCE_EULA;
    case LegalDocument::ThirdPartyNotices:
        return SPECFORGE_RESOURCE_THIRD_PARTY_NOTICES;
    case LegalDocument::DataSources:
        return SPECFORGE_RESOURCE_DATA_SOURCES;
    }
    return 0;
}

}  // namespace

std::string_view EmbeddedLegalDocumentContent(
    LegalDocument document) noexcept
{
    const int resource_id = ResourceId(document);
    if (resource_id == 0) {
        return {};
    }

    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        module,
        MAKEINTRESOURCEW(resource_id),
        RT_RCDATA);
    if (resource == nullptr) {
        return {};
    }

    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const void* data = loaded != nullptr
        ? LockResource(loaded)
        : nullptr;
    if (data == nullptr || size == 0) {
        return {};
    }

    return {
        static_cast<const char*>(data),
        static_cast<std::size_t>(size)};
}

}  // namespace specforge
