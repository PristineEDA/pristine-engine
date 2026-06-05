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
