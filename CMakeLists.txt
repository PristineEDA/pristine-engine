cmake_minimum_required(VERSION 3.23)

project(
  pristine-engine
  VERSION 0.1.0
  DESCRIPTION "Standalone SystemVerilog language server"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PRISTINE_BUILD_TESTS "Build unit tests" ON)
option(PRISTINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(MSVC)
  add_compile_options(/EHsc /utf-8)
endif()

include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(Dependencies)

add_library(
  pristine_core
  src/analysis/CompilationService.cpp
  src/document/DocumentStore.cpp
  src/jsonrpc/JsonRpcServer.cpp
  src/jsonrpc/MessageStream.cpp
  src/lsp/Protocol.cpp
  src/server/ServerSession.cpp
  src/transport/StdioTransport.cpp
  src/workspace/WorkspaceManager.cpp)

target_include_directories(pristine_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
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

add_executable(pristine-lsp src/main.cpp)
target_link_libraries(pristine-lsp PRIVATE pristine_core)

install(TARGETS pristine-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(PRISTINE_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()