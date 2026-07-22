#pragma once
#include "ast.hpp"
#include <ostream>

// v0.95 vectorization engine — milestone 1: the legality analysis.
//
// Decides which `for (i in lo..hi)` loops are safe to turn into SIMD. This
// is the layer that exploits Simple's architecture: value semantics + no
// aliasing mean distinct arrays provably never overlap, so the dependence
// analysis that dominates a C vectorizer collapses to a local, per-lane
// check with no runtime alias guards. Pure analysis — emits nothing, so it
// cannot affect codegen correctness. Later milestones consume its result.

enum class VecKind {
    No,           // cannot vectorize (see reason)
    ElementWise,  // arr[i] = f(arr2[i], ..., invariant scalars) — bit-identical to scalar
    Reduction,    // acc = acc (+|*) f(...[i]) — needs the canonical tree order
};

struct VecVerdict {
    VecKind kind = VecKind::No;
    std::string reason;   // why not, or a note
    std::string loopVar;
    int line = 0;
};

// Classify one statement (a no-op if it isn't a range for-loop).
VecVerdict classifyLoop(const Stmt& s);

// Walk the whole program and print a per-loop report (for --vec-report).
void vectorReport(const Program& p, std::ostream& out);
