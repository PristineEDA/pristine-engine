set(PRISTINE_DEPS_DIR "${PROJECT_SOURCE_DIR}/.deps" CACHE PATH "Local dependency source cache")

function(pristine_require_dependency dependency_name)
  set(dependency_dir "${PRISTINE_DEPS_DIR}/src/${dependency_name}")

  if(NOT EXISTS "${dependency_dir}/CMakeLists.txt")
    message(
      FATAL_ERROR
        "Missing dependency '${dependency_name}' in ${dependency_dir}. Run `cmake -DPRISTINE_ROOT_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/scripts/bootstrap_deps.cmake` first.")
  endif()

  add_subdirectory("${dependency_dir}" "${PROJECT_BINARY_DIR}/_deps/${dependency_name}" EXCLUDE_FROM_ALL)
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

if(PRISTINE_BUILD_TESTS)
  pristine_require_dependency(catch2)
endif()

if(PRISTINE_BUILD_PERF_TESTS)
  set(LSP_BUILD_EXAMPLES OFF CACHE BOOL "Do not build lsp-framework examples" FORCE)
  set(LSP_INSTALL OFF CACHE BOOL "Do not install lsp-framework test dependency" FORCE)
  pristine_require_dependency(lsp_framework)
endif()
