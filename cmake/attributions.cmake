if(NOT DEFINED PRISTINE_ROOT_DIR)
  get_filename_component(PRISTINE_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
else()
  get_filename_component(PRISTINE_ROOT_DIR "${PRISTINE_ROOT_DIR}" ABSOLUTE)
endif()

set(PRISTINE_NOTICE_FAMILY_IDS "")
set(PRISTINE_ATTRIBUTION_IDS "")

macro(pristine_register_notice_family id)
  set(options)
  set(oneValueArgs TITLE SOURCE_PATH NOTE)
  set(multiValueArgs)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_TITLE)
    message(FATAL_ERROR "Notice family '${id}' is missing TITLE")
  endif()
  if(NOT ARG_SOURCE_PATH)
    message(FATAL_ERROR "Notice family '${id}' is missing SOURCE_PATH")
  endif()

  list(APPEND PRISTINE_NOTICE_FAMILY_IDS "${id}")
  set(PRISTINE_NOTICE_FAMILY_IDS "${PRISTINE_NOTICE_FAMILY_IDS}")
  set("PRISTINE_NOTICE_FAMILY_${id}_TITLE" "${ARG_TITLE}")
  set("PRISTINE_NOTICE_FAMILY_${id}_SOURCE_PATH" "${ARG_SOURCE_PATH}")
  set("PRISTINE_NOTICE_FAMILY_${id}_NOTE" "${ARG_NOTE}")
endmacro()

macro(pristine_register_attribution id)
  set(options)
  set(oneValueArgs NAME VERSION OWNER URL LICENSE_LABEL SCOPE RELATIONSHIP NOTES)
  set(multiValueArgs FAMILY_IDS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  foreach(required_arg NAME VERSION OWNER URL LICENSE_LABEL SCOPE RELATIONSHIP)
    if(NOT ARG_${required_arg})
      message(FATAL_ERROR "Attribution '${id}' is missing ${required_arg}")
    endif()
  endforeach()

  if(NOT ARG_FAMILY_IDS)
    message(FATAL_ERROR "Attribution '${id}' must declare at least one FAMILY_IDS entry")
  endif()

  list(APPEND PRISTINE_ATTRIBUTION_IDS "${id}")
  set(PRISTINE_ATTRIBUTION_IDS "${PRISTINE_ATTRIBUTION_IDS}")
  set("PRISTINE_ATTRIBUTION_${id}_NAME" "${ARG_NAME}")
  set("PRISTINE_ATTRIBUTION_${id}_VERSION" "${ARG_VERSION}")
  set("PRISTINE_ATTRIBUTION_${id}_OWNER" "${ARG_OWNER}")
  set("PRISTINE_ATTRIBUTION_${id}_URL" "${ARG_URL}")
  set("PRISTINE_ATTRIBUTION_${id}_LICENSE_LABEL" "${ARG_LICENSE_LABEL}")
  set("PRISTINE_ATTRIBUTION_${id}_SCOPE" "${ARG_SCOPE}")
  set("PRISTINE_ATTRIBUTION_${id}_RELATIONSHIP" "${ARG_RELATIONSHIP}")
  set("PRISTINE_ATTRIBUTION_${id}_NOTES" "${ARG_NOTES}")
  set("PRISTINE_ATTRIBUTION_${id}_FAMILY_IDS" "${ARG_FAMILY_IDS}")
endmacro()

pristine_register_notice_family(
  mit
  TITLE "MIT License"
  SOURCE_PATH "${PRISTINE_ROOT_DIR}/licenses/texts/MIT.txt"
  NOTE "The covered components in this section are distributed under the standard MIT License. The component list records the applicable upstream owners."
)

pristine_register_notice_family(
  fmt-mit-exception
  TITLE "fmt License Text"
  SOURCE_PATH "${PRISTINE_ROOT_DIR}/licenses/texts/fmt-license.txt"
  NOTE "fmt ships the MIT License text with an additional embedded-code exception. This section preserves the upstream license file verbatim."
)

pristine_register_notice_family(
  bsl-1.0
  TITLE "Boost Software License 1.0"
  SOURCE_PATH "${PRISTINE_ROOT_DIR}/licenses/texts/BSL-1.0.txt"
  NOTE "slang currently builds against a vendored boost_unordered header when a suitable Boost package is not found. This section preserves the Boost Software License text for that bundled header code."
)

pristine_register_attribution(
  slang
  NAME "slang"
  VERSION "fd508122d3de5fbe9c90845146794d059fa4eca0"
  OWNER "Michael Popoloski, packaged from the AndrewNolte/slang fork"
  URL "https://github.com/AndrewNolte/slang/tree/fd508122d3de5fbe9c90845146794d059fa4eca0"
  LICENSE_LABEL "MIT"
  SCOPE "redistributed"
  RELATIONSHIP "direct dependency"
  FAMILY_IDS mit
  NOTES "Pinned in cmake/DepsLock.cmake and linked via slang::slang."
)

pristine_register_attribution(
  fmt
  NAME "fmt"
  VERSION "12.1.0"
  OWNER "Victor Zverovich and fmt contributors"
  URL "https://github.com/fmtlib/fmt/tree/12.1.0"
  LICENSE_LABEL "MIT with fmt embedded-code exception"
  SCOPE "redistributed"
  RELATIONSHIP "transitive dependency via slang"
  FAMILY_IDS fmt-mit-exception
  NOTES "Pinned locally to satisfy slang's private fmt dependency without configure-time network access."
)

pristine_register_attribution(
  nlohmann_json
  NAME "nlohmann/json"
  VERSION "v3.11.3"
  OWNER "Niels Lohmann"
  URL "https://github.com/nlohmann/json/tree/v3.11.3"
  LICENSE_LABEL "MIT"
  SCOPE "redistributed"
  RELATIONSHIP "direct dependency"
  FAMILY_IDS mit
  NOTES "Header-only JSON library linked into pristine_core."
)

pristine_register_attribution(
  boost_unordered
  NAME "boost_unordered vendored header"
  VERSION "vendored in slang fd508122d3de5fbe9c90845146794d059fa4eca0"
  OWNER "Boost contributors listed in slang/external/boost_unordered.hpp"
  URL "https://github.com/AndrewNolte/slang/blob/fd508122d3de5fbe9c90845146794d059fa4eca0/external/boost_unordered.hpp"
  LICENSE_LABEL "BSL-1.0"
  SCOPE "redistributed"
  RELATIONSHIP "transitive vendored header via slang"
  FAMILY_IDS bsl-1.0
  NOTES "Confirmed by configure output: slang uses vendored boost_unordered when no suitable Boost package is found."
)