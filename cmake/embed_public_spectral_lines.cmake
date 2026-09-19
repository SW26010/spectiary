if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()

file(READ "${INPUT}" catalog_content)
string(REPLACE "\r\n" "\n" catalog_content "${catalog_content}")
string(REPLACE "\r" "\n" catalog_content "${catalog_content}")
string(REPLACE "\\" "\\\\" catalog_content "${catalog_content}")
string(REPLACE "\"" "\\\"" catalog_content "${catalog_content}")
string(REPLACE "\n" "\\n\"\n    \"" catalog_content "${catalog_content}")

file(WRITE "${OUTPUT}" "#pragma once

namespace spectiary {

inline constexpr const char kEmbeddedPublicSpectralLineCatalog[] =
    \"${catalog_content}\";

}  // namespace spectiary
")
