#include "vectorize.hpp"
#include <set>
#include <string>

namespace {

// A lane context: `iv` is the loop index; `carried` are scalars assigned in
// the loop (loop-carried — unsafe to read as a per-lane value); `laneTemps`
// are `let`-bound per-lane temporaries (safe).
struct Ctx {
    std::string iv;
    std::set<std::string> carried;
    std::set<std::string> laneTemps;
};

bool isVar(const Expr& e, const std::string& name) {
    return e.kind == ExprKind::Var && e.str == name;
}
// isFloat(const Type&) comes from ast.hpp

// Does `e` mention variable `name` anywhere?
bool mentions(const Expr& e, const std::string& name) {
    if (e.kind == ExprKind::Var) return e.str == name;
    if (e.lhs && mentions(*e.lhs, name)) return true;
    if (e.rhs && mentions(*e.rhs, name)) return true;
    for (const auto& a : e.args)
        if (a && mentions(*a, name)) return true;
    return false;
}

// arr[iv]: a *contiguous*, stride-1 read/write at the loop index. For that
// the innermost index must be the loop variable AND the base must be
// loop-invariant (so `c[i][j]` in the j-loop is contiguous, but the diagonal
// `c[i][i]` in the i-loop is strided and is rejected). Arrays and lists never
// alias another value — the language guarantees it — so no runtime overlap
// check is ever needed, which is the whole architectural advantage.
bool laneIndex(const Expr& e, const std::string& iv, std::string& why) {
    if (e.kind != ExprKind::Index) return false;
    if (!isVar(*e.rhs, iv)) {
        why = "an index other than the loop variable (gather or non-unit stride)";
        return false;
    }
    if (mentions(*e.lhs, iv)) {
        why = "a diagonal/strided access (the row also varies with the loop)";
        return false;
    }
    TypeKind bk = e.lhs->type.kind;
    if (bk != TypeKind::Array && bk != TypeKind::List) {
        why = "indexing something that isn't an array or list";
        return false;
    }
    return true;
}

// A lane-local expression: at iteration i its value depends only on index-i
// array reads, loop-invariant scalars, and per-lane `let` temporaries —
// never on i as a bare value, a different index, a loop-carried scalar, or a
// call. This is exactly the shape SIMD computes bit-identically to scalar.
bool laneLocal(const Expr& e, const Ctx& c, std::string& why) {
    switch (e.kind) {
    case ExprKind::FloatLit:
    case ExprKind::IntLit:
    case ExprKind::BoolLit:
        return true;
    case ExprKind::Var:
        if (isVar(e, c.iv)) {
            why = "the loop index used as a value (needs an index vector — later)";
            return false;
        }
        if (c.carried.count(e.str) && !c.laneTemps.count(e.str)) {
            why = "a loop-carried scalar '" + e.str + "' (a sequential dependence)";
            return false;
        }
        return true; // loop-invariant scalar or a per-lane let temporary
    case ExprKind::Index:
        return laneIndex(e, c.iv, why);
    case ExprKind::Cast:
        return laneLocal(*e.lhs, c, why);
    case ExprKind::Unary:
        if (e.op == Tok::Minus)
            return laneLocal(*e.lhs, c, why);
        why = "an unsupported unary operator";
        return false;
    case ExprKind::Binary:
        switch (e.op) {
        case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash:
            return laneLocal(*e.lhs, c, why) && laneLocal(*e.rhs, c, why);
        default:
            why = "an operator that has no vector form here";
            return false;
        }
    case ExprKind::Call:
        why = "a function call in the loop body";
        return false;
    default:
        why = "an expression shape the vectorizer doesn't model yet";
        return false;
    }
}

// A reduction step: acc = acc (+|*) <lane-local>, acc a scalar accumulator.
// Reassociating these into the canonical vector tree is sound by the v0.95
// float-reduction decision. The accumulator appears exactly once in a chain
// of the same operator; the remaining leaves must all be lane-local.
bool reductionLeaves(const Expr& e, Tok op, const std::string& acc,
                     const Ctx& c, int& accCount, std::string& why) {
    if (e.kind == ExprKind::Binary && e.op == op)
        return reductionLeaves(*e.lhs, op, acc, c, accCount, why)
            && reductionLeaves(*e.rhs, op, acc, c, accCount, why);
    if (isVar(e, acc)) { accCount++; return true; }
    return laneLocal(e, c, why);
}

bool isReduction(const Stmt& st, const Ctx& c, std::string& why) {
    const Expr& lhs = *st.lhs;
    if (lhs.kind != ExprKind::Var) return false;
    if (!isFloat(lhs.type)) { why = "a non-float accumulator"; return false; }
    const Expr& rhs = *st.expr;
    if (rhs.kind != ExprKind::Binary ||
        (rhs.op != Tok::Plus && rhs.op != Tok::Star)) {
        why = "a scalar write that isn't a + or * accumulation";
        return false;
    }
    int accCount = 0;
    if (!reductionLeaves(rhs, rhs.op, lhs.str, c, accCount, why))
        return false;
    if (accCount != 1) {
        why = "an accumulation that doesn't read its own accumulator exactly once";
        return false;
    }
    return true;
}

} // namespace

