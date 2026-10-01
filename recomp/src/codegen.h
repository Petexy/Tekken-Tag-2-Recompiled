#pragma once

#include "program.h"

#include <set>
#include <string>

namespace cafe::recomp {

struct EmitOptions {
    bool comments = true; // disassembly comment above each instruction
};

// C++ definition of one entry (function start or alternate entry). Symbols it
// calls are added to `referenced` so the caller can declare them.
std::string emit_entry(const Program& program, const Entry& entry,
                       const EmitOptions& options,
                       std::set<std::string>& referenced);

} // namespace cafe::recomp
