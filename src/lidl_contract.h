#pragma once

// The bridge's only door to logos-lidl: one lidl() answer in, plain data out.
// No lidl or nlohmann type crosses this header; lidl_contract.cpp is the one
// translation unit that includes lidl headers.

#include <string>
#include <vector>

namespace bridge {

struct ContractParam {
    std::string name;
    bool optional = false;   // `?T`
};

struct ContractMethod {
    std::string name;
    bool derived = false;    // a built-in the frontend injected (name, version, lidl)
    std::vector<ContractParam> params;
};

struct ContractEvent {
    std::string name;
    std::vector<ContractParam> params;
};

struct ContractResult {
    bool ok = false;
    std::string error;                    // why not ok; quotes contract text only
    int errorLine = 0;                    // parse errors only
    int errorCol = 0;
    std::vector<std::string> warnings;    // codes: "non_canonical"
    std::vector<std::string> lint;        // validator warnings, verbatim
    std::string moduleName;               // as declared
    std::string contractVersion;
    std::string astJson;                  // lidl::toJson after injectIdentityMethods
    std::vector<ContractMethod> methods;  // declaration order, built-ins appended
    std::vector<ContractEvent> events;
};

// parse -> validate -> name check -> injectIdentityMethods -> toJson. The text
// is non_canonical when serialize(parse(text)) != text.
ContractResult readContract(const std::string& text, const std::string& expectedModuleName);

// The logos-lidl revision this reader was built from, or "unknown".
std::string lidlReaderRev();

// "lidl <version> (<rev>)": the shape `lidl --version` prints.
std::string lidlReaderVersion();

} // namespace bridge
