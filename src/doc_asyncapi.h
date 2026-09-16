#pragma once

// GET /asyncapi.json: the WebSocket endpoint as AsyncAPI 3.0.0, from the bridge's side.
// Payloads are AsyncAPI Schema Objects, a draft-07 superset, so tuples are draft-07.

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "doc_common.h"

namespace bridge {
namespace docs {

constexpr const char* kAsyncApiVersion = "3.0.0";

namespace asyncapi {

using nlohmann::json;

constexpr const char* kChannel = "ws";
constexpr const char* kChannelRef = "#/channels/ws";
constexpr const char* kErrorMessage = "rpc.error";
constexpr const char* kUnsubscribeMessage = "rpc.unsubscribe";
constexpr const char* kUnsubscribedMessage = "rpc.unsubscribed";
constexpr const char* kTerminatedMessage = "rpc.subscription_terminated";

inline json refTo(const std::string& pointer) { return json{{"$ref", pointer}}; }

inline json messageRef(const std::string& key) {
    return refTo(std::string(kChannelRef) + "/messages/" + key);
}

inline json byRequestId() { return refTo("#/components/correlationIds/requestId"); }
inline json bySubscription() { return refTo("#/components/correlationIds/subscriptionId"); }

inline json constSchema(const std::string& value) { return json{{"const", value}}; }

inline json closedObject(json properties, json required) {
    return json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

inline json requestPayload(const std::string& method, json params, bool paramsRequired) {
    json required = json::array({"jsonrpc", "id", "method"});
    if (paramsRequired) required.push_back("params");
    return closedObject(json{{"jsonrpc", constSchema("2.0")},
                             {"id", schemaRef("JsonRpcId")},
                             {"method", constSchema(method)},
                             {"params", std::move(params)}},
                        std::move(required));
}

inline json resultPayload(json result) {
    return closedObject(json{{"jsonrpc", constSchema("2.0")},
                             {"id", schemaRef("JsonRpcId")},
                             {"result", std::move(result)}},
                        json::array({"jsonrpc", "id", "result"}));
}

inline json notificationPayload(const std::string& method, json params) {
    return closedObject(json{{"jsonrpc", constSchema("2.0")},
                             {"method", constSchema(method)},
                             {"params", std::move(params)}},
                        json::array({"jsonrpc", "method", "params"}));
}

// A message carries a one-line summary; the operation using it carries the description.
inline json message(const std::string& key, const std::string& title, const std::string& text,
                    json payload, json correlationId) {
    json m{{"name", key}, {"title", title}, {"payload", std::move(payload)},
           {"correlationId", std::move(correlationId)}};
    const std::string s = summary(text);
    if (!s.empty()) m["summary"] = s;
    return m;
}

inline json tagged(json obj, const std::string& module) {
    obj["tags"] = json::array({json{{"name", module}}});
    return obj;
}

inline json receiveWithReply(const std::string& request, std::vector<std::string> replies) {
    json replyRefs = json::array();
    for (const std::string& r : replies) replyRefs.push_back(messageRef(r));
    return json{{"action", "receive"},
                {"channel", refTo(kChannelRef)},
                {"messages", json::array({messageRef(request)})},
                {"reply", {{"channel", refTo(kChannelRef)}, {"messages", std::move(replyRefs)}}}};
}

inline json sendOnly(const std::string& notification) {
    return json{{"action", "send"},
                {"channel", refTo(kChannelRef)},
                {"messages", json::array({messageRef(notification)})}};
}

struct Builder {
    json messages = json::object();
    json operations = json::object();

    void addMethod(const MethodEntry& e) {
        const std::string module = safe(e.module->name);
        const std::string name = qualified(module, e.method);
        const std::string key = componentKey(name);
        const std::string requestKey = key + ".request";
        const std::string resultKey = key + ".result";
        const std::string text = methodText(e);
        json params = json::object();
        json result = json::object();
        bool paramsRequired = false;
        if (e.typed()) {
            const json& decl = *e.decl;
            if (paramsOf(decl).empty()) {
                params = schemaRef("NoParams");
            } else {
                params = json{{"oneOf", json::array({paramsObject(decl, e.scope, Dialect::Draft07),
                                                     paramsTuple(decl, e.scope, Dialect::Draft07)})}};
                paramsRequired = true;  // absent params mean [], which only fits zero arity
            }
            result = methodResultSchema(decl, e.scope, Dialect::Draft07);
        }
        json request = message(requestKey, "Call " + name, text,
                               requestPayload(name, std::move(params), paramsRequired),
                               byRequestId());
        json reply = message(resultKey, name + " result",
                             "The value " + name + " returned, or a ProviderRejection.",
                             resultPayload(std::move(result)), byRequestId());
        json op = tagged(receiveWithReply(requestKey, {resultKey, kErrorMessage}), module);
        op["title"] = name;
        addText(&op, text);
        for (json* obj : {&request, &reply, &op}) {
            (*obj)["x-logos-module"] = module;
            (*obj)["x-logos-method"] = safe(e.method);
            if (e.typed() && detail::boolField(*e.decl, "derived")) (*obj)["x-logos-derived"] = true;
            if (!e.typed()) (*obj)["x-logos-untyped"] = true;
        }
        messages[requestKey] = std::move(request);
        messages[resultKey] = std::move(reply);
        operations[key] = std::move(op);
    }

    void addEvent(const EventEntry& e) {
        const std::string module = safe(e.module->name);
        const std::string event = safe(e.event);
        const std::string name = qualified(module, e.event);
        const std::string key = componentKey(name);
        const std::string subscribeKey = key + ".subscribe";
        const std::string subscribedKey = key + ".subscribed";
        const std::string eventKey = key + ".event";
        const std::string text = eventText(e);

        json subscribeParams = closedObject(
            json{{"subscription", schemaRef("SubscriptionId")},
                 {"module", constSchema(module)},
                 {"event", constSchema(event)}},
            json::array({"subscription", "module", "event"}));
        json ack = closedObject(
            json{{"subscription", schemaRef("SubscriptionId")},
                 {"operation", constSchema("subscribe")},
                 {"module", constSchema(module)},
                 {"event", constSchema(event)},
                 {"state", schemaRef("SubscriptionState")}},
            json::array({"subscription", "operation", "module", "event", "state"}));
        const json data = e.typed() ? paramsTuple(*e.decl, e.scope, Dialect::Draft07)
                                    : json::object();
        json eventParams = closedObject(
            json{{"subscription", schemaRef("SubscriptionId")},
                 {"module", constSchema(module)},
                 {"event", constSchema(event)},
                 {"data", describe(data, "The event's parameters, by position.")},
                 {"generation", schemaRef("EventGeneration")},
                 {"ts", schemaRef("EventTimestamp")}},
            json::array({"subscription", "module", "event", "data", "generation", "ts"}));

        json subscribe = message(subscribeKey, "Subscribe to " + name, "",
                                 requestPayload("rpc.subscribe", std::move(subscribeParams), true),
                                 byRequestId());
        json subscribed = message(subscribedKey, name + " subscribed", "",
                                  resultPayload(std::move(ack)), byRequestId());
        json notification = message(eventKey, name, text,
                                    notificationPayload("rpc.event", std::move(eventParams)),
                                    bySubscription());
        if (e.typed()) notification["x-logos-params"] = declParamNames(*e.decl);

        json subscribeOp = tagged(receiveWithReply(subscribeKey, {subscribedKey, kErrorMessage}),
                                  module);
        subscribeOp["title"] = "Subscribe to " + name;
        subscribeOp["summary"] = "Subscribe to " + name + " with a caller-assigned subscription id.";
        json eventOp = tagged(sendOnly(eventKey), module);
        eventOp["title"] = name;
        addText(&eventOp, text);
        for (json* obj : {&subscribe, &subscribed, &notification, &subscribeOp, &eventOp}) {
            (*obj)["x-logos-module"] = module;
            (*obj)["x-logos-event"] = event;
            if (!e.typed()) (*obj)["x-logos-untyped"] = true;
        }
        messages[subscribeKey] = std::move(subscribe);
        messages[subscribedKey] = std::move(subscribed);
        messages[eventKey] = std::move(notification);
        operations[subscribeKey] = std::move(subscribeOp);
        operations[eventKey] = std::move(eventOp);
    }

    void addShared() {
        // The one reply every request can get instead of its result; it has no operation of
        // its own, so it keeps its description.
        json error = message(
            kErrorMessage, "JSON-RPC error", "",
            closedObject(json{{"jsonrpc", constSchema("2.0")},
                              {"id", schemaRef("JsonRpcId")},
                              {"error", schemaRef("JsonRpcError")}},
                         json::array({"jsonrpc", "id", "error"})),
            byRequestId());
        addText(&error, "The bridge refused or could not complete a request. Denied, unexposed "
                        "and unknown targets all get the same -32601.");
        json unsubscribe = message(
            kUnsubscribeMessage, "Unsubscribe", "",
            requestPayload("rpc.unsubscribe",
                           closedObject(json{{"subscription", schemaRef("SubscriptionId")}},
                                        json::array({"subscription"})),
                           true),
            byRequestId());
        json unsubscribed = message(
            kUnsubscribedMessage, "Unsubscribed", "The subscription is gone (or never was).",
            resultPayload(closedObject(json{{"subscription", schemaRef("SubscriptionId")},
                                            {"operation", constSchema("unsubscribe")}},
                                       json::array({"subscription", "operation"}))),
            byRequestId());
        json terminated = message(
            kTerminatedMessage, "Subscription terminated", "",
            notificationPayload(
                kTerminatedMessage,
                closedObject(json{{"subscription", schemaRef("SubscriptionId")},
                                  {"module", {{"type", "string"}}},
                                  {"event", {{"type", "string"}}},
                                  {"reason", {{"type", "string"},
                                              {"description", "provider_unavailable (the provider "
                                                              "went away) or provider_changed (a "
                                                              "different build replaced it); other "
                                                              "reasons may be added."}}}},
                             json::array({"subscription", "module", "event", "reason"}))),
            bySubscription());

        const BridgeOpText* unsub = bridgeOp("rpc.unsubscribe");
        json unsubscribeOp = receiveWithReply(kUnsubscribeMessage,
                                              {kUnsubscribedMessage, kErrorMessage});
        unsubscribeOp["title"] = kUnsubscribeMessage;
        unsubscribeOp["summary"] = unsub->summary;
        unsubscribeOp["description"] = unsub->description;
        json terminatedOp = sendOnly(kTerminatedMessage);
        terminatedOp["title"] = kTerminatedMessage;
        terminatedOp["summary"] = "Tell a subscriber its subscription ended.";
        terminatedOp["description"] =
            "The provider went away or was replaced by a different build, so the subscription "
            "ended rather than silently resuming with a gap. Subscribe again (with any id) to "
            "resume, and refetch state.";
        for (json* obj : {&error, &unsubscribe, &unsubscribed, &terminated, &unsubscribeOp,
                          &terminatedOp})
            (*obj)["x-logos-bridge-op"] = true;

        messages[kErrorMessage] = std::move(error);
        messages[kUnsubscribeMessage] = std::move(unsubscribe);
        messages[kUnsubscribedMessage] = std::move(unsubscribed);
        messages[kTerminatedMessage] = std::move(terminated);
        operations[kUnsubscribeMessage] = std::move(unsubscribeOp);
        operations[kTerminatedMessage] = std::move(terminatedOp);
    }
};

inline json schemaComponents() {
    return json{{"Bytes", bytesSchema()},
                {"LogosResult", logosResultSchema()},
                {"ProviderRejection", providerRejectionSchema()},
                {"JsonRpcError", jsonRpcErrorSchema()},
                {"JsonRpcId", jsonRpcIdSchema()},
                {"SubscriptionId", subscriptionIdSchema()},
                {"SubscriptionState", subscriptionStateSchema()},
                {"NoParams", noParamsSchema()},
                {"EventGeneration", eventGenerationSchema()},
                {"EventTimestamp", eventTimestampSchema()}};
}

inline json correlationIds() {
    return json{
        {"requestId", {{"location", "$message.payload#/id"},
                       {"description", "The JSON-RPC id: a reply echoes its request's id."}}},
        {"subscriptionId", {{"location", "$message.payload#/params/subscription"},
                            {"description", "The caller-assigned subscription id from "
                                            "rpc.subscribe."}}}};
}

constexpr const char* kInfoDescription =
    "The WebSocket endpoint of the Logos JSON-RPC bridge. Operations are the bridge's: it "
    "receives requests, each answered by a reply carrying the result or an rpc.error, and it "
    "sends event notifications and subscription terminations. Replies are correlated with "
    "requests by the JSON-RPC id, events by the caller-assigned subscription id. Module methods "
    "use the `<module>.<method>` alias of rpc.call, typed from the module's canonical LIDL "
    "contract when it has one (x-logos-untyped marks names-only modules). The other bridge "
    "operations (rpc.call, rpc.schema, rpc.ping, ...) are described by rpc.discover.";

} // namespace asyncapi

inline nlohmann::json buildAsyncApi(const DocContext& ctx) {
    using nlohmann::json;
    asyncapi::Builder b;
    for (const MethodEntry& e : methodEntries(ctx)) b.addMethod(e);
    for (const EventEntry& e : eventEntries(ctx)) b.addEvent(e);
    b.addShared();

    json channelMessages = json::object();
    for (auto it = b.messages.begin(); it != b.messages.end(); ++it)
        channelMessages[it.key()] = asyncapi::refTo("#/components/messages/" + it.key());

    json headers{
        {"type", "object"},
        {"properties", {{"Sec-WebSocket-Protocol",
                         {{"type", "string"},
                          {"const", safe(ctx.subprotocol)},
                          {"description", "The subprotocol to offer."}}}}}};

    json doc = json::object();
    doc["asyncapi"] = kAsyncApiVersion;
    doc["info"] = {{"title", safe(ctx.title)},
                   {"version", safe(ctx.bridgeVersion)},
                   {"description", asyncapi::kInfoDescription}};
    doc["defaultContentType"] = "application/json";
    doc["servers"] = {{"bridge",
                       {{"host", hostPort(ctx)},
                        {"protocol", "ws"},
                        {"description", "Loopback only. Text frames only; Host and Origin are "
                                        "checked on the upgrade."}}}};
    doc["channels"] = {{asyncapi::kChannel,
                        {{"address", safe(ctx.wsPath)},
                         {"title", "JSON-RPC 2.0 over WebSocket"},
                         {"description", "Every request, reply and notification travels on "
                                         "this one connection. A slow reader is closed, not "
                                         "silently skipped; see x-logos-ws-close-codes."},
                         {"messages", std::move(channelMessages)},
                         {"bindings", {{"ws", {{"method", "GET"},
                                               {"headers", std::move(headers)},
                                               {"bindingVersion", "0.1.0"}}}}}}}};
    doc["operations"] = std::move(b.operations);

    json schemas = asyncapi::schemaComponents();
    for (const ModuleDoc& m : ctx.modules)
        if (isTyped(m)) addRecordComponents(&schemas, m.name, m.interface, Dialect::Draft07);
    doc["components"] = {{"schemas", std::move(schemas)},
                         {"messages", std::move(b.messages)},
                         {"correlationIds", asyncapi::correlationIds()}};
    doc["x-logos-ws-close-codes"] = closeCodes(ctx.limits);
    doc["x-logos-modules"] = xLogosModules(ctx);
    return doc;
}

} // namespace docs
} // namespace bridge
