cmake_minimum_required(VERSION 3.14)
project(JsonRpcBridgeTests LANGUAGES CXX)

include(LogosTest)

# Pure layers only: no sockets, threads or IPC. lidl_contract.cpp is the one
# source that links logos-lidl, and the docs CLI runs in-process against liblgx,
# whose root the flake exports.
set(LGX_ROOT "$ENV{LOGOS_EXT_ROOT_LGX}")
if(NOT LGX_ROOT)
    message(FATAL_ERROR "LOGOS_EXT_ROOT_LGX is not set; build the tests through the flake")
endif()
find_library(LGX_LIBRARY NAMES lgx PATHS "${LGX_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
include(${CMAKE_CURRENT_SOURCE_DIR}/../tools/LgxFeatures.cmake)
bridge_lgx_features("${LGX_ROOT}/include" "${LGX_LIBRARY}" BRIDGE_HAVE_LGX_EXTRACT_ASSETS)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../metadata.json" BRIDGE_METADATA)
string(JSON BRIDGE_VERSION GET "${BRIDGE_METADATA}" version)

logos_test(
    NAME json_rpc_bridge_tests
    MODULE_SOURCES
        ../src/lidl_contract.cpp
        ../tools/contract_sources.cpp
        ../tools/json_rpc_bridge_docs.cpp
    TEST_SOURCES
        main.cpp
        test_bridge_config.cpp
        test_digest.cpp
        test_discovery_io.cpp
        test_discovery_model.cpp
        test_doc_goldens.cpp
        test_doc_publisher.cpp
        test_docs.cpp
        test_docs_cli.cpp
        test_error_map.cpp
        test_exposure.cpp
        test_http_body.cpp
        test_lidl_contract.cpp
        test_peer_slots.cpp
        test_retry_scheduler.cpp
        test_rpc_dispatcher.cpp
        test_schema_ir.cpp
        test_subscription_table.cpp
        test_termination.cpp
    EXTRA_INCLUDES
        ../src
        ../tools
        ${LGX_ROOT}/include
    EXTRA_LINK_LIBS
        ${LGX_LIBRARY}
    # logos-lidl's static archives; the C bridge first, it depends on the core.
    EXTERNAL_LIBS
        logos_lidl_c
        logos_lidl
)
# Fixtures and goldens are read from the source tree, which the nix check builds in.
target_compile_definitions(json_rpc_bridge_tests PRIVATE
    LOGOS_LIDL_REV="$ENV{LOGOS_LIDL_REV}"
    LOGOS_LIDL_VERSION="$ENV{LOGOS_LIDL_VERSION}"
    JSON_RPC_BRIDGE_VERSION="${BRIDGE_VERSION}"
    BRIDGE_HAVE_LGX_EXTRACT_ASSETS=${BRIDGE_HAVE_LGX_EXTRACT_ASSETS}
    BRIDGE_DOCS_NO_MAIN
    BRIDGE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    BRIDGE_TEST_GOLDENS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens")
