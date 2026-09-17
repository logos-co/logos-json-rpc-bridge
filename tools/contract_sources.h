#pragma once

// Where the offline renderer reads contracts from: .lidl files, and the
// assets/lidl/ an .lgx carries. Packages go through logos-package's C API only.

#include <cstddef>
#include <map>
#include <string>

namespace bridge_docs {

enum class ReadStatus { Ok, Missing, TooLarge, Failed };

// At most maxBytes; *why says what went wrong otherwise.
ReadStatus readContractFile(const std::string& path, std::size_t maxBytes, std::string* out,
                            std::string* why);

struct LgxContracts {
    bool ok = false;
    std::string error;
    std::string extractedFrom;                   // "assets", or the variant that was unpacked
    std::map<std::string, std::string> lidl;     // module name -> exact bytes
    std::map<std::string, std::string> refused;  // module name -> why its file was not read
};

// lgx_verify, lgx_load, extraction into a private directory that is removed again.
LgxContracts readLgxContracts(const std::string& path, std::size_t maxBytes);

// "lgx <version>" of the linked liblgx.
std::string lgxLibraryVersion();

} // namespace bridge_docs
