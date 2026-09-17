#pragma once

// Shared by the document tests: LIDL AST snippets, typed views, and document walks.

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "doc_asyncapi.h"
#include "doc_common.h"
#include "doc_openapi.h"
#include "doc_openrpc.h"
#include "sha256.h"

namespace bridge_test {

using nlohmann::json;
using namespace bridge::docs;
using bridge::isBuiltinMethod;
using bridge::sha256Hex;

// ── AST snippets, shaped like `lidl json --identity` ───────────────────────

inline json prim(const std::string& name) {
    return json{{"kind", "primitive"}, {"name", name}, {"elements", json::array()}};
}
inline json named(const std::string& name) {
    return json{{"kind", "named"}, {"name", name}, {"elements", json::array()}};
}
inline json arrayOf(json element) {
    return json{{"kind", "array"}, {"name", ""}, {"elements", json::array({std::move(element)})}};
}
inline json mapOf(json key, json value) {
    return json{{"kind", "map"}, {"name", ""},
                {"elements", json::array({std::move(key), std::move(value)})}};
}
inline json optionalOf(json inner) {
    return json{{"kind", "optional"}, {"name", ""}, {"elements", json::array({std::move(inner)})}};
}

// The derived value type: every leading optional stripped, as logos-lidl does.
inline json valueTypeOf(const json& type) {
    const json* t = &type;
    while (t->value("kind", "") == "optional" && !(*t)["elements"].empty()) t = &(*t)["elements"][0];
    return *t;
}

inline json param(const std::string& name, json type) {
    const bool optional = type.value("kind", "") == "optional";
    json vt = valueTypeOf(type);
    return json{{"name", name}, {"type", std::move(type)}, {"isOptional", optional},
                {"valueType", std::move(vt)}};
}

inline json field(const std::string& name, json type, bool flag = false) {
    const bool optional = flag || type.value("kind", "") == "optional";
    json vt = valueTypeOf(type);
    return json{{"name", name}, {"type", std::move(type)}, {"optional", flag},
                {"isOptional", optional}, {"valueType", std::move(vt)}};
}

inline json record(const std::string& name, json fields) {
    return json{{"name", name}, {"fields", std::move(fields)}};
}

inline json method(const std::string& name, json params, const json& returnType = json(),
                   const std::string& description = "", bool derived = false) {
    json m{{"name", name}, {"params", std::move(params)}, {"description", description},
           {"jsonReturn", false}, {"resultReturn", false}, {"derived", derived}};
    if (!returnType.is_null()) {
        m["returnType"] = returnType;
        m["returnIsOptional"] = returnType.value("kind", "") == "optional";
        m["returnValueType"] = valueTypeOf(returnType);
    }
    return m;
}

inline json event(const std::string& name, json params, const std::string& description = "") {
    return json{{"name", name}, {"params", std::move(params)}, {"description", description}};
}

inline json identityMethods() {
    return json::array({
        method("name", json::array(), prim("tstr"), "The module's name, as declared in its metadata.", true),
        method("version", json::array(), prim("tstr"), "The module's version, as declared in its metadata.", true),
        method("lidl", json::array(), prim("tstr"), "The module's canonical LIDL interface document.", true),
    });
}

inline json moduleAst(const std::string& name, json types, json methods, json events,
                      const std::string& description = "") {
    for (auto& m : identityMethods()) methods.push_back(m);
    return json{{"name", name}, {"version", "1.0.0"}, {"description", description},
                {"category", "testing"}, {"depends", json::array()},
                {"optional_depends", json::array()}, {"types", std::move(types)},
                {"methods", std::move(methods)}, {"events", std::move(events)}};
}

// A typed view the way discovery builds it: exposure is the contract minus the
// denied names, with the built-ins always kept.
inline ModuleDoc typedDoc(const std::string& name, json interface,
                          const std::set<std::string>& deniedMethods = {},
                          const std::set<std::string>& deniedEvents = {}) {
    ModuleDoc m;
    m.name = name;
    m.status = kStatusOk;
    for (const auto& x : interface["methods"]) {
        const std::string n = x["name"].get<std::string>();
        if (isBuiltinMethod(n) || !deniedMethods.count(n)) m.exposedMethods.push_back(n);
    }
    for (const auto& x : interface["events"]) {
        const std::string n = x["name"].get<std::string>();
        if (!deniedEvents.count(n)) m.exposedEvents.push_back(n);
    }
    m.interfaceSha256 = sha256Hex(interface.dump());
    m.contractSha256 = sha256Hex("module " + name + " {}\n");
    m.interface = std::move(interface);
    return m;
}

// A small typed module with a record, an optional, bytes and a no-return method.
inline json sampleAst() {
    json types = json::array({
        record("Point", json::array({field("x", prim("int")), field("y", prim("int")),
                                     field("label", prim("tstr"), true)})),
        record("Unused", json::array({field("note", optionalOf(prim("tstr")))})),
    });
    json methods = json::array({
        method("move", json::array({param("to", named("Point")), param("speed", optionalOf(prim("float64")))}),
               named("Point"),
               "Move to a point.\n\nExample:\n@code{.json}\n{\"x\": 1}\n@endcode\n@param to where"),
        method("reset", json::array()),
        method("blob", json::array({param("data", prim("bstr"))}), prim("bstr")),
    });
    json events = json::array({
        event("moved", json::array({param("at", named("Point")), param("n", prim("uint"))}), "Emitted after a move."),
    });
    return moduleAst("sample", std::move(types), std::move(methods), std::move(events), "A sample module.");
}

inline DocContext sampleContext(ModuleDoc m) {
    DocContext ctx;
    ctx.bridgeVersion = "0.1.0";
    ctx.modules.push_back(std::move(m));
    return ctx;
}

const std::vector<std::string> kDocKinds = {"openrpc", "openapi", "asyncapi"};

inline json buildDoc(const std::string& kind, const DocContext& ctx) {
    if (kind == "openrpc") return buildOpenRpc(ctx);
    if (kind == "openapi") return buildOpenApi(ctx);
    return buildAsyncApi(ctx);
}

// Calls fn(node, key-it-sits-under) for every value in the document.
inline void walk(const json& node, const std::function<void(const json&, const std::string&)>& fn,
                 const std::string& key = "") {
    fn(node, key);
    if (node.is_object())
        for (auto it = node.begin(); it != node.end(); ++it) walk(it.value(), fn, it.key());
    else if (node.is_array())
        for (const auto& v : node) walk(v, fn, key);
}

// Each $ref that does not resolve inside `doc` (external ones included), as "where -> ref".
inline std::vector<std::string> unresolvedRefs(const json& doc) {
    std::vector<std::string> out;
    std::function<void(const json&, const std::string&)> visit = [&](const json& n,
                                                                      const std::string& at) {
        if (n.is_object()) {
            const auto it = n.find("$ref");
            if (it != n.end()) {
                const std::string ref = it->is_string() ? it->get<std::string>() : std::string();
                bool resolves = false;
                if (ref.rfind("#/", 0) == 0) {
                    try {
                        resolves = doc.contains(json::json_pointer(ref.substr(1)));
                    } catch (const json::exception&) {
                    }
                }
                if (!resolves) out.push_back((at.empty() ? "/" : at) + " -> " + ref);
            }
            for (auto c = n.begin(); c != n.end(); ++c) visit(c.value(), at + "/" + c.key());
        } else if (n.is_array()) {
            for (std::size_t i = 0; i < n.size(); ++i) visit(n[i], at + "/" + std::to_string(i));
        }
    };
    visit(doc, "");
    return out;
}

// Every object carrying x-logos-module, with its member name (method or event).
inline std::vector<std::pair<std::string, std::string>> targets(const json& doc) {
    std::vector<std::pair<std::string, std::string>> out;
    walk(doc, [&](const json& n, const std::string&) {
        if (!n.is_object() || !n.contains("x-logos-module")) return;
        const std::string member = n.contains("x-logos-method") ? n["x-logos-method"].get<std::string>()
                                 : n.contains("x-logos-event")  ? n["x-logos-event"].get<std::string>()
                                                                : std::string();
        out.emplace_back(n["x-logos-module"].get<std::string>(), member);
    });
    return out;
}

} // namespace bridge_test
