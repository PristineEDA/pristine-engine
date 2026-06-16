set(PRISTINE_DEPS_DIR "${PROJECT_SOURCE_DIR}/.deps" CACHE PATH "Local dependency source cache")

include("${PROJECT_SOURCE_DIR}/cmake/DepsLock.cmake")

function(pristine_require_dependency dependency_name)
  set(dependency_dir "${PRISTINE_DEPS_DIR}/src/${dependency_name}")
  set(dependency_cmake_subdir_var "PRISTINE_DEP_${dependency_name}_CMAKE_SUBDIR")
  set(dependency_cmake_subdir "")
  if(DEFINED ${dependency_cmake_subdir_var})
    set(dependency_cmake_subdir "${${dependency_cmake_subdir_var}}")
  endif()
  if(dependency_cmake_subdir)
    set(dependency_cmake_dir "${dependency_dir}/${dependency_cmake_subdir}")
  else()
    set(dependency_cmake_dir "${dependency_dir}")
  endif()

  if(NOT EXISTS "${dependency_cmake_dir}/CMakeLists.txt")
    message(
      FATAL_ERROR
        "Missing dependency '${dependency_name}' in ${dependency_dir}. Run `cmake -DPRISTINE_ROOT_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/scripts/bootstrap_deps.cmake` first.")
  endif()

  add_subdirectory("${dependency_cmake_dir}" "${PROJECT_BINARY_DIR}/_deps/${dependency_name}" EXCLUDE_FROM_ALL)
endfunction()

set(pristine_fmt_dir "${PRISTINE_DEPS_DIR}/src/fmt")
if(NOT EXISTS "${pristine_fmt_dir}/CMakeLists.txt")
  message(
    FATAL_ERROR
      "Missing dependency 'fmt' in ${pristine_fmt_dir}. Run `cmake -DPRISTINE_ROOT_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/scripts/bootstrap_deps.cmake` first.")
endif()

set(FETCHCONTENT_SOURCE_DIR_FMT "${pristine_fmt_dir}" CACHE PATH "Use locally bootstrapped fmt for slang" FORCE)
set(FETCHCONTENT_UPDATES_DISCONNECTED_FMT ON CACHE BOOL "Disable fmt updates during configure" FORCE)

set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "Don't build slang tools" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "Don't build slang docs" FORCE)
set(SLANG_INCLUDE_PYLIB OFF CACHE BOOL "Don't build slang Python bindings" FORCE)
set(SLANG_INCLUDE_INSTALL OFF CACHE BOOL "Don't install slang separately" FORCE)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "Don't build slang tests" FORCE)
set(SLANG_USE_MIMALLOC OFF CACHE BOOL "Don't let slang fetch mimalloc during configure" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Do not build zlib examples" FORCE)
set(LZ4_BUILD_CLI OFF CACHE BOOL "Do not build LZ4 CLI" FORCE)
set(LZ4_BUNDLED_MODE ON CACHE BOOL "Build LZ4 as a bundled dependency" FORCE)

set(pristine_git_find_package_was_defined FALSE)
if(DEFINED CMAKE_DISABLE_FIND_PACKAGE_Git)
  set(pristine_git_find_package_was_defined TRUE)
  set(pristine_disable_find_package_git "${CMAKE_DISABLE_FIND_PACKAGE_Git}")
endif()

# Vendored slang source trees are unpacked under this repository, so allowing
# Git discovery makes its version probe climb into the pristine-engine repo and
# pick up unrelated tags.
set(CMAKE_DISABLE_FIND_PACKAGE_Git ON)
pristine_require_dependency(slang)
if(pristine_git_find_package_was_defined)
  set(CMAKE_DISABLE_FIND_PACKAGE_Git "${pristine_disable_find_package_git}")
else()
  unset(CMAKE_DISABLE_FIND_PACKAGE_Git)
endif()

pristine_require_dependency(nlohmann_json)
pristine_require_dependency(zlib)
pristine_require_dependency(lz4)

set(pristine_boost_dir "${PRISTINE_DEPS_DIR}/src/boost")
if(NOT EXISTS "${pristine_boost_dir}/boost/geometry.hpp")
  message(
    FATAL_ERROR
      "Missing dependency 'boost' in ${pristine_boost_dir}. Run `cmake -DPRISTINE_ROOT_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/scripts/bootstrap_deps.cmake` first.")
endif()
add_library(pristine_boost_headers INTERFACE)
target_include_directories(pristine_boost_headers SYSTEM INTERFACE "${pristine_boost_dir}")

if(PRISTINE_BUILD_TESTS)
  pristine_require_dependency(catch2)
endif()

if(PRISTINE_BUILD_PERF_TESTS)
  set(LSP_BUILD_EXAMPLES OFF CACHE BOOL "Do not build lsp-framework examples" FORCE)
  set(LSP_INSTALL OFF CACHE BOOL "Do not install lsp-framework test dependency" FORCE)
  pristine_require_dependency(lsp_framework)
endif()
