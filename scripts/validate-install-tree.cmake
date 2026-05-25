cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED PRISTINE_INSTALL_PREFIX)
  message(FATAL_ERROR "PRISTINE_INSTALL_PREFIX must be provided")
endif()

file(TO_CMAKE_PATH "${PRISTINE_INSTALL_PREFIX}" PRISTINE_INSTALL_PREFIX)
if(NOT IS_ABSOLUTE "${PRISTINE_INSTALL_PREFIX}")
  get_filename_component(
    PRISTINE_INSTALL_PREFIX
    "${CMAKE_CURRENT_LIST_DIR}/../${PRISTINE_INSTALL_PREFIX}"
    ABSOLUTE)
endif()

if(CMAKE_HOST_WIN32)
  set(pristine_binary_name "pristine-lsp.exe")
else()
  set(pristine_binary_name "pristine-lsp")
endif()

set(pristine_notice_dir "${PRISTINE_INSTALL_PREFIX}/share/pristine-engine/licenses")
set(pristine_expected_paths
    "${PRISTINE_INSTALL_PREFIX}/bin/${pristine_binary_name}"
    "${pristine_notice_dir}/LICENSE"
    "${pristine_notice_dir}/ATTRIBUTIONS.md"
    "${pristine_notice_dir}/NOTICE")

foreach(expected_path IN LISTS pristine_expected_paths)
  if(NOT EXISTS "${expected_path}")
    message(FATAL_ERROR "Missing installed artifact: ${expected_path}")
  endif()
  message(STATUS "Found installed artifact: ${expected_path}")
endforeach()