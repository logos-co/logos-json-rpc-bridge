cmake_minimum_required(VERSION 3.19)
project(JsonRpcBridgeDocs LANGUAGES CXX)

# json-rpc-bridge-docs, the offline renderer: the bridge's pure headers, its one
# logos-lidl translation unit, and liblgx for --lgx. nix/docs-cli.nix builds it.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIDL_ROOT "" CACHE PATH "logos-lidl prefix: include/lidl and lib/liblogos_lidl{_c,}.a")
set(LGX_ROOT "" CACHE PATH "logos-package lib output: include/lgx.h and lib/liblgx")
set(LOGOS_LIDL_REV "" CACHE STRING "logos-lidl revision, for --version")
set(LOGOS_LIDL_VERSION "" CACHE STRING "logos-lidl version, for --version")
if(NOT LIDL_ROOT OR NOT LGX_ROOT)
    message(FATAL_ERROR "LIDL_ROOT and LGX_ROOT are required (nix/docs-cli.nix sets both)")
endif()

find_package(nlohmann_json 3.11 REQUIRED)
find_library(LGX_LIBRARY NAMES lgx PATHS "${LGX_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
include(${CMAKE_CURRENT_SOURCE_DIR}/LgxFeatures.cmake)
bridge_lgx_features("${LGX_ROOT}/include" "${LGX_LIBRARY}" BRIDGE_HAVE_LGX_EXTRACT_ASSETS)

# info.version defaults to the plugin's own: metadata.json's.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../metadata.json" BRIDGE_METADATA)
string(JSON BRIDGE_VERSION GET "${BRIDGE_METADATA}" version)

set(BRIDGE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_executable(json-rpc-bridge-docs
    json_rpc_bridge_docs.cpp
    contract_sources.cpp
    ${BRIDGE_SRC}/lidl_contract.cpp)
target_include_directories(json-rpc-bridge-docs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${BRIDGE_SRC}
    ${LIDL_ROOT}/include
    ${LGX_ROOT}/include)
# logos-lidl's C bridge archive holds toJson, and needs the core after it.
target_link_libraries(json-rpc-bridge-docs PRIVATE
    ${LIDL_ROOT}/lib/liblogos_lidl_c.a
    ${LIDL_ROOT}/lib/liblogos_lidl.a
    ${LGX_LIBRARY}
    nlohmann_json::nlohmann_json)
target_compile_definitions(json-rpc-bridge-docs PRIVATE
    JSON_RPC_BRIDGE_VERSION="${BRIDGE_VERSION}"
    LOGOS_LIDL_REV="${LOGOS_LIDL_REV}"
    LOGOS_LIDL_VERSION="${LOGOS_LIDL_VERSION}"
    BRIDGE_HAVE_LGX_EXTRACT_ASSETS=${BRIDGE_HAVE_LGX_EXTRACT_ASSETS})
set_target_properties(json-rpc-bridge-docs PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "${LGX_ROOT}/lib")

install(TARGETS json-rpc-bridge-docs RUNTIME DESTINATION bin)
