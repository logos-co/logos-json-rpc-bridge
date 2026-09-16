#pragma once

// LIDL JSON AST types -> JSON Schema for the self-description documents. Pure; reads
// the AST's derived optionality keys, and degrades unknown spellings to {}.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace bridge {
namespace docs {

// Draft07: OpenRPC and AsyncAPI 3.0.0 payloads. Draft2020_12: OpenAPI 3.1.
enum class Dialect { Draft07, Draft2020_12 };

constexpr const char* kSchemasPrefix = "#/components/schemas/";
constexpr const char* kUnknownTypeKey = "x-logos-unknown-type";
constexpr const char* kMapKeyTypeKey = "x-logos-map-key-type";

namespace detail {

inline std::string strField(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

inline bool boolField(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return false;
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

inline const nlohmann::json* arrayField(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    return it != j.end() && it->is_array() ? &*it : nullptr;
}

inline const nlohmann::json& elementAt(const nlohmann::json& type, std::size_t i) {
    static const nlohmann::json kNull;
    const nlohmann::json* el = arrayField(type, "elements");
    return el && i < el->size() ? (*el)[i] : kNull;
}

inline std::size_t elementCount(const nlohmann::json& type) {
    const nlohmann::json* el = arrayField(type, "elements");
    return el ? el->size() : 0;
}

inline bool isPrimitive(const nlohmann::json& type, const char* name) {
    return strField(type, "kind") == "primitive" && strField(type, "name") == name;
}

} // namespace detail

// Component keys must match ^[a-zA-Z0-9._-]+$ (OpenAPI, AsyncAPI); anything else becomes '_'.
inline std::string componentKey(const std::string& raw) {
    std::string out = raw.empty() ? std::string("_") : raw;
    for (char& c : out) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) c = '_';
    }
    return out;
}

inline std::string recordComponentName(const std::string& module, const std::string& type) {
    return componentKey(module + "." + type);
}

inline nlohmann::json schemaRef(const std::string& componentName) {
    return nlohmann::json{{"$ref", std::string(kSchemasPrefix) + componentKey(componentName)}};
}

// A description never sits next to $ref: a reference is wrapped in allOf first.
inline nlohmann::json describe(nlohmann::json schema, const std::string& text) {
    if (text.empty()) return schema;
    if (!schema.is_object() || schema.contains("$ref"))
        schema = nlohmann::json{{"allOf", nlohmann::json::array({std::move(schema)})}};
    schema["description"] = text;
    return schema;
}

// ?T: the value or null. {} already admits null, so it stays {}.
inline nlohmann::json nullable(nlohmann::json schema) {
    if (schema.is_object() && schema.empty()) return schema;
    return nlohmann::json{{"anyOf", nlohmann::json::array({std::move(schema),
                                                            nlohmann::json{{"type", "null"}}})}};
}

// A positional array of exactly items.size() values, in the dialect's tuple form.
inline nlohmann::json tupleSchema(std::vector<nlohmann::json> items, Dialect dialect) {
    nlohmann::json s{{"type", "array"}};
    const std::size_t n = items.size();
    if (n == 0) {  // an empty items list is not a valid schema array
        s["maxItems"] = 0;
        return s;
    }
    if (dialect == Dialect::Draft07) {
        s["items"] = std::move(items);
        s["additionalItems"] = false;
    } else {
        s["prefixItems"] = std::move(items);
        s["items"] = false;
    }
    s["minItems"] = n;
    s["maxItems"] = n;
    return s;
}

inline nlohmann::json unknownType(const std::string& spelling) {
    return nlohmann::json{{kUnknownTypeKey, spelling}};
}

// The LIDL text of a type expression, for diagnostics.
inline std::string typeSpelling(const nlohmann::json& type) {
    if (!type.is_object()) return "(missing)";
    const std::string kind = detail::strField(type, "kind");
    const std::string name = detail::strField(type, "name");
    const auto el = [&](std::size_t i) { return typeSpelling(detail::elementAt(type, i)); };
    if (kind == "primitive" || kind == "named") return name.empty() ? "(missing)" : name;
    if (kind == "array") return "[" + el(0) + "]";
    if (kind == "map") return "{" + el(0) + ": " + el(1) + "}";
    if (kind == "optional") return "? " + el(0);
    if (kind.empty()) return "(missing kind)";
    return name.empty() ? kind : kind + ":" + name;
}

// The records a module declares, for resolving named references.
struct TypeScope {
    std::string module;
    std::set<std::string> records;

