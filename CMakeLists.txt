cmake_minimum_required(VERSION 3.23)

project(
  pristine-engine
  VERSION 0.1.1
  DESCRIPTION "Standalone SystemVerilog language server"
  LANGUAGES CXX)

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

set(PRISTINE_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${PRISTINE_GENERATED_INCLUDE_DIR}/pristine")

function(pristine_normalize_version_literal input output)
  set(value "${input}")
  string(REPLACE "\\" "\\\\" value "${value}")
  string(REPLACE "\"" "\\\"" value "${value}")
  string(REPLACE "\n" " " value "${value}")
  string(REPLACE "\r" " " value "${value}")
  set("${output}" "${value}" PARENT_SCOPE)
endfunction()

set(PRISTINE_GIT_BRANCH "$ENV{GITHUB_HEAD_REF}")
if(NOT PRISTINE_GIT_BRANCH)
  set(PRISTINE_GIT_BRANCH "$ENV{GITHUB_REF_NAME}")
endif()
if(NOT PRISTINE_GIT_BRANCH)
  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --abbrev-ref HEAD
    OUTPUT_VARIABLE PRISTINE_GIT_BRANCH
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(NOT PRISTINE_GIT_BRANCH)
  set(PRISTINE_GIT_BRANCH "unknown")
elseif(PRISTINE_GIT_BRANCH STREQUAL "HEAD")
  set(PRISTINE_GIT_BRANCH "detached")
endif()

set(PRISTINE_GIT_COMMIT "$ENV{GITHUB_SHA}")
if(NOT PRISTINE_GIT_COMMIT)
  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE PRISTINE_GIT_COMMIT
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(NOT PRISTINE_GIT_COMMIT)
  set(PRISTINE_GIT_COMMIT "unknown")
endif()
if(NOT PRISTINE_GIT_COMMIT STREQUAL "unknown")
  string(SUBSTRING "${PRISTINE_GIT_COMMIT}" 0 12 PRISTINE_GIT_COMMIT)
endif()

if("$ENV{GITHUB_REF_TYPE}" STREQUAL "tag" AND "$ENV{GITHUB_REF_NAME}")
  set(PRISTINE_GIT_TAG "$ENV{GITHUB_REF_NAME}")
else()
  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" describe --tags --exact-match HEAD
    OUTPUT_VARIABLE PRISTINE_GIT_TAG
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT PRISTINE_GIT_TAG)
    set(PRISTINE_GIT_TAG "none")
  endif()
endif()

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

pristine_normalize_version_literal("${PRISTINE_GIT_BRANCH}" PRISTINE_GIT_BRANCH_LITERAL)
pristine_normalize_version_literal("${PRISTINE_GIT_COMMIT}" PRISTINE_GIT_COMMIT_LITERAL)
pristine_normalize_version_literal("${PRISTINE_GIT_TAG}" PRISTINE_GIT_TAG_LITERAL)
pristine_normalize_version_literal("${PRISTINE_BUILD_OS}" PRISTINE_BUILD_OS_LITERAL)
pristine_normalize_version_literal("${PRISTINE_BUILD_ARCH}" PRISTINE_BUILD_ARCH_LITERAL)
pristine_normalize_version_literal("${PRISTINE_BUILD_TYPE}" PRISTINE_BUILD_TYPE_LITERAL)

configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/Version.h.in"
  "${PRISTINE_GENERATED_INCLUDE_DIR}/pristine/Version.h"
  @ONLY)

set(PRISTINE_NOTICE_OUTPUTS
    "${PROJECT_SOURCE_DIR}/ATTRIBUTIONS.md"
    "${PROJECT_SOURCE_DIR}/NOTICE")
set(PRISTINE_NOTICE_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/pristine-engine/licenses")

add_custom_command(
  OUTPUT ${PRISTINE_NOTICE_OUTPUTS}
  COMMAND ${CMAKE_COMMAND} -DPRISTINE_ROOT_DIR=${PROJECT_SOURCE_DIR} -P
          ${PROJECT_SOURCE_DIR}/scripts/generate-notice.cmake
  DEPENDS
    ${PROJECT_SOURCE_DIR}/cmake/attributions.cmake
    ${PROJECT_SOURCE_DIR}/scripts/generate-notice.cmake
    ${PROJECT_SOURCE_DIR}/licenses/texts/BSL-1.0.txt
    ${PROJECT_SOURCE_DIR}/licenses/texts/MIT.txt
    ${PROJECT_SOURCE_DIR}/licenses/texts/fmt-license.txt
  COMMENT "Generating third-party notice files"
  VERBATIM)

add_custom_target(pristine_generate_notice DEPENDS ${PRISTINE_NOTICE_OUTPUTS})

add_custom_target(
  pristine_validate_notice
  COMMAND ${CMAKE_COMMAND} -DPRISTINE_ROOT_DIR=${PROJECT_SOURCE_DIR}
          -DPRISTINE_VALIDATE_ONLY=ON -P
          ${PROJECT_SOURCE_DIR}/scripts/generate-notice.cmake
  DEPENDS
    ${PROJECT_SOURCE_DIR}/ATTRIBUTIONS.md
    ${PROJECT_SOURCE_DIR}/NOTICE
    ${PROJECT_SOURCE_DIR}/cmake/attributions.cmake
    ${PROJECT_SOURCE_DIR}/scripts/generate-notice.cmake
    ${PROJECT_SOURCE_DIR}/licenses/texts/BSL-1.0.txt
    ${PROJECT_SOURCE_DIR}/licenses/texts/MIT.txt
    ${PROJECT_SOURCE_DIR}/licenses/texts/fmt-license.txt
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
  src/lsp/Protocol.cpp
  src/server/ServerSession.cpp
  src/transport/StdioTransport.cpp
  src/workspace/WorkspaceManager.cpp)

target_include_directories(
  pristine_core PUBLIC "${PROJECT_SOURCE_DIR}/include" "${PRISTINE_GENERATED_INCLUDE_DIR}")
target_link_libraries(pristine_core PUBLIC nlohmann_json::nlohmann_json slang::slang)

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
