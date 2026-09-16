cmake_minimum_required(VERSION 3.14)
project(JsonRpcBridgeTests LANGUAGES CXX)

include(LogosTest)

# Pure layers only: no sockets, threads or IPC. lidl_contract.cpp is the one
# source that links logos-lidl, and the docs CLI runs in-process against liblgx;
# the flake exports the roots of both.
set(LIDL_ROOT "$ENV{LOGOS_EXT_ROOT_LOGOS_LIDL}")
set(LIDL_C_ROOT "$ENV{LOGOS_EXT_ROOT_LOGOS_LIDL_C}")
set(LGX_ROOT "$ENV{LOGOS_EXT_ROOT_LGX}")
if(NOT LIDL_ROOT OR NOT LIDL_C_ROOT OR NOT LGX_ROOT)
    message(FATAL_ERROR "LOGOS_EXT_ROOT_{LOGOS_LIDL,LOGOS_LIDL_C,LGX} are not set; build the tests through the flake")
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
        ${LIDL_ROOT}/include
        ${LGX_ROOT}/include
    EXTRA_LINK_LIBS
        ${LIDL_C_ROOT}/lib/liblogos_lidl_c.a
        ${LIDL_ROOT}/lib/liblogos_lidl.a
        ${LGX_LIBRARY}
)
target_compile_definitions(json_rpc_bridge_tests PRIVATE
    LOGOS_LIDL_REV="$ENV{LOGOS_LIDL_REV}"
    LOGOS_LIDL_VERSION="$ENV{LOGOS_LIDL_VERSION}"
    JSON_RPC_BRIDGE_VERSION="${BRIDGE_VERSION}"
    BRIDGE_HAVE_LGX_EXTRACT_ASSETS=${BRIDGE_HAVE_LGX_EXTRACT_ASSETS}
    BRIDGE_DOCS_NO_MAIN
    BRIDGE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
