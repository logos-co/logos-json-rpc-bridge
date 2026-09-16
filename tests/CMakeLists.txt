cmake_minimum_required(VERSION 3.14)
project(JsonRpcBridgeTests LANGUAGES CXX)

include(LogosTest)

# Pure layers only: no sockets, threads or IPC. lidl_contract.cpp is the one
# source that links logos-lidl; the flake exports its roots.
set(LIDL_ROOT "$ENV{LOGOS_EXT_ROOT_LOGOS_LIDL}")
set(LIDL_C_ROOT "$ENV{LOGOS_EXT_ROOT_LOGOS_LIDL_C}")
if(NOT LIDL_ROOT OR NOT LIDL_C_ROOT)
    message(FATAL_ERROR "LOGOS_EXT_ROOT_LOGOS_LIDL{,_C} are not set; build the tests through the flake")
endif()

logos_test(
    NAME json_rpc_bridge_tests
    MODULE_SOURCES
        ../src/lidl_contract.cpp
    TEST_SOURCES
        main.cpp
        test_bridge_config.cpp
        test_digest.cpp
        test_discovery_io.cpp
        test_discovery_model.cpp
        test_docs.cpp
        test_error_map.cpp
        test_exposure.cpp
        test_lidl_contract.cpp
        test_peer_slots.cpp
        test_retry_scheduler.cpp
        test_rpc_dispatcher.cpp
        test_schema_ir.cpp
        test_subscription_table.cpp
        test_termination.cpp
    EXTRA_INCLUDES
        ../src
        ${LIDL_ROOT}/include
    EXTRA_LINK_LIBS
        ${LIDL_C_ROOT}/lib/liblogos_lidl_c.a
        ${LIDL_ROOT}/lib/liblogos_lidl.a
)
target_compile_definitions(json_rpc_bridge_tests PRIVATE
    LOGOS_LIDL_REV="$ENV{LOGOS_LIDL_REV}"
    LOGOS_LIDL_VERSION="$ENV{LOGOS_LIDL_VERSION}"
    BRIDGE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
