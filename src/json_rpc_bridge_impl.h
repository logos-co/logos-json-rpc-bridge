#pragma once

// json_rpc_bridge — a universal (pure-C++) Logos module that exposes a
// configured set of other modules' METHOD CALLS and EVENT SUBSCRIPTIONS to
// external clients over HTTP and WebSocket, speaking JSON-RPC 2.0.
//
// It is an ordinary application module doing egress: every upstream call goes
// through the normal by-name client (logos::LpClient), and it never injects
// framed transport messages. It is NOT a transport binding and makes no
// conformance claim against LOGOS-MODULE-TRANSPORT.
//
// SCOPE is only what a normal module can already do — call methods, subscribe
// to events. Module lifecycle (loading, installing, unloading) is deliberately
// absent and will not be added: it is the one surface that would turn a
// read/write bridge into a code-execution one.
//
// AUTHORITY, stated here because it is the thing to understand before
// deploying it: every bridged call executes with json_rpc_bridge's OWN
// authority. There is no delegated identity in the model, so an external
// client inherits the bridge's aggregate reach over every exposed module.
// `expose.modules` is a containment filter, not an authorization system. The
// loopback-only bind, the optional bearer token, and the mandatory Origin/Host
// checks are the compensating controls; see README.md.
//
// This header stays free of Qt, of libwebsockets, and of the generated
// logos_sdk.h — the code generator parses it as plain C++ text and derives the
// module's contract from it, so anything it cannot parse breaks the build in a
// way that points somewhere else. All of that lives in the .cpp.

#include <memory>
#include <string>

#include <logos_module_context.h>  // LogosModuleContext base
#include <logos_result.h>          // StdLogosResult

// Everything with a socket, a thread, or an upstream client. Opaque here so the
// impl header the generator reads stays a plain declaration of the contract.
class BridgeCore;

class JsonRpcBridgeImpl : public LogosModuleContext {
public:
    JsonRpcBridgeImpl();
    ~JsonRpcBridgeImpl();

    // Parse the config JSON, resolve the exposure allowlist, and start the
    // HTTP/WebSocket server. Idempotent in the sense that a second call while
    // running fails rather than rebinding.
    //
    // On success `value` is
    //   {"http":"http://127.0.0.1:8645", "ws":"ws://127.0.0.1:8645/ws",
    //    "modules":[...], "protocol_version":"0.9.0",
    //    "subscription_continuity":true}
    // On failure `error` names the reason. Config shape and every default are
    // documented in README.md; the short version:
    //   {"http":{"host","port","allowed_origins"},
    //    "auth":{"mode":"none"|"bearer"},
    //    "expose":{"modules":[ "<name>" | {"name","methods":{"allow","deny"},
    //                                             "events":{"allow","deny"}} ]},
    //    "limits":{...}}
    //
    // Refuses outright: a non-loopback host, an empty expose list, and exposing
    // this module itself.
    StdLogosResult start(const std::string& configJson);

    // Stop the server, drop every client connection and every upstream
    // subscription. Returns failure if it was not running.
    StdLogosResult stop();

    // Current state as a JSON object: whether it is running, the bound
    // endpoints, the resolved exposure, live connection and subscription
    // counts, and the protocol version in use (which decides whether
    // subscription-loss notification is available at all).
    std::string getInfo();

protected:
    // Hard-stop everything before the host tears the module down. Returns
    // Asynchronous: connections have to be closed and threads joined, and doing
    // that inline on the dispatch thread would block the host.
    LogosShutdown aboutToUnload() override;

private:
    std::shared_ptr<BridgeCore> m_core;
};
