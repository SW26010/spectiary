# Explicit independent contracts; never derive storage or identity from display text.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../config/project_identity.json" project_identity_json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../config/project_identity.json")
foreach(field IN ITEMS founding_identity product_display_name local_app_data_leaf
        artifact_basename metadata_filename shell_identity)
    string(JSON PROJECT_${field} GET "${project_identity_json}" ${field})
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/project_identity.h.in"
    "${CMAKE_BINARY_DIR}/generated/app/project_identity.h" @ONLY)