    static TypeScope of(const std::string& module, const nlohmann::json& interface) {
        TypeScope scope;
        scope.module = module;
        if (const nlohmann::json* types = detail::arrayField(interface, "types"))
            for (const auto& t : *types) {
                const std::string name = detail::strField(t, "name");
                if (!name.empty()) scope.records.insert(name);
            }
        return scope;
    }
};

inline nlohmann::json primitiveSchema(const std::string& name) {
    using nlohmann::json;
    if (name == "tstr") return json{{"type", "string"}};
    if (name == "bstr") return schemaRef("Bytes");
    if (name == "int")
        return json{{"type", "integer"}, {"format", "int64"},
                    {"minimum", std::numeric_limits<std::int64_t>::min()},
                    {"maximum", std::numeric_limits<std::int64_t>::max()}};
    if (name == "uint")
        return json{{"type", "integer"}, {"format", "uint64"},
                    {"minimum", 0},
                    {"maximum", std::numeric_limits<std::uint64_t>::max()}};
    if (name == "float64") return json{{"type", "number"}, {"format", "double"}};
    if (name == "bool") return json{{"type", "boolean"}};
    if (name == "any") return json::object();
    if (name == "result") return schemaRef("LogosResult");
    return unknownType(name.empty() ? "(missing)" : name);
}

inline nlohmann::json typeSchema(const nlohmann::json& type, const TypeScope& scope,
                                 Dialect dialect) {
    using nlohmann::json;
    if (!type.is_object()) return unknownType("(missing)");
    const std::string kind = detail::strField(type, "kind");
    const std::size_t n = detail::elementCount(type);
    if (kind == "primitive") return primitiveSchema(detail::strField(type, "name"));
    if (kind == "array") {
        if (n < 1) return unknownType(typeSpelling(type));
        return json{{"type", "array"},
                    {"items", typeSchema(detail::elementAt(type, 0), scope, dialect)}};
    }
    if (kind == "map") {
        if (n < 2) return unknownType(typeSpelling(type));
        json s{{"type", "object"},
               {"additionalProperties", typeSchema(detail::elementAt(type, 1), scope, dialect)}};
        const json& key = detail::elementAt(type, 0);
        if (!detail::isPrimitive(key, "tstr")) s[kMapKeyTypeKey] = typeSpelling(key);
        return s;
    }
    if (kind == "optional") {
        const json* inner = &type;  // ??T is ?T
        while (detail::strField(*inner, "kind") == "optional" && detail::elementCount(*inner) > 0)
            inner = &detail::elementAt(*inner, 0);
        if (detail::strField(*inner, "kind") == "optional") return unknownType(typeSpelling(type));
        return nullable(typeSchema(*inner, scope, dialect));
    }
    if (kind == "named") {
        const std::string name = detail::strField(type, "name");
        if (scope.records.count(name)) return schemaRef(recordComponentName(scope.module, name));
        return unknownType(name.empty() ? "(missing)" : name);
    }
    return unknownType(typeSpelling(type));
}

inline bool slotIsOptional(const nlohmann::json& slot) {
    return detail::boolField(slot, "isOptional");
}

// A field or parameter: the value type's schema, null-able when the slot is optional.
inline nlohmann::json slotSchema(const nlohmann::json& slot, const TypeScope& scope,
                                 Dialect dialect) {
    const bool hasValueType = slot.is_object() && slot.contains("valueType");
    const nlohmann::json& valueType = hasValueType ? slot["valueType"]
                                    : slot.is_object() && slot.contains("type") ? slot["type"]
                                    : slot;
    nlohmann::json s = typeSchema(valueType, scope, dialect);
    return slotIsOptional(slot) ? nullable(std::move(s)) : s;
}

// False for a no-return method, including the legacy `void` spelling.
inline bool methodReturns(const nlohmann::json& method) {
    if (!method.is_object()) return false;
    const auto it = method.find("returnType");
    if (it == method.end() || it->is_null()) return false;
    const std::string kind = detail::strField(*it, "kind");
    const bool isVoid = (kind == "primitive" || kind == "named") &&
                        detail::strField(*it, "name") == "void" &&
                        detail::elementCount(*it) == 0;
    return !isVoid;
}

// The value a successful call returns; a no-return method answers `true`.
inline nlohmann::json returnSchema(const nlohmann::json& method, const TypeScope& scope,
                                   Dialect dialect) {
    if (!methodReturns(method)) return nlohmann::json{{"const", true}};
    const nlohmann::json& valueType = method.contains("returnValueType")
                                          ? method["returnValueType"] : method["returnType"];
    nlohmann::json s = typeSchema(valueType, scope, dialect);
    return detail::boolField(method, "returnIsOptional") ? nullable(std::move(s)) : s;
}

// anyOf, not oneOf: an `any` return also matches the rejection shape.
inline nlohmann::json resultSchema(nlohmann::json value) {
    return nlohmann::json{{"anyOf", nlohmann::json::array({std::move(value),
                                                            schemaRef("ProviderRejection")})}};
}

inline nlohmann::json methodResultSchema(const nlohmann::json& method, const TypeScope& scope,
                                         Dialect dialect) {
    return resultSchema(returnSchema(method, scope, dialect));
}

inline const nlohmann::json& paramsOf(const nlohmann::json& decl) {
    static const nlohmann::json kEmpty = nlohmann::json::array();
    const nlohmann::json* p = detail::arrayField(decl, "params");
    return p ? *p : kEmpty;
}

inline bool anyRequiredParam(const nlohmann::json& decl) {
    for (const auto& p : paramsOf(decl))
        if (!slotIsOptional(p)) return true;
    return false;
}

inline nlohmann::json paramsTuple(const nlohmann::json& decl, const TypeScope& scope,
                                  Dialect dialect) {
    std::vector<nlohmann::json> items;
    for (const auto& p : paramsOf(decl)) items.push_back(slotSchema(p, scope, dialect));
    return tupleSchema(std::move(items), dialect);
}

// The by-name params object: declared names only, required unless optional.
inline nlohmann::json paramsObject(const nlohmann::json& decl, const TypeScope& scope,
                                   Dialect dialect) {
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    for (const auto& p : paramsOf(decl)) {
        const std::string name = detail::strField(p, "name");
        if (properties.contains(name)) continue;
        properties[name] = slotSchema(p, scope, dialect);
        if (!slotIsOptional(p)) required.push_back(name);
    }
    nlohmann::json s{{"type", "object"}, {"properties", std::move(properties)},
                     {"additionalProperties", false}};
    if (!required.empty()) s["required"] = std::move(required);
    return s;
}

inline nlohmann::json recordSchema(const nlohmann::json& typeDecl, const TypeScope& scope,
                                   Dialect dialect) {
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    if (const nlohmann::json* fields = detail::arrayField(typeDecl, "fields"))
        for (const auto& f : *fields) {
            const std::string name = detail::strField(f, "name");
            if (properties.contains(name)) continue;
            properties[name] = slotSchema(f, scope, dialect);
            if (!slotIsOptional(f)) required.push_back(name);
        }
    nlohmann::json s{{"type", "object"}, {"title", detail::strField(typeDecl, "name")},
                     {"properties", std::move(properties)}};
    if (!required.empty()) s["required"] = std::move(required);
    return s;
}

// Every record the contract declares, used by an exposed member or not.
inline void addRecordComponents(nlohmann::json* schemas, const std::string& module,
                                const nlohmann::json& interface, Dialect dialect) {
    const TypeScope scope = TypeScope::of(module, interface);
    if (const nlohmann::json* types = detail::arrayField(interface, "types"))
        for (const auto& t : *types) {
            const std::string name = detail::strField(t, "name");
            if (name.empty()) continue;
            (*schemas)[recordComponentName(module, name)] = recordSchema(t, scope, dialect);
        }
}

// ── shared LIDL components ─────────────────────────────────────────────────

inline nlohmann::json bytesSchema() {
    return nlohmann::json{
        {"title", "Bytes"},
        {"description", "A LIDL bstr: the bytes as base64url without padding (RFC 4648 "
                        "section 5), carried as the object's only key. Do not name a map key "
                        "\"_bytes\"."},
        {"type", "object"},
        {"properties", {{"_bytes", {{"type", "string"}, {"pattern", "^[A-Za-z0-9_-]*$"}}}}},
        {"required", {"_bytes"}},
        {"additionalProperties", false},
    };
}

// The {success, value, error} object every SDK emits for a LIDL `result`.
inline nlohmann::json logosResultSchema() {
    return nlohmann::json{
        {"title", "LogosResult"},
        {"description", "A LIDL result. A provider that ran and failed answers success=false "
                        "here: an application failure is a successful call, never a JSON-RPC "
                        "error."},
        {"type", "object"},
        {"properties",
         {{"success", {{"type", "boolean"}}},
          {"value", {{"description", "The payload; any JSON value, null when absent."}}},
          {"error", {{"description", "The failure message as a string; null when there is "
                                     "none."}}}}},
        {"required", {"success"}},
    };
}

inline nlohmann::json providerRejectionSchema() {
    return nlohmann::json{
        {"title", "ProviderRejection"},
        {"description", "The provider refused the call and said why. It arrives as the call's "
                        "result, not as a JSON-RPC error."},
        {"type", "object"},
        {"properties",
         {{"code", {{"type", "string"},
                    {"description", "Known codes: dispatch_failed (the argument values were "
                                    "refused), invalid_args (wrong argument count), "
                                    "unknown_method (reserved; not emitted yet). Treat any other "
                                    "code as a refusal too."}}},
          {"message", {{"type", "string"}}},
          {"origin", {{"type", "string"}, {"description", "The module that refused."}}}}},
        {"required", {"code", "message", "origin"}},
        {"additionalProperties", false},
    };
}

} // namespace docs
} // namespace bridge
