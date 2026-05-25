cmake_minimum_required(VERSION 3.23)

if(NOT DEFINED PRISTINE_ROOT_DIR)
  get_filename_component(PRISTINE_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
else()
  get_filename_component(PRISTINE_ROOT_DIR "${PRISTINE_ROOT_DIR}" ABSOLUTE)
endif()

if(NOT DEFINED PRISTINE_DEPS_DIR)
  set(PRISTINE_DEPS_DIR "${PRISTINE_ROOT_DIR}/.deps")
endif()

include("${PRISTINE_ROOT_DIR}/cmake/DepsLock.cmake")

if(NOT DEFINED PRISTINE_COMPONENTS)
  set(PRISTINE_COMPONENTS ${PRISTINE_DEPENDENCY_NAMES})
endif()

file(MAKE_DIRECTORY "${PRISTINE_DEPS_DIR}/archives")
file(MAKE_DIRECTORY "${PRISTINE_DEPS_DIR}/src")
file(MAKE_DIRECTORY "${PRISTINE_DEPS_DIR}/staging")

foreach(dependency_name IN LISTS PRISTINE_COMPONENTS)
  list(FIND PRISTINE_DEPENDENCY_NAMES "${dependency_name}" dependency_index)
  if(dependency_index EQUAL -1)
    message(FATAL_ERROR "Unknown dependency '${dependency_name}'")
  endif()

  set(dependency_url "${PRISTINE_DEP_${dependency_name}_URL}")
  set(dependency_sha256 "${PRISTINE_DEP_${dependency_name}_SHA256}")
  set(dependency_archive_name "${PRISTINE_DEP_${dependency_name}_ARCHIVE_NAME}")
  set(archive_path "${PRISTINE_DEPS_DIR}/archives/${dependency_archive_name}")
  set(source_path "${PRISTINE_DEPS_DIR}/src/${dependency_name}")
  set(staging_path "${PRISTINE_DEPS_DIR}/staging/${dependency_name}")

  if(EXISTS "${source_path}/CMakeLists.txt")
    message(STATUS "Dependency '${dependency_name}' already bootstrapped")
    continue()
  endif()

  if(EXISTS "${archive_path}")
    file(SHA256 "${archive_path}" archive_sha256)
    if(NOT archive_sha256 STREQUAL dependency_sha256)
      file(REMOVE "${archive_path}")
    endif()
  endif()

  if(NOT EXISTS "${archive_path}")
    message(STATUS "Downloading ${dependency_name} from ${dependency_url}")
    file(
      DOWNLOAD
      "${dependency_url}"
      "${archive_path}"
      EXPECTED_HASH "SHA256=${dependency_sha256}"
      SHOW_PROGRESS
      STATUS download_status)
    list(GET download_status 0 download_code)
    list(GET download_status 1 download_message)
    if(NOT download_code EQUAL 0)
      message(FATAL_ERROR "Failed to download '${dependency_name}': ${download_message}")
    endif()
  endif()

  file(REMOVE_RECURSE "${staging_path}")
  file(MAKE_DIRECTORY "${staging_path}")
  file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${staging_path}")

  file(GLOB extracted_entries RELATIVE "${staging_path}" "${staging_path}/*")
  list(LENGTH extracted_entries extracted_count)
  if(NOT extracted_count EQUAL 1)
    message(FATAL_ERROR "Archive '${dependency_name}' extracted to an unexpected layout")
  endif()

  list(GET extracted_entries 0 extracted_root)
  file(REMOVE_RECURSE "${source_path}")
  file(RENAME "${staging_path}/${extracted_root}" "${source_path}")
  file(REMOVE_RECURSE "${staging_path}")

  message(STATUS "Bootstrapped '${dependency_name}' into ${source_path}")
endforeach()