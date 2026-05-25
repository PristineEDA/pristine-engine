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

pristine_require_dependency(nlohmann_json)

if(PRISTINE_BUILD_TESTS)
  pristine_require_dependency(catch2)
endif()