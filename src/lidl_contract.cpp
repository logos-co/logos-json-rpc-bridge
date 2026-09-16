#include "lidl_contract.h"

#include <exception>

#include <lidl/ast.hpp>
#include <lidl/identity.hpp>
#include <lidl/json.hpp>
#include <lidl/parser.hpp>
#include <lidl/serializer.hpp>
#include <lidl/validator.hpp>

// Set by the flake (preConfigure -> CMake); empty in an ad-hoc build.
#ifndef LOGOS_LIDL_REV
#define LOGOS_LIDL_REV ""
#endif
#ifndef LOGOS_LIDL_VERSION
#define LOGOS_LIDL_VERSION ""
#endif

namespace bridge {
namespace {

std::vector<ContractParam> paramsOf(const std::vector<lidl::ParamDecl>& params) {
    std::vector<ContractParam> out;
    out.reserve(params.size());
    for (const auto& p : params) out.push_back({p.name, lidl::paramIsOptional(p)});
    return out;
}

std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += "; ";
        out += p;
    }
    return out;
}

ContractResult read(const std::string& text, const std::string& expected) {
    ContractResult r;
    lidl::ParseResult parsed = lidl::parse(text);
    if (parsed.hasError()) {
        r.error = parsed.error;
        r.errorLine = parsed.errorLine;
        r.errorCol = parsed.errorColumn;
        return r;
    }
    lidl::ModuleDecl& module = parsed.module;
    r.moduleName = module.name;
    r.contractVersion = module.version;
    if (lidl::serialize(module) != text) r.warnings.push_back("non_canonical");

    lidl::ValidationResult checked = lidl::validate(module);
    r.lint = std::move(checked.warnings);
    if (checked.hasErrors()) {
        r.error = "invalid contract: " + joined(checked.errors);
        return r;
    }
    if (module.name != expected) {
        r.error = "the contract declares module '" + module.name + "', not '" + expected + "'";
        return r;
    }
    const lidl::IdentityInjection injected = lidl::injectIdentityMethods(module);
    if (injected.hasError()) {
        r.error = injected.error;
        return r;
    }

    r.astJson = lidl::toJson(module);
    for (const auto& m : module.methods)
        r.methods.push_back({m.name, m.derived, paramsOf(m.params)});
    for (const auto& e : module.events)
        r.events.push_back({e.name, paramsOf(e.params)});
    r.ok = true;
    return r;
}

} // namespace

ContractResult readContract(const std::string& text, const std::string& expectedModuleName) {
    try {
        return read(text, expectedModuleName);
    } catch (const std::exception&) {
        // toJson throws on text nlohmann cannot encode; callers check UTF-8 first.
        ContractResult r;
        r.error = "the contract could not be read";
        return r;
    }
}

std::string lidlReaderRev() {
    const std::string rev = LOGOS_LIDL_REV;
    return rev.empty() ? std::string("unknown") : rev;
}

std::string lidlReaderVersion() {
    const std::string version = LOGOS_LIDL_VERSION;
    return "lidl " + (version.empty() ? std::string("unknown") : version) + " (" +
           lidlReaderRev() + ")";
}

} // namespace bridge
