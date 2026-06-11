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
  set(PRISTINE_COMPONENTS
    slang
    fmt
    zlib
    lz4
    fastlz
    wellen
    nlohmann_json
    catch2)
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
  if(dependency_sha256 MATCHES "^PLACEHOLDER_")
    message(FATAL_ERROR "Dependency '${dependency_name}' has no pinned SHA256 yet; update cmake/DepsLock.cmake before bootstrapping it.")
  endif()
  set(dependency_archive_name "${PRISTINE_DEP_${dependency_name}_ARCHIVE_NAME}")
  set(dependency_cmake_subdir_var "PRISTINE_DEP_${dependency_name}_CMAKE_SUBDIR")
  set(dependency_allow_no_cmake_var "PRISTINE_DEP_${dependency_name}_ALLOW_NO_CMAKE")
  set(dependency_source_dir_name_var "PRISTINE_DEP_${dependency_name}_SOURCE_DIR_NAME")
  set(dependency_cmake_subdir "")
  set(dependency_allow_no_cmake FALSE)
  set(dependency_source_dir_name "${dependency_name}")
  if(DEFINED ${dependency_cmake_subdir_var})
    set(dependency_cmake_subdir "${${dependency_cmake_subdir_var}}")
  endif()
  if(DEFINED ${dependency_allow_no_cmake_var})
    set(dependency_allow_no_cmake "${${dependency_allow_no_cmake_var}}")
  endif()
  if(DEFINED ${dependency_source_dir_name_var})
    set(dependency_source_dir_name "${${dependency_source_dir_name_var}}")
  endif()
  set(archive_path "${PRISTINE_DEPS_DIR}/archives/${dependency_archive_name}")
  set(source_path "${PRISTINE_DEPS_DIR}/src/${dependency_source_dir_name}")
  if(dependency_cmake_subdir)
    set(cmake_probe_path "${source_path}/${dependency_cmake_subdir}/CMakeLists.txt")
  else()
    set(cmake_probe_path "${source_path}/CMakeLists.txt")
  endif()
  set(staging_path "${PRISTINE_DEPS_DIR}/staging/${dependency_name}")

  if(EXISTS "${cmake_probe_path}" OR (dependency_allow_no_cmake AND EXISTS "${source_path}"))
    message(STATUS "Dependency '${dependency_name}' already bootstrapped")
    continue()
  endif()

  if(EXISTS "${archive_path}")
    file(SHA256 "${archive_path}" archive_sha256)
    string(TOLOWER "${archive_sha256}" archive_sha256_lower)
    string(TOLOWER "${dependency_sha256}" dependency_sha256_lower)
    if(NOT archive_sha256_lower STREQUAL dependency_sha256_lower)
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
