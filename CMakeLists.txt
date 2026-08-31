cmake_minimum_required(VERSION 3.14)
project(JsonRpcBridgeModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. LOGOS_MODULE_BUILDER_ROOT is not set.")
endif()

# Universal module: the builder runs `logos-cpp-generator --from-header` over
# src/json_rpc_bridge_impl.h for the Qt glue. Only the impl pair is listed as
# SOURCES — everything else here is header-only, so the generator never parses
# it and the impl header stays the single declaration of the module's contract.
logos_module(
    NAME json_rpc_bridge
    SOURCES
        src/json_rpc_bridge_impl.h
        src/json_rpc_bridge_impl.cpp
        src/ws_server.h
        src/ws_server.cpp
)

# Embedded HTTP + WebSocket server: libwebsockets, which serves both on ONE
# context and one port and has a documented cross-thread wake
# (lws_cancel_service) — the two properties this module is built around.
# libmicrohttpd, which openmetrics uses, has neither.
#
# openssl is explicit because libwebsockets.h includes <openssl/ssl.h> and
# nixpkgs does not propagate it (its propagatedBuildInputs is empty), so the
# transitive include would fail to resolve without naming it here and in
# metadata.json's nix.packages.runtime.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED IMPORTED_TARGET libwebsockets)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)
target_link_libraries(json_rpc_bridge_module_plugin
    PRIVATE PkgConfig::LWS PkgConfig::OPENSSL)