VecVerdict classifyLoop(const Stmt& s) {
    VecVerdict v;
    if (s.kind != StmtKind::For || !s.expr2) {
        v.reason = "not a counted range loop";
        return v;
    }
    v.loopVar = s.name;
    v.line = s.line;
    if (s.body.empty()) { v.reason = "empty body"; return v; }

    Ctx c;
    c.iv = s.name;
    // pre-scan: every scalar assigned in the body is loop-carried until we
    // learn (below, in order) that it's a per-lane `let` temporary
    for (const auto& stp : s.body)
        if (stp->kind == StmtKind::Assign && stp->lhs->kind == ExprKind::Var)
            c.carried.insert(stp->lhs->str);

    bool hasReduction = false;
    for (const auto& stp : s.body) {
        const Stmt& st = *stp;
        std::string why;
        if (st.kind == StmtKind::Let) {
            // a per-lane temporary is fine if its value is lane-local
            if (!st.names.empty()) { v.reason = "a destructuring let in the loop"; return v; }
            if (!laneLocal(*st.expr, c, why)) {
                v.reason = "the let value involves " + why;
                return v;
            }
            c.laneTemps.insert(st.name);
            continue;
        }
        if (st.kind != StmtKind::Assign) {
            v.reason = "a non-assignment statement in the body (control flow, call, or nested loop)";
            return v;
        }
        const Expr& lhs = *st.lhs;
        if (lhs.kind == ExprKind::Index) {
            // element-wise store: arr[i] = <lane-local>
            if (!laneIndex(lhs, c.iv, why)) { v.reason = "writes through " + why; return v; }
            if (!lhs.lhs->type.elem || !isFloat(*lhs.lhs->type.elem)) {
                v.reason = "non-float arrays (integer SIMD is a later milestone)";
                return v;
            }
            if (!laneLocal(*st.expr, c, why)) { v.reason = "the value involves " + why; return v; }
        } else if (lhs.kind == ExprKind::Var) {
            if (!isReduction(st, c, why)) { v.reason = "the scalar write is " + why; return v; }
            hasReduction = true;
        } else {
            v.reason = "an assignment target that is neither arr[i] nor a scalar";
            return v;
        }
    }
    v.kind = hasReduction ? VecKind::Reduction : VecKind::ElementWise;
    return v;
}

static void walk(const std::vector<StmtPtr>& body, std::ostream& out);

static void walkStmt(const Stmt& s, std::ostream& out) {
    if (s.kind == StmtKind::For && s.expr2) {
        VecVerdict v = classifyLoop(s);
        const char* tag = v.kind == VecKind::ElementWise ? "VECTORIZE (element-wise)"
                        : v.kind == VecKind::Reduction   ? "VECTORIZE (reduction)"
                                                         : "scalar";
        out << "  line " << s.line << "  for (" << s.name << " in ..)  -> " << tag;
        if (v.kind == VecKind::No) out << "  [" << v.reason << "]";
        out << "\n";
    }
    // recurse into nested control so we report inner loops too
    walk(s.body, out);
    walk(s.elseBody, out);
}

static void walk(const std::vector<StmtPtr>& body, std::ostream& out) {
    for (const auto& s : body) walkStmt(*s, out);
}

void vectorReport(const Program& p, std::ostream& out) {
    out << "vectorization report (v0.95 milestone 1 — analysis only)\n";
    for (const auto& f : p.funcs) {
        if (f.isExtern) continue;
        out << "fn " << f.name << ":\n";
        walk(f.body, out);
    }
}
