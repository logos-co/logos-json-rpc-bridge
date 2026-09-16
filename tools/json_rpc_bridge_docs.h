#pragma once

// json-rpc-bridge-docs: what a running bridge serves for typed modules, rendered
// from their contracts without one.

#include <ostream>

namespace bridge_docs {

// 0 ok, 2 usage, 3 config rejected, 4 contract unreadable or invalid,
// 5 an exposed module has no contract, 6 output not written.
int runDocsCli(int argc, const char* const* argv, std::ostream& out, std::ostream& err);

} // namespace bridge_docs
