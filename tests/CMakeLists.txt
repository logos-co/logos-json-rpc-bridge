cmake_minimum_required(VERSION 3.14)
project(JsonRpcBridgeTests LANGUAGES CXX)

include(LogosTest)

# The three layers under test are header-only and pure — no sockets, no
# threads, no IPC — so there is nothing to list in MODULE_SOURCES and the whole
# policy surface is reachable without standing up a server or a module host.
logos_test(
    NAME json_rpc_bridge_tests
    TEST_SOURCES
        main.cpp
        test_bridge_config.cpp
        test_error_map.cpp
        test_rpc_dispatcher.cpp
    EXTRA_INCLUDES
        ../src
)
