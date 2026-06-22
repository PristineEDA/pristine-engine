cmake_minimum_required(VERSION 3.23)

project(
  pristine-engine
  VERSION 0.1.4
  DESCRIPTION "Standalone SystemVerilog language server"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PRISTINE_BUILD_TESTS "Build unit tests" ON)
option(PRISTINE_BUILD_PERF_TESTS "Build opt-in performance baseline tests" OFF)
option(PRISTINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(MSVC)
  add_compile_options(/EHsc /utf-8)
endif()

include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(Dependencies)

set(PRISTINE_FASTLZ_SOURCE_DIR "${PRISTINE_DEPS_DIR}/src/fastlz")
if(NOT EXISTS "${PRISTINE_FASTLZ_SOURCE_DIR}/fastlz.c")
  message(
    FATAL_ERROR
      "Missing dependency 'fastlz' in ${PRISTINE_FASTLZ_SOURCE_DIR}. Run `cmake -DPRISTINE_ROOT_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/scripts/bootstrap_deps.cmake` first.")
endif()
add_library(fastlz_static STATIC "${PRISTINE_FASTLZ_SOURCE_DIR}/fastlz.c")
target_include_directories(fastlz_static PUBLIC "${PRISTINE_FASTLZ_SOURCE_DIR}")
if(MSVC)
  target_compile_options(fastlz_static PRIVATE /wd4068)
endif()

set(PRISTINE_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated/include")
set(PRISTINE_GENERATED_SOURCE_DIR "${PROJECT_BINARY_DIR}/generated/src")
file(MAKE_DIRECTORY "${PRISTINE_GENERATED_INCLUDE_DIR}/pristine")
file(MAKE_DIRECTORY "${PRISTINE_GENERATED_SOURCE_DIR}")

function(pristine_git_path_to_absolute input output)
  if(NOT input)
    set("${output}" "" PARENT_SCOPE)
    return()
  endif()

  if(IS_ABSOLUTE "${input}")
    set(path "${input}")
  else()
    set(path "${PROJECT_SOURCE_DIR}/${input}")
  endif()
  get_filename_component(path "${path}" ABSOLUTE)
  set("${output}" "${path}" PARENT_SCOPE)
endfunction()

function(pristine_track_version_dependency path)
  if(path AND EXISTS "${path}")
    set_property(GLOBAL APPEND PROPERTY PRISTINE_VERSION_GIT_DEPENDS "${path}")
  endif()
endfunction()

function(pristine_track_git_metadata)
  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --git-path HEAD
    OUTPUT_VARIABLE PRISTINE_GIT_HEAD_PATH
    RESULT_VARIABLE PRISTINE_GIT_HEAD_RESULT
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(PRISTINE_GIT_HEAD_RESULT EQUAL 0 AND NOT "${PRISTINE_GIT_HEAD_PATH}" STREQUAL "")
    pristine_git_path_to_absolute("${PRISTINE_GIT_HEAD_PATH}" PRISTINE_GIT_HEAD_ABSOLUTE)
    pristine_track_version_dependency("${PRISTINE_GIT_HEAD_ABSOLUTE}")
  endif()

  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --git-path logs/HEAD
    OUTPUT_VARIABLE PRISTINE_GIT_HEAD_LOG_PATH
    RESULT_VARIABLE PRISTINE_GIT_HEAD_LOG_RESULT
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(PRISTINE_GIT_HEAD_LOG_RESULT EQUAL 0 AND NOT "${PRISTINE_GIT_HEAD_LOG_PATH}" STREQUAL "")
    pristine_git_path_to_absolute("${PRISTINE_GIT_HEAD_LOG_PATH}" PRISTINE_GIT_HEAD_LOG_ABSOLUTE)
    pristine_track_version_dependency("${PRISTINE_GIT_HEAD_LOG_ABSOLUTE}")
  endif()

  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" symbolic-ref -q HEAD
    OUTPUT_VARIABLE PRISTINE_GIT_REF_NAME
    RESULT_VARIABLE PRISTINE_GIT_REF_RESULT
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(PRISTINE_GIT_REF_RESULT EQUAL 0 AND NOT "${PRISTINE_GIT_REF_NAME}" STREQUAL "")
    execute_process(
      COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --git-path "${PRISTINE_GIT_REF_NAME}"
      OUTPUT_VARIABLE PRISTINE_GIT_REF_PATH
      RESULT_VARIABLE PRISTINE_GIT_REF_PATH_RESULT
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(PRISTINE_GIT_REF_PATH_RESULT EQUAL 0 AND NOT "${PRISTINE_GIT_REF_PATH}" STREQUAL "")
      pristine_git_path_to_absolute("${PRISTINE_GIT_REF_PATH}" PRISTINE_GIT_REF_ABSOLUTE)
      pristine_track_version_dependency("${PRISTINE_GIT_REF_ABSOLUTE}")
    endif()

    execute_process(
      COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --git-path "logs/${PRISTINE_GIT_REF_NAME}"
      OUTPUT_VARIABLE PRISTINE_GIT_REF_LOG_PATH
      RESULT_VARIABLE PRISTINE_GIT_REF_LOG_PATH_RESULT
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(PRISTINE_GIT_REF_LOG_PATH_RESULT EQUAL 0 AND NOT "${PRISTINE_GIT_REF_LOG_PATH}" STREQUAL "")
      pristine_git_path_to_absolute("${PRISTINE_GIT_REF_LOG_PATH}" PRISTINE_GIT_REF_LOG_ABSOLUTE)
      pristine_track_version_dependency("${PRISTINE_GIT_REF_LOG_ABSOLUTE}")
    endif()
  endif()

  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --git-path packed-refs
    OUTPUT_VARIABLE PRISTINE_GIT_PACKED_REFS_PATH
    RESULT_VARIABLE PRISTINE_GIT_PACKED_REFS_RESULT
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(PRISTINE_GIT_PACKED_REFS_RESULT EQUAL 0 AND NOT "${PRISTINE_GIT_PACKED_REFS_PATH}" STREQUAL "")
    pristine_git_path_to_absolute("${PRISTINE_GIT_PACKED_REFS_PATH}" PRISTINE_GIT_PACKED_REFS_ABSOLUTE)
    pristine_track_version_dependency("${PRISTINE_GIT_PACKED_REFS_ABSOLUTE}")
  endif()
endfunction()

pristine_track_git_metadata()
get_property(PRISTINE_VERSION_GIT_DEPENDS GLOBAL PROPERTY PRISTINE_VERSION_GIT_DEPENDS)

if(WIN32)
  set(PRISTINE_BUILD_OS "windows")
elseif(APPLE)
  set(PRISTINE_BUILD_OS "macos")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(PRISTINE_BUILD_OS "linux")
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" PRISTINE_BUILD_OS)
  if(NOT PRISTINE_BUILD_OS)
    set(PRISTINE_BUILD_OS "unknown")
  endif()
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" PRISTINE_SYSTEM_PROCESSOR_LOWER)
if(PRISTINE_SYSTEM_PROCESSOR_LOWER MATCHES "^(aarch64|arm64)$")
  set(PRISTINE_BUILD_ARCH "arm64")
elseif(PRISTINE_SYSTEM_PROCESSOR_LOWER MATCHES "^(x86_64|amd64)$")
  set(PRISTINE_BUILD_ARCH "x64")
elseif(PRISTINE_SYSTEM_PROCESSOR_LOWER MATCHES "^(i[3-6]86|x86)$")
  set(PRISTINE_BUILD_ARCH "x32")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(PRISTINE_BUILD_ARCH "x64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(PRISTINE_BUILD_ARCH "x32")
else()
  set(PRISTINE_BUILD_ARCH "${PRISTINE_SYSTEM_PROCESSOR_LOWER}")
  if(NOT PRISTINE_BUILD_ARCH)
    set(PRISTINE_BUILD_ARCH "unknown")
  endif()
endif()

string(TOLOWER "${CMAKE_BUILD_TYPE}" PRISTINE_BUILD_TYPE)
if(NOT PRISTINE_BUILD_TYPE)
  set(PRISTINE_BUILD_TYPE "unknown")
endif()

set(PRISTINE_VERSION_HEADER "${PRISTINE_GENERATED_INCLUDE_DIR}/pristine/Version.h")
set(PRISTINE_VERSION_SOURCE "${PRISTINE_GENERATED_SOURCE_DIR}/Version.cpp")
add_custom_command(
  OUTPUT "${PRISTINE_VERSION_HEADER}" "${PRISTINE_VERSION_SOURCE}"
  COMMAND
    ${CMAKE_COMMAND} -DPRISTINE_SOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DPRISTINE_HEADER_FILE=${PRISTINE_VERSION_HEADER}
    -DPRISTINE_SOURCE_FILE=${PRISTINE_VERSION_SOURCE}
    -DPRISTINE_PROJECT_VERSION=${PROJECT_VERSION}
    -DPRISTINE_BUILD_OS=${PRISTINE_BUILD_OS}
    -DPRISTINE_BUILD_ARCH=${PRISTINE_BUILD_ARCH}
    -DPRISTINE_BUILD_TYPE=${PRISTINE_BUILD_TYPE} -P
    ${PROJECT_SOURCE_DIR}/scripts/generate-version.cmake
  DEPENDS ${PROJECT_SOURCE_DIR}/scripts/generate-version.cmake ${PRISTINE_VERSION_GIT_DEPENDS}
  COMMENT "Generating pristine version sources"
  VERBATIM)

set(PRISTINE_NOTICE_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/pristine-engine/licenses")

set(PRISTINE_NOTICE_DEPENDS
    ${PROJECT_SOURCE_DIR}/cmake/attributions.cmake
    ${PROJECT_SOURCE_DIR}/scripts/generate-notice.cmake
    ${PROJECT_SOURCE_DIR}/licenses/texts/BSD-2-Clause-LZ4.txt
    ${PROJECT_SOURCE_DIR}/licenses/texts/BSL-1.0.txt
    ${PROJECT_SOURCE_DIR}/licenses/texts/FastLZ-MIT.txt
    ${PROJECT_SOURCE_DIR}/licenses/texts/MIT.txt
    ${PROJECT_SOURCE_DIR}/licenses/texts/Zlib.txt
    ${PROJECT_SOURCE_DIR}/licenses/texts/fmt-license.txt)

add_custom_target(
  pristine_generate_notice
  COMMAND ${CMAKE_COMMAND} -DPRISTINE_ROOT_DIR=${PROJECT_SOURCE_DIR} -P
          ${PROJECT_SOURCE_DIR}/scripts/generate-notice.cmake
  DEPENDS ${PRISTINE_NOTICE_DEPENDS}
  COMMENT "Generating third-party notice files"
  VERBATIM)

add_custom_target(
  pristine_validate_notice
  COMMAND ${CMAKE_COMMAND} -DPRISTINE_ROOT_DIR=${PROJECT_SOURCE_DIR}
          -DPRISTINE_VALIDATE_ONLY=ON -P
          ${PROJECT_SOURCE_DIR}/scripts/generate-notice.cmake
  DEPENDS
    ${PROJECT_SOURCE_DIR}/ATTRIBUTIONS.md
    ${PROJECT_SOURCE_DIR}/NOTICE
    ${PRISTINE_NOTICE_DEPENDS}
  COMMENT "Validating third-party notice files"
  VERBATIM)

add_library(
  pristine_core
  src/analysis/CompilationService.cpp
  src/analysis/SemanticEngine.cpp
  src/analysis/SemanticWorkspace.cpp
  src/analysis/SourceUtil.cpp
  src/analysis/SyntaxDocumentCache.cpp
  src/analysis/semantic/AstIndex.cpp
  src/analysis/semantic/CodeActionProvider.cpp
  src/analysis/semantic/CompletionProvider.cpp
  src/analysis/semantic/DebugTrace.cpp
  src/analysis/semantic/DesignGraphProvider.cpp
  src/analysis/semantic/DiagnosticProvider.cpp
  src/analysis/semantic/NavigationProvider.cpp
  src/analysis/semantic/QueryCache.cpp
  src/analysis/semantic/SignatureInlayProvider.cpp
  src/analysis/semantic/SnapshotBuilder.cpp
  src/analysis/semantic/WorkspaceDiscoveryIndex.cpp
  src/document/DocumentStore.cpp
  src/jsonrpc/JsonRpcServer.cpp
  src/jsonrpc/MessageStream.cpp
  src/layout/LayoutBinaryProtocol.cpp
  src/layout/LayoutParser.cpp
  src/layout/LayoutPipeService.cpp
  src/layout/LayoutSpatialIndex.cpp
  src/layout/LayoutSource.cpp
  src/lsp/Protocol.cpp
  src/server/ServerSession.cpp
  src/transport/StdioTransport.cpp
  src/waveform/WaveformBinaryProtocol.cpp
  src/waveform/FstWaveformSource.cpp
  src/waveform/WaveformMockGenerator.cpp
  src/waveform/WaveformPipeService.cpp
  src/waveform/WaveformSource.cpp
  src/waveform/WaveformViewportEncoder.cpp
  src/waveform/fst/FstReader.cpp
  src/workspace/WorkspaceManager.cpp
  "${PRISTINE_VERSION_SOURCE}")

target_include_directories(
  pristine_core PUBLIC "${PROJECT_SOURCE_DIR}/include" "${PRISTINE_GENERATED_INCLUDE_DIR}")
target_link_libraries(
  pristine_core PUBLIC nlohmann_json::nlohmann_json slang::slang zlibstatic lz4_static
                       fastlz_static pristine_boost_headers)

if(MSVC)
  target_compile_options(pristine_core PRIVATE /W4 /permissive-)
  if(PRISTINE_WARNINGS_AS_ERRORS)
    target_compile_options(pristine_core PRIVATE /WX)
  endif()
else()
  target_compile_options(pristine_core PRIVATE -Wall -Wextra -Wpedantic)
  if(PRISTINE_WARNINGS_AS_ERRORS)
    target_compile_options(pristine_core PRIVATE -Werror)
  endif()
endif()

add_executable(pristine-engine src/main.cpp)
target_link_libraries(pristine-engine PRIVATE pristine_core)

install(TARGETS pristine-engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
  FILES
    ${PROJECT_SOURCE_DIR}/LICENSE
    ${PROJECT_SOURCE_DIR}/ATTRIBUTIONS.md
    ${PROJECT_SOURCE_DIR}/NOTICE
  DESTINATION ${PRISTINE_NOTICE_INSTALL_DIR})

if(PRISTINE_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()
