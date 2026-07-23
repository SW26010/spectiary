function(specforge_read_vcpkg_package_version package_name output_variable)
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR VCPKG_INSTALLED_DIR STREQUAL "")
        message(FATAL_ERROR
            "VCPKG_INSTALLED_DIR is required to resolve the installed ${package_name} version."
        )
    endif()
    if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
        message(FATAL_ERROR
            "VCPKG_TARGET_TRIPLET is required to resolve the installed ${package_name} version."
        )
    endif()

    set(spdx_path
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${package_name}/vcpkg.spdx.json"
    )
    if(NOT EXISTS "${spdx_path}")
        message(FATAL_ERROR
            "Installed vcpkg SPDX metadata was not found for ${package_name}: ${spdx_path}"
        )
    endif()

    file(READ "${spdx_path}" spdx_json)
    string(JSON package_count LENGTH "${spdx_json}" packages)
    if(package_count EQUAL 0)
        message(FATAL_ERROR "No packages were recorded in ${spdx_path}.")
    endif()

    math(EXPR last_package_index "${package_count} - 1")
    foreach(package_index RANGE 0 ${last_package_index})
        string(JSON installed_name GET "${spdx_json}" packages ${package_index} name)
        if(installed_name STREQUAL package_name)
            string(JSON installed_version GET "${spdx_json}" packages ${package_index} versionInfo)
            string(REGEX REPLACE "#[0-9]+$" "" upstream_version "${installed_version}")
            if(upstream_version STREQUAL "")
                message(FATAL_ERROR
                    "Installed vcpkg SPDX metadata has an empty version for ${package_name}: ${spdx_path}"
                )
            endif()
            set(${output_variable} "${upstream_version}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    message(FATAL_ERROR
        "Installed vcpkg SPDX metadata does not contain package '${package_name}': ${spdx_path}"
    )
endfunction()

function(specforge_require_notice_heading notice_path component_name expected_heading)
    if(NOT EXISTS "${notice_path}")
        message(FATAL_ERROR "Third-party notice file was not found: ${notice_path}")
    endif()

    file(STRINGS "${notice_path}" matching_headings REGEX "^${component_name} [0-9]")
    list(LENGTH matching_headings heading_count)
    if(NOT heading_count EQUAL 1)
        message(FATAL_ERROR
            "${notice_path} must contain exactly one heading beginning '${component_name} '; "
            "found ${heading_count}."
        )
    endif()

    list(GET matching_headings 0 actual_heading)
    if(NOT actual_heading STREQUAL expected_heading)
        message(FATAL_ERROR
            "${notice_path} is stale for ${component_name}. "
            "Expected '${expected_heading}', found '${actual_heading}'. "
            "Review the installed package license and update the checked-in notice."
        )
    endif()
endfunction()
