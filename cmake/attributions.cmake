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
  slang-server-v0.2.5-mit
  TITLE "slang-server v0.2.5 MIT License Text"
  SOURCE_PATH "${PRISTINE_ROOT_DIR}/licenses/texts/slang-server-v0.2.5-MIT.txt"
  NOTE "slang-server v0.2.5 is used as a local differential reference. This section preserves the upstream license file captured from the local v0.2.5 checkout."
)

pristine_register_notice_family(
  bsl-1.0
  TITLE "Boost Software License 1.0"
  SOURCE_PATH "${PRISTINE_ROOT_DIR}/licenses/texts/BSL-1.0.txt"
  NOTE "Boost-covered code is redistributed through the direct Boost 1.91.0 header dependency and slang's vendored boost_unordered header. This section preserves the Boost Software License text for that code."
)

pristine_register_notice_family(
  lz4-bsd-2-clause
  TITLE "LZ4 lib BSD 2-Clause License"
  SOURCE_PATH "${PRISTINE_ROOT_DIR}/licenses/texts/BSD-2-Clause-LZ4.txt"
  NOTE "Only LZ4 lib sources are linked for FST value-chain decompression; LZ4 program/test/example sources are not linked into pristine-engine."
)

pristine_register_notice_family(
  fastlz-mit
  TITLE "FastLZ MIT License"
  SOURCE_PATH "${PRISTINE_ROOT_DIR}/licenses/texts/FastLZ-MIT.txt"
  NOTE "FastLZ is pinned for FST FastLZ value-chain decompression."
)

pristine_register_notice_family(
  zlib
  TITLE "zlib License"
  SOURCE_PATH "${PRISTINE_ROOT_DIR}/licenses/texts/Zlib.txt"
  NOTE "zlib is pinned for FST DEFLATE value-block support. This section preserves the upstream zlib license text."
)

pristine_register_attribution(
  slang
  NAME "slang"
  VERSION "v11.0"
  OWNER "Mike Popoloski and slang contributors"
  URL "https://github.com/MikePopoloski/slang/tree/v11.0"
  LICENSE_LABEL "MIT"
  SCOPE "redistributed"
  RELATIONSHIP "direct dependency"
  FAMILY_IDS mit
  NOTES "Pinned to the MikePopoloski/slang v11.0 tag in cmake/DepsLock.cmake and linked via slang::slang. Tag v11.0 resolves to commit 7ddf4059f79eff508dd486eb42fd650cdf320d52."
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
  zlib
  NAME "zlib"
  VERSION "v1.3.1"
  OWNER "Jean-loup Gailly, Mark Adler, and zlib contributors"
  URL "https://github.com/madler/zlib/tree/v1.3.1"
  LICENSE_LABEL "zlib"
  SCOPE "redistributed"
  RELATIONSHIP "direct dependency for FST DEFLATE waveform blocks"
  FAMILY_IDS zlib
  NOTES "Pinned in cmake/DepsLock.cmake and linked via zlibstatic for FST DEFLATE geometry, time table, initial frame, and value-chain blocks."
)

pristine_register_attribution(
  lz4
  NAME "LZ4"
  VERSION "v1.10.0"
  OWNER "Yann Collet and LZ4 contributors"
  URL "https://github.com/lz4/lz4/tree/v1.10.0"
  LICENSE_LABEL "BSD 2-Clause for lib sources"
  SCOPE "redistributed"
  RELATIONSHIP "direct dependency for FST LZ4 value-chain blocks"
  FAMILY_IDS lz4-bsd-2-clause
  NOTES "Pinned in cmake/DepsLock.cmake and linked via the bundled lz4_static target; only lib sources are linked."
)

pristine_register_attribution(
  fastlz
  NAME "FastLZ"
  VERSION "commit b1342dabcf5257ab303743c9332fe75e9147a011"
  OWNER "Ariya Hidayat and FastLZ contributors"
  URL "https://github.com/ariya/FastLZ/tree/b1342dabcf5257ab303743c9332fe75e9147a011"
  LICENSE_LABEL "MIT"
  SCOPE "redistributed"
  RELATIONSHIP "direct dependency for FST FastLZ value-chain blocks"
  FAMILY_IDS fastlz-mit
  NOTES "Pinned in cmake/DepsLock.cmake and linked via the local fastlz_static target."
)

pristine_register_attribution(
  slang_server_v0_2_5
  NAME "slang-server"
  VERSION "v0.2.5"
  OWNER "Hudson River Trading LLC and slang-server contributors"
  URL "https://github.com/hudson-trading/slang-server/tree/v0.2.5"
  LICENSE_LABEL "MIT"
  SCOPE "redistributed"
  RELATIONSHIP "test/differential reference fixture source"
  FAMILY_IDS slang-server-v0.2.5-mit
  NOTES "Local reference checkout at C:/Users/maksy/Desktop/project/slang-server reports tag v0.2.5 and commit 0ec16ae4905ae8a5cf9bb33727a9eccbb82fba2b. Use as the attribution anchor if any differential fixture copies upstream MIT-covered material; rewritten fixtures remain preferred."
)

pristine_register_attribution(
  boost
  NAME "Boost"
  VERSION "1.91.0"
  OWNER "Boost contributors"
  URL "https://archives.boost.io/release/1.91.0/source/boost_1_91_0.tar.gz"
  LICENSE_LABEL "BSL-1.0"
  SCOPE "redistributed"
  RELATIONSHIP "direct header-only dependency for layout spatial indexing"
  FAMILY_IDS bsl-1.0
  NOTES "Pinned in cmake/DepsLock.cmake; pristine-engine uses Boost.Geometry R-tree headers behind LayoutSpatialIndex."
)

pristine_register_attribution(
  boost_unordered
  NAME "boost_unordered vendored header"
  VERSION "vendored in slang v11.0"
  OWNER "Boost contributors listed in slang/external/boost_unordered.hpp"
  URL "https://github.com/MikePopoloski/slang/blob/v11.0/external/boost_unordered.hpp"
  LICENSE_LABEL "BSL-1.0"
  SCOPE "redistributed"
  RELATIONSHIP "transitive vendored header via slang"
  FAMILY_IDS bsl-1.0
  NOTES "Confirmed by configure output: slang uses vendored boost_unordered when no suitable Boost package is found."
)
