function(pristine_normalize_version_literal input output)
  set(value "${input}")
  string(REPLACE "\\" "\\\\" value "${value}")
  string(REPLACE "\"" "\\\"" value "${value}")
  string(REPLACE "\n" " " value "${value}")
  string(REPLACE "\r" " " value "${value}")
  set("${output}" "${value}" PARENT_SCOPE)
endfunction()

set(PRISTINE_IN_GITHUB_ACTIONS FALSE)
if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
  set(PRISTINE_IN_GITHUB_ACTIONS TRUE)
endif()

set(PRISTINE_GIT_BRANCH "")
if(PRISTINE_IN_GITHUB_ACTIONS)
  set(PRISTINE_GIT_BRANCH "$ENV{GITHUB_HEAD_REF}")
  if(NOT PRISTINE_GIT_BRANCH)
    set(PRISTINE_GIT_BRANCH "$ENV{GITHUB_REF_NAME}")
  endif()
endif()
if(NOT PRISTINE_GIT_BRANCH)
  execute_process(
    COMMAND git -C "${PRISTINE_SOURCE_DIR}" rev-parse --abbrev-ref HEAD
    OUTPUT_VARIABLE PRISTINE_GIT_BRANCH
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(NOT PRISTINE_GIT_BRANCH)
  set(PRISTINE_GIT_BRANCH "unknown")
elseif(PRISTINE_GIT_BRANCH STREQUAL "HEAD")
  set(PRISTINE_GIT_BRANCH "detached")
endif()

set(PRISTINE_GIT_COMMIT "")
if(PRISTINE_IN_GITHUB_ACTIONS)
  set(PRISTINE_GIT_COMMIT "$ENV{GITHUB_SHA}")
endif()
if(NOT PRISTINE_GIT_COMMIT)
  execute_process(
    COMMAND git -C "${PRISTINE_SOURCE_DIR}" rev-parse HEAD
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

if(PRISTINE_IN_GITHUB_ACTIONS AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag" AND "$ENV{GITHUB_REF_NAME}")
  set(PRISTINE_GIT_TAG "$ENV{GITHUB_REF_NAME}")
else()
  execute_process(
    COMMAND git -C "${PRISTINE_SOURCE_DIR}" describe --tags --exact-match HEAD
    OUTPUT_VARIABLE PRISTINE_GIT_TAG
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT PRISTINE_GIT_TAG)
    set(PRISTINE_GIT_TAG "none")
  endif()
endif()

pristine_normalize_version_literal("${PRISTINE_GIT_BRANCH}" PRISTINE_GIT_BRANCH_LITERAL)
pristine_normalize_version_literal("${PRISTINE_GIT_COMMIT}" PRISTINE_GIT_COMMIT_LITERAL)
pristine_normalize_version_literal("${PRISTINE_GIT_TAG}" PRISTINE_GIT_TAG_LITERAL)
pristine_normalize_version_literal("${PRISTINE_BUILD_OS}" PRISTINE_BUILD_OS_LITERAL)
pristine_normalize_version_literal("${PRISTINE_BUILD_ARCH}" PRISTINE_BUILD_ARCH_LITERAL)
pristine_normalize_version_literal("${PRISTINE_BUILD_TYPE}" PRISTINE_BUILD_TYPE_LITERAL)

set(header_content "#pragma once

#include <string_view>

namespace pristine {

extern const char kVersionString[];
extern const std::string_view kVersion;
extern const char kGitBranchString[];
extern const std::string_view kGitBranch;
extern const char kGitCommitString[];
extern const std::string_view kGitCommit;
extern const char kGitTagString[];
extern const std::string_view kGitTag;
extern const char kBuildOsString[];
extern const std::string_view kBuildOs;
extern const char kBuildArchString[];
extern const std::string_view kBuildArch;
extern const char kBuildTypeString[];
extern const std::string_view kBuildType;
extern const char kVersionLineString[];
extern const std::string_view kVersionLine;

} // namespace pristine
")

set(source_content "#include \"pristine/Version.h\"

namespace pristine {

const char kVersionString[] = \"${PRISTINE_PROJECT_VERSION}\";
const std::string_view kVersion = kVersionString;
const char kGitBranchString[] = \"${PRISTINE_GIT_BRANCH_LITERAL}\";
const std::string_view kGitBranch = kGitBranchString;
const char kGitCommitString[] = \"${PRISTINE_GIT_COMMIT_LITERAL}\";
const std::string_view kGitCommit = kGitCommitString;
const char kGitTagString[] = \"${PRISTINE_GIT_TAG_LITERAL}\";
const std::string_view kGitTag = kGitTagString;
const char kBuildOsString[] = \"${PRISTINE_BUILD_OS_LITERAL}\";
const std::string_view kBuildOs = kBuildOsString;
const char kBuildArchString[] = \"${PRISTINE_BUILD_ARCH_LITERAL}\";
const std::string_view kBuildArch = kBuildArchString;
const char kBuildTypeString[] = \"${PRISTINE_BUILD_TYPE_LITERAL}\";
const std::string_view kBuildType = kBuildTypeString;
const char kVersionLineString[] =
    \"pristine-engine ${PRISTINE_PROJECT_VERSION} branch=${PRISTINE_GIT_BRANCH_LITERAL} \"
    \"commit=${PRISTINE_GIT_COMMIT_LITERAL} tag=${PRISTINE_GIT_TAG_LITERAL} \"
    \"os=${PRISTINE_BUILD_OS_LITERAL} arch=${PRISTINE_BUILD_ARCH_LITERAL} \"
    \"build=${PRISTINE_BUILD_TYPE_LITERAL}\";
const std::string_view kVersionLine = kVersionLineString;

} // namespace pristine
")

file(WRITE "${PRISTINE_HEADER_FILE}.tmp" "${header_content}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PRISTINE_HEADER_FILE}.tmp" "${PRISTINE_HEADER_FILE}")
file(REMOVE "${PRISTINE_HEADER_FILE}.tmp")

file(WRITE "${PRISTINE_SOURCE_FILE}.tmp" "${source_content}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PRISTINE_SOURCE_FILE}.tmp" "${PRISTINE_SOURCE_FILE}")
file(REMOVE "${PRISTINE_SOURCE_FILE}.tmp")
