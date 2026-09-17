// LIDL type -> JSON Schema: every mapping row in both dialects, optionality,
// records, results, tuples, and the fallbacks for spellings it does not know.

#include <logos_test.h>

#include <string>
#include <vector>

#include "doc_test_util.h"
#include "schema_ir.h"

using namespace bridge::docs;
using namespace bridge_test;

namespace {

const std::vector<Dialect> kDialects = {Dialect::Draft07, Dialect::Draft2020_12};

json j(const char* text) { return json::parse(text); }

json schemaOf(const json& type, Dialect d = Dialect::Draft07) {
    TypeScope scope;
    scope.module = "m";
    scope.records = {"Point"};
    return typeSchema(type, scope, d);
}

} // namespace

LOGOS_TEST(every_primitive_maps_the_same_in_both_dialects) {
    for (Dialect d : kDialects) {
        LOGOS_ASSERT_EQ(schemaOf(prim("tstr"), d), j(R"({"type":"string"})"));
        LOGOS_ASSERT_EQ(schemaOf(prim("bstr"), d), j(R"({"$ref":"#/components/schemas/Bytes"})"));
        LOGOS_ASSERT_EQ(schemaOf(prim("int"), d),
                        j(R"({"type":"integer","format":"int64","minimum":-9223372036854775808,"maximum":9223372036854775807})"));
        LOGOS_ASSERT_EQ(schemaOf(prim("uint"), d),
                        j(R"({"type":"integer","format":"uint64","minimum":0,"maximum":18446744073709551615})"));
        LOGOS_ASSERT_EQ(schemaOf(prim("float64"), d), j(R"({"type":"number","format":"double"})"));
        LOGOS_ASSERT_EQ(schemaOf(prim("bool"), d), j(R"({"type":"boolean"})"));
        LOGOS_ASSERT_EQ(schemaOf(prim("any"), d), json::object());
        LOGOS_ASSERT_EQ(schemaOf(prim("result"), d), j(R"({"$ref":"#/components/schemas/LogosResult"})"));
    }
}

// int64/uint64 bounds must survive as exact integers, not doubles.
LOGOS_TEST(integer_bounds_are_emitted_exactly) {
    const std::string i = schemaOf(prim("int")).dump();
    const std::string u = schemaOf(prim("uint")).dump();
    LOGOS_ASSERT_CONTAINS(i, "\"minimum\":-9223372036854775808");
    LOGOS_ASSERT_CONTAINS(i, "\"maximum\":9223372036854775807");
    LOGOS_ASSERT_CONTAINS(u, "\"minimum\":0");
    LOGOS_ASSERT_CONTAINS(u, "\"maximum\":18446744073709551615");
    LOGOS_ASSERT_TRUE(u.find("e+") == std::string::npos && u.find(".0") == std::string::npos);
}

LOGOS_TEST(containers_and_optionals_nest) {
    const json type = arrayOf(mapOf(prim("tstr"), optionalOf(arrayOf(prim("bool")))));
    for (Dialect d : kDialects) {
        LOGOS_ASSERT_EQ(schemaOf(type, d), j(R"({"type":"array","items":{"type":"object",
            "additionalProperties":{"anyOf":[{"type":"array","items":{"type":"boolean"}},{"type":"null"}]}}})"));
    }
}

LOGOS_TEST(a_map_with_a_non_text_key_says_so) {
    LOGOS_ASSERT_EQ(schemaOf(mapOf(prim("int"), prim("tstr"))),
                    j(R"({"type":"object","additionalProperties":{"type":"string"},"x-logos-map-key-type":"int"})"));
}

// ?T is two-state: ??T collapses, and ?any adds nothing to any.
LOGOS_TEST(optionals_collapse_and_optional_any_stays_any) {
    LOGOS_ASSERT_EQ(schemaOf(optionalOf(optionalOf(prim("tstr")))),
                    j(R"({"anyOf":[{"type":"string"},{"type":"null"}]})"));
    LOGOS_ASSERT_EQ(schemaOf(optionalOf(prim("any"))), json::object());
    LOGOS_ASSERT_EQ(schemaOf(optionalOf(named("Point"))),
                    j(R"({"anyOf":[{"$ref":"#/components/schemas/m.Point"},{"type":"null"}]})"));
}

LOGOS_TEST(a_record_is_a_module_qualified_component_ref) {
    for (Dialect d : kDialects)
        LOGOS_ASSERT_EQ(schemaOf(named("Point"), d), j(R"({"$ref":"#/components/schemas/m.Point"})"));
    LOGOS_ASSERT_EQ(recordComponentName("storage_module", "Manifest"), std::string("storage_module.Manifest"));
    // An undeclared name cannot become a dangling $ref.
    LOGOS_ASSERT_EQ(schemaOf(named("Nope")), j(R"({"x-logos-unknown-type":"Nope"})"));
}

LOGOS_TEST(unknown_spellings_fall_back_to_any_without_throwing) {
    LOGOS_ASSERT_EQ(schemaOf(prim("u128")), j(R"({"x-logos-unknown-type":"u128"})"));
    LOGOS_ASSERT_EQ(schemaOf(j(R"({"kind":"tuple","name":"","elements":[]})")),
                    j(R"({"x-logos-unknown-type":"tuple"})"));
    LOGOS_ASSERT_EQ(schemaOf(j(R"({"kind":"array","name":"","elements":[]})")),
                    j(R"j({"x-logos-unknown-type":"[(missing)]"})j"));
    LOGOS_ASSERT_EQ(schemaOf(j(R"({"kind":"map","elements":[{"kind":"primitive","name":"tstr"}]})")),
                    j(R"j({"x-logos-unknown-type":"{tstr: (missing)}"})j"));
    LOGOS_ASSERT_EQ(schemaOf(j(R"({"kind":"optional","elements":[]})")),
                    j(R"j({"x-logos-unknown-type":"? (missing)"})j"));
    LOGOS_ASSERT_EQ(schemaOf(json()), j(R"j({"x-logos-unknown-type":"(missing)"})j"));
    LOGOS_ASSERT_EQ(schemaOf(j("42")), j(R"j({"x-logos-unknown-type":"(missing)"})j"));
    LOGOS_ASSERT_EQ(schemaOf(j(R"({"name":"tstr"})")), j(R"j({"x-logos-unknown-type":"(missing kind)"})j"));
    // `void` is only a legacy return spelling, never a value type.
    LOGOS_ASSERT_EQ(schemaOf(prim("void")), j(R"({"x-logos-unknown-type":"void"})"));
}

// Records: required lists the non-optional fields in declaration order, and
// either optional spelling makes a field null-able and not required.
LOGOS_TEST(record_components_follow_the_derived_optionality) {
    const json opt = record("Opt", json::array({
        field("required", prim("tstr")),
        field("maybe", prim("tstr"), /*flag=*/true),
        field("count", optionalOf(prim("uint"))),
        field("blob", prim("bstr")),
    }));
    for (Dialect d : kDialects) {
        TypeScope scope = TypeScope::of("m", json{{"types", json::array({opt})}});
        const json s = recordSchema(opt, scope, d);
        LOGOS_ASSERT_EQ(s["title"], json("Opt"));
        LOGOS_ASSERT_EQ(s["type"], json("object"));
        LOGOS_ASSERT_EQ(s["required"], j(R"(["required","blob"])"));
        LOGOS_ASSERT_FALSE(s.contains("additionalProperties"));
        LOGOS_ASSERT_EQ(s["properties"]["maybe"], j(R"({"anyOf":[{"type":"string"},{"type":"null"}]})"));
        LOGOS_ASSERT_EQ(s["properties"]["count"]["anyOf"][1], j(R"({"type":"null"})"));
        LOGOS_ASSERT_EQ(s["properties"]["blob"], j(R"({"$ref":"#/components/schemas/Bytes"})"));
    }
    // A record whose fields are all optional has no required list at all.
    const json loose = record("Loose", json::array({field("a", optionalOf(prim("tstr")))}));
    LOGOS_ASSERT_FALSE(recordSchema(loose, TypeScope{}, Dialect::Draft07).contains("required"));
}

LOGOS_TEST(every_declared_record_gets_a_component) {
    json schemas = json::object();
    addRecordComponents(&schemas, "sample", sampleAst(), Dialect::Draft07);
    LOGOS_ASSERT_TRUE(schemas.contains("sample.Point"));
    LOGOS_ASSERT_TRUE(schemas.contains("sample.Unused"));
    LOGOS_ASSERT_EQ(schemas.size(), static_cast<size_t>(2));
}

LOGOS_TEST(a_no_return_method_answers_true) {
    TypeScope scope;
    const json none = method("notify", json::array());
    const json legacyPrimitive = method("old", json::array(), prim("void"));
    const json legacyNamed = method("older", json::array(), named("void"));
    const json nullReturn = json{{"name", "x"}, {"params", json::array()}, {"returnType", nullptr}};
    for (const json* m : {&none, &legacyPrimitive, &legacyNamed, &nullReturn}) {
        LOGOS_ASSERT_FALSE(methodReturns(*m));
        LOGOS_ASSERT_EQ(returnSchema(*m, scope, Dialect::Draft07), j(R"({"const":true})"));
        LOGOS_ASSERT_EQ(methodResultSchema(*m, scope, Dialect::Draft2020_12),
                        j(R"({"anyOf":[{"const":true},{"$ref":"#/components/schemas/ProviderRejection"}]})"));
    }
}

// anyOf, not oneOf: an `any` return also matches the rejection object.
LOGOS_TEST(results_are_the_value_or_a_provider_rejection) {
    TypeScope scope;
    LOGOS_ASSERT_EQ(methodResultSchema(method("a", json::array(), prim("any")), scope, Dialect::Draft07),
                    j(R"({"anyOf":[{},{"$ref":"#/components/schemas/ProviderRejection"}]})"));
    LOGOS_ASSERT_EQ(methodResultSchema(method("o", json::array(), optionalOf(prim("tstr"))), scope, Dialect::Draft07),
                    j(R"({"anyOf":[{"anyOf":[{"type":"string"},{"type":"null"}]},{"$ref":"#/components/schemas/ProviderRejection"}]})"));
    LOGOS_ASSERT_EQ(methodResultSchema(method("r", json::array(), prim("result")), scope, Dialect::Draft07)["anyOf"][0],
                    j(R"({"$ref":"#/components/schemas/LogosResult"})"));
}

LOGOS_TEST(tuples_take_each_dialects_form) {
    const json m = method("t", json::array({param("a", prim("tstr")), param("b", optionalOf(prim("int")))}));
    TypeScope scope;
    const json draft07 = paramsTuple(m, scope, Dialect::Draft07);
    LOGOS_ASSERT_EQ(draft07["items"].size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(draft07["additionalItems"], json(false));
    LOGOS_ASSERT_EQ(draft07["minItems"], json(2));
    LOGOS_ASSERT_EQ(draft07["maxItems"], json(2));
    LOGOS_ASSERT_FALSE(draft07.contains("prefixItems"));
    // An optional positional slot is still present, as null.
    LOGOS_ASSERT_EQ(draft07["items"][1]["anyOf"][1], j(R"({"type":"null"})"));

    const json d2020 = paramsTuple(m, scope, Dialect::Draft2020_12);
    LOGOS_ASSERT_EQ(d2020["prefixItems"].size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(d2020["items"], json(false));
    LOGOS_ASSERT_EQ(d2020["minItems"], json(2));
    LOGOS_ASSERT_EQ(d2020["maxItems"], json(2));
    LOGOS_ASSERT_FALSE(d2020.contains("additionalItems"));

    // A schema array may not be empty, so zero arity is just "no items".
    for (Dialect d : kDialects)
        LOGOS_ASSERT_EQ(tupleSchema({}, d), j(R"({"type":"array","maxItems":0})"));
}

LOGOS_TEST(by_name_params_require_only_the_non_optional_ones) {
    const json m = method("t", json::array({param("a", prim("tstr")), param("b", optionalOf(prim("int"))),
                                            param("c", named("Point"))}));
    TypeScope scope;
    scope.module = "m";
    scope.records = {"Point"};
    const json s = paramsObject(m, scope, Dialect::Draft2020_12);
    LOGOS_ASSERT_EQ(s["required"], j(R"(["a","c"])"));
    LOGOS_ASSERT_EQ(s["additionalProperties"], json(false));
    LOGOS_ASSERT_EQ(s["properties"]["c"], j(R"({"$ref":"#/components/schemas/m.Point"})"));
    LOGOS_ASSERT_TRUE(anyRequiredParam(m));
    LOGOS_ASSERT_FALSE(anyRequiredParam(method("u", json::array({param("x", optionalOf(prim("tstr")))}))));
}

// Nothing ever sits next to $ref: a description wraps the reference in allOf.
LOGOS_TEST(a_described_ref_is_wrapped_in_allOf) {
    LOGOS_ASSERT_EQ(describe(schemaRef("Bytes"), "the payload"),
                    j(R"({"allOf":[{"$ref":"#/components/schemas/Bytes"}],"description":"the payload"})"));
    LOGOS_ASSERT_EQ(describe(j(R"({"type":"string"})"), "a name"),
                    j(R"({"type":"string","description":"a name"})"));
    LOGOS_ASSERT_EQ(describe(schemaRef("Bytes"), ""), schemaRef("Bytes"));
    LOGOS_ASSERT_EQ(describe(json(false), "never"), j(R"({"allOf":[false],"description":"never"})"));
}

LOGOS_TEST(component_keys_stay_inside_the_spec_pattern) {
    LOGOS_ASSERT_EQ(componentKey("storage_module.Manifest-v2"), std::string("storage_module.Manifest-v2"));
    LOGOS_ASSERT_EQ(componentKey("we ird/name~"), std::string("we_ird_name_"));
    LOGOS_ASSERT_EQ(componentKey(""), std::string("_"));
}

LOGOS_TEST(shared_components_have_the_wire_shapes) {
    const json bytes = bytesSchema();
    LOGOS_ASSERT_EQ(bytes["required"], j(R"(["_bytes"])"));
    LOGOS_ASSERT_EQ(bytes["additionalProperties"], json(false));
    LOGOS_ASSERT_EQ(bytes["properties"]["_bytes"]["pattern"], json("^[A-Za-z0-9_-]*$"));
    LOGOS_ASSERT_CONTAINS(bytes["description"].get<std::string>(), "base64url");
    LOGOS_ASSERT_CONTAINS(bytes["description"].get<std::string>(), "without padding");

    // Every SDK emits all three keys; only `success` is needed to read one.
    const json result = logosResultSchema();
    LOGOS_ASSERT_EQ(result["required"], j(R"(["success"])"));
    LOGOS_ASSERT_EQ(result["properties"].size(), static_cast<size_t>(3));
    LOGOS_ASSERT_EQ(result["properties"]["success"], j(R"({"type":"boolean"})"));
    LOGOS_ASSERT_TRUE(result["properties"].contains("value"));
    LOGOS_ASSERT_TRUE(result["properties"].contains("error"));

    const json rejection = providerRejectionSchema();
    LOGOS_ASSERT_EQ(rejection["required"], j(R"(["code","message","origin"])"));
    LOGOS_ASSERT_EQ(rejection["additionalProperties"], json(false));
    LOGOS_ASSERT_EQ(rejection["properties"].size(), static_cast<size_t>(3));
    // A string, not an enum, so a future code still validates.
    LOGOS_ASSERT_FALSE(rejection["properties"]["code"].contains("enum"));
    for (const char* code : {"dispatch_failed", "invalid_args", "unknown_method"})
        LOGOS_ASSERT_CONTAINS(rejection["properties"]["code"]["description"].get<std::string>(), code);
}
