#pragma once
#include "ast.hpp"

// Type-checks the program and annotates every Expr with its type.
// Throws CompileError on the first problem found.
void analyze(Program& prog);
