#pragma once
#include <string>
#include "ast.hpp"

// Generates QBE IR for a type-checked program. When optimize is true the
// MIR passes run (inlining, strength reduction, const-fold + DCE).
std::string genQBE(Program& prog, bool optimize);
