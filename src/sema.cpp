#include "sema.hpp"
#include "diag.hpp"
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string tn(const Type& t) { return typeName(t); }

const char* opName(Tok t) {
    switch (t) {
    case Tok::Plus: return "+";
    case Tok::Minus: return "-";
    case Tok::Star: return "*";
    case Tok::Slash: return "/";
    case Tok::Percent: return "%";
    case Tok::Lt: return "<";
    case Tok::Le: return "<=";
    case Tok::Gt: return ">";
    case Tok::Ge: return ">=";
    case Tok::EqEq: return "==";
    case Tok::NotEq: return "!=";
    case Tok::Amp: return "&";
    case Tok::Pipe: return "|";
    case Tok::Caret: return "^";
    case Tok::Shl: return "<<";
    case Tok::Shr: return ">>";
    default: return "?";
    }
}

struct VarInfo {
    Type type;
    bool isMut;
};

class Sema {
public:
    void run(Program& p) {
        try {
            runImpl(p);
        } catch (CompileError& e) {
            // v0.9: name the file the failing declaration lives in
            if (e.file.empty() && files_ && errFileId_ >= 0 &&
                errFileId_ < (int)files_->size())
                e.file = (*files_)[errFileId_].path;
            throw;
        }
    }

private:
    void runImpl(Program& p) {
        if (p.files.empty()) p.files.push_back(SourceFile{"<input>", {}});
        files_ = &p.files;
        // v0.9 modules: what each file can see
        plainImports_.assign(p.files.size(), {});
        aliasTo_.assign(p.files.size(), {});
        for (size_t i = 0; i < p.files.size(); i++) {
            for (auto& im : p.files[i].imports) {
                if (im.alias.empty()) plainImports_[i].push_back(im.fileId);
                else aliasTo_[i][im.alias] = im.fileId;
            }
        }
        // pass 1a: struct table
        for (auto& d : p.structs) {
            errFileId_ = d.fileId;
            if (d.name == "int" || d.name == "bool" || d.name == "str" || d.name == "void")
                err(d.line, "'" + d.name + "' is a built-in type name");
            if (structs_.count(d.name)) {
                StructDecl* prev = structs_[d.name];
                if (prev->fileId == d.fileId)
                    err(d.line, "struct '" + d.name + "' is defined twice");
                err(d.line, "struct '" + d.name + "' is defined in both " +
                                p.files[prev->fileId].path + " and " +
                                p.files[d.fileId].path +
                                " — struct names are program-wide, pick one");
            }
            structs_[d.name] = &d;
        }
        // pass 1b: validate fields, reject recursive layouts
        for (auto& d : p.structs) {
            errFileId_ = d.fileId;
            std::set<std::string> seen;
            for (auto& f : d.fields) {
                if (!seen.insert(f.name).second)
                    err(f.line, "duplicate field '" + f.name + "' in struct '" + d.name + "'");
                validateType(f.type, f.line);
            }
        }
        for (auto& d : p.structs) {
            std::set<std::string> path;
            checkCycle(d.name, d.line, path);
        }
        // pass 1c: function table. Names may repeat across files (that is
        // what import aliases are for); never twice in one file, and never
        // both extern and Simple.
        for (auto& f : p.funcs) {
            errFileId_ = f.fileId;
            if (f.name == "print" || f.name == "len" || f.name == "send" ||
                f.name == "recv" || f.name == "push" || f.name == "pop" ||
                f.name == "substr" || f.name == "fail" || f.name == "argc" ||
                f.name == "arg" || f.name == "input" || f.name == "read_all" ||
                f.name == "read_file" || f.name == "write_file" || f.name == "exit" ||
                f.name == "has" || f.name == "del")
                err(f.line, "'" + f.name + "' is a built-in function and cannot be redefined");
            for (Function* g : fns_[f.name]) {
                if (g->fileId == f.fileId)
                    err(f.line, "function '" + f.name + "' is defined twice");
                if (g->isExtern != f.isExtern)
                    err(f.line, "'" + f.name + "' is declared extern elsewhere — a "
                                "Simple function cannot share an extern's name");
                if (g->isExtern && f.isExtern &&
                    g->params.size() != f.params.size())
                    err(f.line, "extern '" + f.name + "' is declared elsewhere with a "
                                "different signature");
            }
            fns_[f.name].push_back(&f);
        }
        // link names: unique across the program. Externs keep their C name
        // (every declaration is the same symbol); a Simple name repeated in
        // several files gets a per-file suffix ('.' cannot appear in a
        // Simple identifier, so no collision with user names is possible).
        for (auto& [name, v] : fns_) {
            for (Function* f : v)
                f->linkName = (v.size() == 1 || f->isExtern)
                                  ? f->name
                                  : f->name + ".f" + std::to_string(f->fileId);
        }
        for (auto& f : p.funcs) byLink_[f.linkName] = &f;
        auto mit = fns_.find("main");
        if (mit == fns_.end())
            err(1, "no 'main' function found (every program needs `fn main() { ... }`)");
        if (mit->second.size() > 1)
            err(mit->second[1]->line, "'main' is defined in both " +
                    p.files[mit->second[0]->fileId].path + " and " +
                    p.files[mit->second[1]->fileId].path);
        Function* m = mit->second[0];
        if (!m->params.empty()) err(m->line, "'main' takes no parameters");
        if (m->ret.kind != TypeKind::Void && m->ret.kind != TypeKind::Int)
            err(m->line, "'main' must return nothing or int");
        // pass 2: bodies (extern declarations have none)
        for (auto& f : p.funcs) {
            errFileId_ = f.fileId;
            if (!f.isExtern) checkFunction(f);
        }
    }
    std::unordered_map<std::string, StructDecl*> structs_;
    std::unordered_map<std::string, std::vector<Function*>> fns_;
    std::unordered_map<std::string, Function*> byLink_;
    const std::vector<SourceFile>* files_ = nullptr;
    std::vector<std::vector<int>> plainImports_; // per file: plainly imported fileIds
    std::vector<std::map<std::string, int>> aliasTo_; // per file: alias -> fileId
    int errFileId_ = -1;
    std::vector<std::unordered_map<std::string, VarInfo>> scopes_;
    Function* cur_ = nullptr;
    int loopDepth_ = 0;
    int unsafeDepth_ = 0;

    void requireUnsafe(int line, const std::string& what) {
        if (unsafeDepth_ == 0)
            err(line, what + " is only allowed inside an `unsafe { }` block");
    }

    // push/pop and `l[i] = x` mutate the list, so the variable holding it
    // must be `let mut` — same rule as any other assignment.
    void mustBeMutableList(const Expr& lst) {
        const Expr* root = &lst;
        while (root->kind == ExprKind::Field || root->kind == ExprKind::Index)
            root = root->lhs.get();
        if (root->kind != ExprKind::Var) return; // a temporary list: nothing to check
        VarInfo* v = lookup(root->str);
        if (v && !v->isMut)
            err(lst.line, "cannot modify '" + root->str +
                              "' because it is immutable (declare it with `let mut`)");
    }
    [[noreturn]] void errMixed(int line, const std::string& op, const Type& a,
                               const Type& b) {
        err(line, "'" + op + "' needs both sides to be the same type, got " + tn(a) +
                      " and " + tn(b) + " (convert explicitly, e.g. " + tn(a) + "(x))");
    }

    // Integer literals adapt to the type they are used with, so `let x: u8
    // = 5;` and `flags & 1` work without a cast. Only literals adapt —
    // typed values never convert implicitly.
    // a literal, or any arithmetic built only out of literals (so `1 << 4`
    // and `0xFF & 0x0F` adapt just like `5` does)
    static bool isIntLit(const Expr& e) {
        switch (e.kind) {
        case ExprKind::IntLit:
            return true;
        case ExprKind::Unary:
            return (e.op == Tok::Minus || e.op == Tok::Tilde) && e.lhs && isIntLit(*e.lhs);
        case ExprKind::Binary:
            return isInt(e.type) && e.lhs && e.rhs && isIntLit(*e.lhs) && isIntLit(*e.rhs);
        default:
            return false;
        }
    }
    // a float literal, or arithmetic over float literals — adapts f64<->f32
    static bool isFloatLit(const Expr& e) {
        switch (e.kind) {
        case ExprKind::FloatLit:
            return true;
        case ExprKind::Unary:
            return e.op == Tok::Minus && e.lhs && isFloatLit(*e.lhs);
        case ExprKind::Binary:
            return isFloat(e.type) && e.lhs && e.rhs &&
                   isFloatLit(*e.lhs) && isFloatLit(*e.rhs);
        default:
            return false;
        }
    }
    static void retype(Expr& e, const Type& t) {
        e.type = t;
        if ((e.kind == ExprKind::Unary || e.kind == ExprKind::Binary) && e.lhs)
            retype(*e.lhs, t);
        if (e.kind == ExprKind::Binary && e.rhs) retype(*e.rhs, t);
    }
    static void coerce(Expr& e, const Type& want) {
        if (isInt(want) && isInt(e.type) && e.type != want && isIntLit(e))
            retype(e, want);
        else if (isFloat(want) && isFloat(e.type) && e.type != want && isFloatLit(e))
            retype(e, want);
    }

    // Inside `unsafe`, any pointer may stand in for any other. This is the
    // one place Simple converts without being asked — deliberately, because
    // `unsafe` already means the compiler has stopped vouching for you, and
    // C interop is unusable otherwise.
    bool assignable(const Type& to, const Type& from) const {
        if (to == from) return true;
        return unsafeDepth_ > 0 && to.kind == TypeKind::Ptr &&
               from.kind == TypeKind::Ptr;
    }

    void validateType(const Type& t, int line) {
        if (t.kind == TypeKind::Struct && !structs_.count(t.sname))
            err(line, "unknown type '" + t.sname + "'");
        if (t.kind == TypeKind::Array || t.kind == TypeKind::Chan ||
            t.kind == TypeKind::Ptr || t.kind == TypeKind::List)
            validateType(*t.elem, line);
        if (t.kind == TypeKind::Map) {
            if (t.key->kind != TypeKind::Str &&
                !(t.key->kind == TypeKind::Int && t.key->bits == 64 && !t.key->uns))
                err(line, "map keys must be str or int, got " + typeName(*t.key));
            validateType(*t.elem, line);
        }
    }

    // a struct containing itself (directly, via a nested struct, or via an
    // array) would have infinite size
    void checkCycle(const std::string& name, int line, std::set<std::string>& path) {
        if (path.count(name))
            err(line, "struct '" + name + "' contains itself (directly or indirectly)");
        path.insert(name);
        for (auto& f : structs_[name]->fields) {
            const Type* t = &f.type;
            while (t->kind == TypeKind::Array) t = t->elem.get();
            if (t->kind == TypeKind::Struct) checkCycle(t->sname, f.line, path);
        }
        path.erase(name);
    }

    void checkFunction(Function& f) {
        cur_ = &f;
        loopDepth_ = 0;
        scopes_.clear();
        scopes_.emplace_back(); // parameter scope
        for (auto& p : f.params) {
            validateType(p.type, p.line);
            if (scopes_.back().count(p.name))
                err(p.line, "duplicate parameter '" + p.name + "'");
            scopes_.back()[p.name] = {p.type, false};
        }
        if (f.ret.kind == TypeKind::Multi)
            for (auto& rt : f.rets) validateType(rt, f.line);
        else if (f.ret.kind != TypeKind::Void) validateType(f.ret, f.line);
        scopes_.emplace_back(); // body scope
        checkStmts(f.body);
        scopes_.clear();
        if (f.ret.kind != TypeKind::Void && !stmtsReturn(f.body))
            err(f.line, "function '" + f.name + "' must return " + tn(f.ret) + " on every path");
    }

    VarInfo* lookup(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    void checkStmts(std::vector<StmtPtr>& stmts) {
        for (auto& s : stmts) checkStmt(*s);
    }

    // v0.9 modules: resolve a call from the current function's file.
    // Unqualified: own file + plainly imported files; ambiguity is an error
    // pointing at `as`. Qualified: exactly that aliased file.
    Function* resolveFn(const std::string& name, const std::string& qual, int line) {
        auto it = fns_.find(name);
        int me = cur_->fileId;
        if (!qual.empty()) {
            auto& am = aliasTo_[me];
            auto ai = am.find(qual);
            if (ai == am.end())
                err(line, "unknown import alias '" + qual + "'");
            int target = ai->second;
            if (it != fns_.end())
                for (Function* f : it->second)
                    if (f->fileId == target || f->isExtern) return f;
            err(line, (*files_)[target].path + " has no function '" + name + "'");
        }
        if (it == fns_.end()) return nullptr;
        auto visible = [&](int fid) {
            if (fid == me) return true;
            for (int x : plainImports_[me])
                if (x == fid) return true;
            return false;
        };
        std::vector<Function*> cands;
        for (Function* f : it->second)
            if (visible(f->fileId)) cands.push_back(f);
        if (cands.empty()) {
            err(line, "function '" + name + "' exists in " +
                          (*files_)[it->second[0]->fileId].path +
                          " — import that file to use it");
        }
        bool allExtern = true;
        for (Function* f : cands) allExtern = allExtern && f->isExtern;
        if (cands.size() > 1 && !allExtern)
            err(line, "'" + name + "' is ambiguous: defined in " +
                          (*files_)[cands[0]->fileId].path + " and " +
                          (*files_)[cands[1]->fileId].path +
                          " — import one with `as` and qualify the call");
        return cands[0];
    }

    // Multi-return BUILTINS (v0.85 IO): their result types, or empty.
    // Mirrored in codegen — keep the two in sync.
    static std::vector<Type> builtinRets(const std::string& name) {
        if (name == "read_file")
            return {Type{TypeKind::Str}, Type{TypeKind::Error}};
        return {};
    }

    void checkCond(Stmt& s) {
        Type t = checkExpr(*s.expr);
        if (t.kind != TypeKind::Bool)
            err(s.expr->line, "condition must be bool, got " + tn(t));
    }

    void checkStmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::Let: {
            // let (a, b) = f();  — the only way to receive several values
            if (!s.names.empty()) {
                if (s.expr->kind != ExprKind::Call)
                    err(s.line, "let (...) receives a function's return values "
                                "— the right side must be a call");
                Type ct = checkExpr(*s.expr);
                if (ct.kind != TypeKind::Multi)
                    err(s.line, "'" + s.expr->str + "' returns one value "
                                "— use a plain `let x = ...`");
                std::vector<Type> rets = builtinRets(s.expr->str);
                if (rets.empty()) rets = byLink_.at(s.expr->str)->rets;
                if (s.names.size() != rets.size())
                    err(s.line, "'" + s.expr->str + "' returns " +
                                    std::to_string(rets.size()) + " values, but " +
                                    std::to_string(s.names.size()) + " are received");
                for (size_t i = 0; i < s.names.size(); i++) {
                    if (s.names[i] == "_") continue; // discard this slot
                    if (scopes_.back().count(s.names[i]))
                        err(s.line, "'" + s.names[i] + "' is already defined in this scope");
                    scopes_.back()[s.names[i]] = {rets[i], s.isMut};
                }
                break;
            }
            Type t = checkExpr(*s.expr);
            if (t.kind == TypeKind::Void)
                err(s.line, "cannot store the result of a function that returns nothing");
            if (t.kind == TypeKind::Multi)
                err(s.line, "'" + s.expr->str + "' returns several values "
                            "— receive them with `let (a, b) = " + s.expr->str + "(...);`");
            if (s.hasType) {
                validateType(s.declType, s.line);
                coerce(*s.expr, s.declType);
                t = s.expr->type;
                if (!assignable(s.declType, t))
                    err(s.line, "'" + s.name + "' is declared as " + tn(s.declType) +
                                    " but initialized with " + tn(t));
            }
            if (scopes_.back().count(s.name))
                err(s.line, "'" + s.name + "' is already defined in this scope");
            // an explicit annotation wins: `let p: *u32 = &bytes[0];` gives a
            // *u32, not the *u8 the initializer happened to have
            scopes_.back()[s.name] = {s.hasType ? s.declType : t, s.isMut};
            break;
        }
        case StmtKind::Assign: {
            Type lt = checkExpr(*s.lhs);
            // `.msg` is a read-only view of an error, not storage
            if (s.lhs->kind == ExprKind::Field &&
                s.lhs->lhs->type.kind == TypeKind::Error)
                err(s.line, "an error's message cannot be assigned "
                            "(build a new one with fail(...))");
            // find the root of the target chain
            Expr* root = s.lhs.get();
            while (root->kind == ExprKind::Field || root->kind == ExprKind::Index)
                root = root->lhs.get();
            // writing through a raw pointer doesn't mutate the pointer itself
            if (root->kind != ExprKind::Deref) {
                if (root->kind != ExprKind::Var)
                    err(s.line, "cannot assign into a temporary value");
                VarInfo* v = lookup(root->str);
                if (!v) err(s.line, "assignment to undefined variable '" + root->str + "'");
                if (!v->isMut)
                    err(s.line, "cannot assign because '" + root->str +
                                    "' is immutable (declare it with `let mut`)");
            }
            Type t = checkExpr(*s.expr);
            coerce(*s.expr, lt);
            t = s.expr->type;
            if (!assignable(lt, t))
                err(s.line, "cannot assign " + tn(t) + " to a target of type " + tn(lt));
            break;
        }
        case StmtKind::ExprStmt:
            if (s.expr->kind != ExprKind::Call)
                err(s.line, "this expression does nothing (its result is unused)");
            checkExpr(*s.expr);
            if (s.expr->type.kind == TypeKind::Multi)
                err(s.line, "'" + s.expr->str + "' returns several values — receive "
                            "them with `let (a, b) = ...;` (use _ to discard)");
            break;
        case StmtKind::Return:
            if (cur_->ret.kind == TypeKind::Multi) {
                size_t want = cur_->rets.size();
                if (s.exprs.empty())
                    err(s.line, "'" + cur_->name + "' returns " + std::to_string(want) +
                                    " values: `return a, b;`");
                if (s.exprs.size() != want)
                    err(s.line, "'" + cur_->name + "' returns " + std::to_string(want) +
                                    " values, got " + std::to_string(s.exprs.size()));
                for (size_t i = 0; i < want; i++) {
                    Type t = checkExpr(*s.exprs[i]);
                    coerce(*s.exprs[i], cur_->rets[i]);
                    t = s.exprs[i]->type;
                    if (!assignable(cur_->rets[i], t))
                        err(s.exprs[i]->line, "return value " + std::to_string(i + 1) +
                                                  " of '" + cur_->name + "' must be " +
                                                  tn(cur_->rets[i]) + ", got " + tn(t));
                }
                break;
            }
            if (!s.exprs.empty())
                err(s.line, "'" + cur_->name + "' returns one value, not " +
                                std::to_string(s.exprs.size()));
            if (cur_->ret.kind == TypeKind::Void) {
                if (s.expr)
                    err(s.line, "'" + cur_->name + "' returns nothing, but this returns a value");
            } else {
                if (!s.expr)
                    err(s.line, "'" + cur_->name + "' must return " + tn(cur_->ret));
                Type t = checkExpr(*s.expr);
                coerce(*s.expr, cur_->ret);
                t = s.expr->type;
                if (!assignable(cur_->ret, t))
                    err(s.line, "'" + cur_->name + "' returns " + tn(cur_->ret) + ", not " + tn(t));
            }
            break;
        case StmtKind::If:
            checkCond(s);
            scopes_.emplace_back();
            checkStmts(s.body);
            scopes_.pop_back();
            scopes_.emplace_back();
            checkStmts(s.elseBody);
            scopes_.pop_back();
            break;
        case StmtKind::While:
            checkCond(s);
            loopDepth_++;
            scopes_.emplace_back();
            checkStmts(s.body);
            scopes_.pop_back();
            loopDepth_--;
            break;
        case StmtKind::For: {
            Type a = checkExpr(*s.expr);
            if (!s.expr2) { // for (k in m): walk a map's keys, insertion order
                if (a.kind != TypeKind::Map)
                    err(s.line, "for (x in v) walks a map's keys — got " + tn(a) +
                                    " (ranges are `for (i in a..b)`)");
                loopDepth_++;
                scopes_.emplace_back();
                scopes_.back()[s.name] = {*a.key, false}; // key copy, immutable
                checkStmts(s.body);
                scopes_.pop_back();
                loopDepth_--;
                break;
            }
            Type b = checkExpr(*s.expr2);
            if (a.kind != TypeKind::Int || b.kind != TypeKind::Int)
                err(s.line, "for-range bounds must be ints, got " + tn(a) + " and " + tn(b));
            loopDepth_++;
            scopes_.emplace_back();
            scopes_.back()[s.name] = {intType(), false}; // loop var is immutable
            checkStmts(s.body);
            scopes_.pop_back();
            loopDepth_--;
            break;
        }
        case StmtKind::Spawn: {
            Expr& call = *s.expr;
            if (call.str == "print" || call.str == "len" || call.str == "send" ||
                call.str == "recv" || call.str == "fail" || call.str == "argc" ||
                call.str == "arg" || call.str == "input" || call.str == "read_all" ||
                call.str == "read_file" || call.str == "write_file" ||
                call.str == "exit" || call.str == "push" || call.str == "pop" ||
                call.str == "substr" || call.str == "has" || call.str == "del")
                err(s.line, "cannot spawn built-in functions");
            checkExpr(call);
            Function* f = byLink_.at(call.str);
            if (f->ret.kind != TypeKind::Void)
                err(s.line, "'" + call.str + "' returns " + tn(f->ret) +
                                " — spawned functions must return nothing; "
                                "use a channel to deliver results");
            break;
        }
        case StmtKind::Block:
            scopes_.emplace_back();
            checkStmts(s.body);
            scopes_.pop_back();
            break;
        case StmtKind::Unsafe:
            unsafeDepth_++;
            scopes_.emplace_back();
            checkStmts(s.body);
            scopes_.pop_back();
            unsafeDepth_--;
            break;
        case StmtKind::Break:
            if (loopDepth_ == 0) err(s.line, "'break' outside of a loop");
            break;
        case StmtKind::Continue:
            if (loopDepth_ == 0) err(s.line, "'continue' outside of a loop");
            break;
        }
    }

    Type checkExpr(Expr& e) {
        switch (e.kind) {
        case ExprKind::IntLit:  e.type = intType(); break;
        case ExprKind::FloatLit: e.type = floatType(); break;
        case ExprKind::BoolLit: e.type = {TypeKind::Bool}; break;
        case ExprKind::StrLit:  e.type = {TypeKind::Str}; break;
        case ExprKind::Var: {
            VarInfo* v = lookup(e.str);
            if (!v) {
                // `ok` is the built-in no-error value (a null error), usable
                // anywhere an `error` is expected — unless a local shadows it.
                if (e.qual.empty() && e.str == "ok") {
                    e.kind = ExprKind::NullLit;
                    e.type = {TypeKind::Error};
                    break;
                }
                err(e.line, "undefined variable '" + e.str + "'");
            }
            e.type = v->type;
            break;
        }
        case ExprKind::Unary: {
            Type t = checkExpr(*e.lhs);
            if (e.op == Tok::Minus) {
                if (!isNumeric(t))
                    err(e.line, "unary '-' needs a number, got " + tn(t));
                e.type = t;
            } else if (e.op == Tok::Tilde) {
                if (!isInt(t)) err(e.line, "'~' needs an int, got " + tn(t));
                e.type = t;
            } else {
                if (t.kind != TypeKind::Bool)
                    err(e.line, "'!' needs a bool, got " + tn(t));
                e.type = {TypeKind::Bool};
            }
            break;
        }
        case ExprKind::Cast: {
            Type from = checkExpr(*e.lhs);
            // str(n): int -> decimal string;  int(s): str -> parsed integer
            if (e.type.kind == TypeKind::Str) {
                if (!isInt(from))
                    err(e.line, "str(x) converts an int to text, got " + tn(from));
                break;
            }
            if (from.kind == TypeKind::Str) {
                if (!isInt(e.type))
                    err(e.line, "only int(s) parses a string, not " + tn(e.type));
                break;
            }
            // int(x)/float(x)/u8(x)... convert between numbers freely; pointer
            // conversions need unsafe
            if (!isNumeric(from) && from.kind != TypeKind::Ptr)
                err(e.line, "cannot convert " + tn(from) + " to " + tn(e.type));
            if (from.kind == TypeKind::Ptr) requireUnsafe(e.line, "converting a pointer");
            break; // e.type was set by the parser
        }
        case ExprKind::NullLit: {
            requireUnsafe(e.line, "the null pointer");
            e.type.kind = TypeKind::Ptr;
            e.type.elem = std::make_shared<Type>(intType(8, true)); // *u8
            break;
        }
        case ExprKind::AddrOf: {
            requireUnsafe(e.line, "taking an address with '&'");
            Type t = checkExpr(*e.lhs);
            const Expr* root = e.lhs.get();
            while (root->kind == ExprKind::Field || root->kind == ExprKind::Index)
                root = root->lhs.get();
            if (root->kind != ExprKind::Var)
                err(e.line, "'&' needs a variable, field, or element");
            e.type.kind = TypeKind::Ptr;
            e.type.elem = std::make_shared<Type>(t);
            break;
        }
        case ExprKind::Deref: {
            requireUnsafe(e.line, "dereferencing a pointer with '*'");
            Type t = checkExpr(*e.lhs);
            if (t.kind != TypeKind::Ptr)
                err(e.line, "'*' needs a pointer, got " + tn(t));
            if (t.elem->kind == TypeKind::Void)
                err(e.line, "cannot dereference a pointer to nothing");
            e.type = *t.elem;
            break;
        }
        case ExprKind::Binary: {
            Type a = checkExpr(*e.lhs);
            Type b = checkExpr(*e.rhs);
            // let a numeric literal take the other side's type. Shifts are
            // excluded (the count may be any int type); for the value-style
            // bitwise ops (& | ^) both sides must still match, so coercing a
            // literal to the other's type is what makes `reg | (1 << 4)` work.
            if (isNumeric(a) && isNumeric(b) && a != b &&
                e.op != Tok::Shl && e.op != Tok::Shr) {
                coerce(*e.lhs, b);
                coerce(*e.rhs, a);
                a = e.lhs->type;
                b = e.rhs->type;
            }
            switch (e.op) {
            case Tok::Plus:
                if (a.kind == TypeKind::Str && b.kind == TypeKind::Str) {
                    e.type = {TypeKind::Str};
                    break;
                }
                if (a.kind == TypeKind::Ptr && isInt(b)) { // pointer arithmetic
                    requireUnsafe(e.line, "pointer arithmetic");
                    e.type = a;
                    break;
                }
                if (!isNumeric(a) || !isNumeric(b))
                    err(e.line, "'+' needs two numbers or two strs, got " + tn(a) + " and " + tn(b));
                if (a != b) errMixed(e.line, "+", a, b);
                e.type = a;
                break;
            case Tok::Minus:
                if (a.kind == TypeKind::Ptr && isInt(b)) {
                    requireUnsafe(e.line, "pointer arithmetic");
                    e.type = a;
                    break;
                }
                if (!isNumeric(a) || !isNumeric(b))
                    err(e.line, "'-' needs numeric operands, got " + tn(a) + " and " + tn(b));
                if (a != b) errMixed(e.line, "-", a, b);
                e.type = a;
                break;
            case Tok::Star: case Tok::Slash:
                if (!isNumeric(a) || !isNumeric(b))
                    err(e.line, std::string("'") + opName(e.op) + "' needs numeric operands, got " +
                                    tn(a) + " and " + tn(b));
                if (a != b) errMixed(e.line, opName(e.op), a, b);
                e.type = a;
                break;
            case Tok::Percent:
            case Tok::Amp: case Tok::Pipe: case Tok::Caret:
                // '%' and bitwise are integer-only (floats use no remainder)
                if (!isInt(a) || !isInt(b))
                    err(e.line, std::string("'") + opName(e.op) + "' needs int operands, got " +
                                    tn(a) + " and " + tn(b));
                if (a != b) errMixed(e.line, opName(e.op), a, b);
                e.type = a;
                break;
            case Tok::Shl: case Tok::Shr:
                // the shift amount is a count, not a value: any int type
                if (!isInt(a) || !isInt(b))
                    err(e.line, std::string("'") + opName(e.op) + "' needs int operands, got " +
                                    tn(a) + " and " + tn(b));
                e.type = a;
                break;
            case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge:
                if (!isNumeric(a) || !isNumeric(b))
                    err(e.line, std::string("'") + opName(e.op) + "' needs numeric operands, got " +
                                    tn(a) + " and " + tn(b));
                if (a != b) errMixed(e.line, opName(e.op), a, b);
                e.type = {TypeKind::Bool};
                break;
            case Tok::EqEq: case Tok::NotEq:
                if (!assignable(a, b) && !assignable(b, a))
                    err(e.line, "cannot compare " + tn(a) + " with " + tn(b));
                if (isAggregate(a))
                    err(e.line, "structs and arrays cannot be compared with '==' (compare fields)");
                if (a.kind == TypeKind::Chan)
                    err(e.line, "channels cannot be compared");
                if (a.kind == TypeKind::Map || a.kind == TypeKind::List)
                    err(e.line, tn(a) + " values cannot be compared with '==' "
                                "(compare contents yourself)");
                if (a.kind == TypeKind::Void)
                    err(e.line, "cannot compare values that don't exist");
                e.type = {TypeKind::Bool};
                break;
            case Tok::AndAnd: case Tok::OrOr:
                if (a.kind != TypeKind::Bool || b.kind != TypeKind::Bool)
                    err(e.line, "'&&' and '||' need bool operands, got " + tn(a) + " and " + tn(b));
                e.type = {TypeKind::Bool};
                break;
            default:
                err(e.line, "internal error: unknown binary operator");
            }
            break;
        }
        case ExprKind::ArrayLit: {
            Type t0 = checkExpr(*e.args[0]);
            if (t0.kind == TypeKind::Void)
                err(e.line, "array elements must be values");
            if (e.ival > 0) { // [value; count]
                e.type.kind = TypeKind::Array;
                e.type.alen = (int)e.ival;
                e.type.elem = std::make_shared<Type>(t0);
                break;
            }
            for (size_t i = 1; i < e.args.size(); i++) {
                Type ti = checkExpr(*e.args[i]);
                coerce(*e.args[i], t0);
                ti = e.args[i]->type;
                if (ti != t0)
                    err(e.args[i]->line, "array elements must all be the same type: got " +
                                             tn(t0) + " then " + tn(ti));
            }
            e.type.kind = TypeKind::Array;
            e.type.alen = (int)e.args.size();
            e.type.elem = std::make_shared<Type>(t0);
            break;
        }
        case ExprKind::Index: {
            Type base = checkExpr(*e.lhs);
            Type idx = checkExpr(*e.rhs);
            if (base.kind == TypeKind::Map) { // m[k]: key must match, traps if absent
                coerce(*e.rhs, *base.key);
                idx = e.rhs->type;
                if (idx != *base.key)
                    err(e.rhs->line, "this map's keys are " + tn(*base.key) +
                                         ", got " + tn(idx));
                e.type = *base.elem;
                break;
            }
            if (!isInt(idx))
                err(e.rhs->line, "index must be an int, got " + tn(idx));
            if (base.kind == TypeKind::Str) {
                e.type = intType();   // s[i] is the byte value 0..255
                break;
            }
            if (base.kind != TypeKind::Array && base.kind != TypeKind::List)
                err(e.line, "cannot index into " + tn(base) +
                                " (only strings, arrays, lists, and maps can be indexed)");
            e.type = *base.elem;
            break;
        }
        case ExprKind::ListNew: {
            validateType(e.type, e.line); // parser filled in the list type
            break;
        }
        case ExprKind::MapNew: {
            validateType(e.type, e.line); // parser filled in the map type
            break;
        }
        case ExprKind::Field: {
            Type base = checkExpr(*e.lhs);
            // the built-in `error` type exposes one field: `.msg` (its text)
            if (base.kind == TypeKind::Error) {
                if (e.str != "msg")
                    err(e.line, "error has only the field 'msg', not '" + e.str + "'");
                e.type = {TypeKind::Str};
                break;
            }
            if (base.kind != TypeKind::Struct)
                err(e.line, tn(base) + " has no fields");
            StructDecl* d = structs_[base.sname];
            bool found = false;
            for (auto& f : d->fields) {
                if (f.name == e.str) {
                    e.type = f.type;
                    found = true;
                    break;
                }
            }
            if (!found)
                err(e.line, "struct '" + base.sname + "' has no field '" + e.str + "'");
            break;
        }
        case ExprKind::ChanNew: {
            validateType(e.type, e.line); // parser filled in the chan type
            Type cap = checkExpr(*e.args[0]);
            if (cap.kind != TypeKind::Int)
                err(e.args[0]->line, "channel capacity must be an int, got " + tn(cap));
            break;
        }
        case ExprKind::StructLit: {
            auto it = structs_.find(e.str);
            if (it == structs_.end())
                err(e.line, "unknown struct '" + e.str + "'");
            StructDecl* d = it->second;
            std::set<std::string> given;
            for (size_t i = 0; i < e.fieldNames.size(); i++) {
                const std::string& fname = e.fieldNames[i];
                if (!given.insert(fname).second)
                    err(e.args[i]->line, "field '" + fname + "' given twice");
                const Param* decl = nullptr;
                for (auto& f : d->fields)
                    if (f.name == fname) decl = &f;
                if (!decl)
                    err(e.args[i]->line, "struct '" + e.str + "' has no field '" + fname + "'");
                Type t = checkExpr(*e.args[i]);
                coerce(*e.args[i], decl->type);
                t = e.args[i]->type;
                if (!assignable(decl->type, t))
                    err(e.args[i]->line, "field '" + fname + "' is " + tn(decl->type) +
                                             ", got " + tn(t));
            }
            for (auto& f : d->fields)
                if (!given.count(f.name))
                    err(e.line, "missing field '" + f.name + "' in " + e.str + " literal");
            e.type.kind = TypeKind::Struct;
            e.type.sname = e.str;
            break;
        }
        case ExprKind::Call: {
            if (e.qual.empty() && e.str == "print") {
                if (e.args.size() != 1) err(e.line, "print takes exactly one argument");
                Type t = checkExpr(*e.args[0]);
                if (isAggregate(t))
                    err(e.line, "cannot print a " + tn(t) + " directly (print its fields/elements)");
                if (t.kind != TypeKind::Int && t.kind != TypeKind::Float &&
                    t.kind != TypeKind::Bool && t.kind != TypeKind::Str)
                    err(e.line, "print takes a number, bool, or str, got " + tn(t));
                e.type = {TypeKind::Void};
                break;
            }
            if (e.qual.empty() && e.str == "send") {
                if (e.args.size() != 2)
                    err(e.line, "send takes a channel and a value: send(ch, v)");
                Type ct = checkExpr(*e.args[0]);
                if (ct.kind != TypeKind::Chan)
                    err(e.args[0]->line, "send's first argument must be a channel, got " + tn(ct));
                Type vt = checkExpr(*e.args[1]);
                if (vt != *ct.elem)
                    err(e.args[1]->line, "this channel carries " + tn(*ct.elem) +
                                             ", cannot send " + tn(vt));
                e.type = {TypeKind::Void};
                break;
            }
            if (e.qual.empty() && e.str == "recv") {
                if (e.args.size() != 1) err(e.line, "recv takes one channel: recv(ch)");
                Type ct = checkExpr(*e.args[0]);
                if (ct.kind != TypeKind::Chan)
                    err(e.args[0]->line, "recv needs a channel, got " + tn(ct));
                e.type = *ct.elem;
                break;
            }
            if (e.qual.empty() && e.str == "len") {
                if (e.args.size() != 1) err(e.line, "len takes exactly one argument");
                Type t = checkExpr(*e.args[0]);
                if (t.kind != TypeKind::Str && t.kind != TypeKind::Array &&
                    t.kind != TypeKind::List && t.kind != TypeKind::Map)
                    err(e.line, "len needs a str, array, list, or map, got " + tn(t));
                e.type = intType();
                break;
            }
            if (e.qual.empty() && e.str == "has") {
                if (e.args.size() != 2)
                    err(e.line, "has takes a map and a key: has(m, k)");
                Type mt = checkExpr(*e.args[0]);
                if (mt.kind != TypeKind::Map)
                    err(e.args[0]->line, "has's first argument must be a map, got " + tn(mt));
                Type kt = checkExpr(*e.args[1]);
                coerce(*e.args[1], *mt.key);
                kt = e.args[1]->type;
                if (kt != *mt.key)
                    err(e.args[1]->line, "this map's keys are " + tn(*mt.key) +
                                             ", got " + tn(kt));
                e.type = {TypeKind::Bool};
                break;
            }
            if (e.qual.empty() && e.str == "del") {
                if (e.args.size() != 2)
                    err(e.line, "del takes a map and a key: del(m, k)");
                Type mt = checkExpr(*e.args[0]);
                if (mt.kind != TypeKind::Map)
                    err(e.args[0]->line, "del's first argument must be a map, got " + tn(mt));
                mustBeMutableList(*e.args[0]); // same rule: mutating needs `let mut`
                Type kt = checkExpr(*e.args[1]);
                coerce(*e.args[1], *mt.key);
                kt = e.args[1]->type;
                if (kt != *mt.key)
                    err(e.args[1]->line, "this map's keys are " + tn(*mt.key) +
                                             ", got " + tn(kt));
                e.type = {TypeKind::Void};
                break;
            }
            if (e.qual.empty() && e.str == "push") {
                if (e.args.size() != 2)
                    err(e.line, "push takes a list and a value: push(l, v)");
                Type lt = checkExpr(*e.args[0]);
                if (lt.kind != TypeKind::List)
                    err(e.args[0]->line, "push's first argument must be a list, got " + tn(lt));
                mustBeMutableList(*e.args[0]);
                Type vt = checkExpr(*e.args[1]);
                coerce(*e.args[1], *lt.elem);
                vt = e.args[1]->type;
                if (vt != *lt.elem)
                    err(e.args[1]->line, "this list holds " + tn(*lt.elem) +
                                             ", cannot push " + tn(vt));
                e.type = {TypeKind::Void};
                break;
            }
            if (e.qual.empty() && e.str == "substr") {
                if (e.args.size() != 3)
                    err(e.line, "substr takes a string and two ints: substr(s, start, end)");
                Type st = checkExpr(*e.args[0]);
                if (st.kind != TypeKind::Str)
                    err(e.args[0]->line, "substr's first argument must be a str, got " + tn(st));
                for (int k = 1; k < 3; k++) {
                    Type it = checkExpr(*e.args[k]);
                    if (!isInt(it))
                        err(e.args[k]->line, "substr bounds must be ints, got " + tn(it));
                }
                e.type = {TypeKind::Str};
                break;
            }
            if (e.qual.empty() && e.str == "pop") {
                if (e.args.size() != 1) err(e.line, "pop takes one list: pop(l)");
                Type lt = checkExpr(*e.args[0]);
                if (lt.kind != TypeKind::List)
                    err(e.args[0]->line, "pop needs a list, got " + tn(lt));
                mustBeMutableList(*e.args[0]);
                e.type = *lt.elem;   // returns the removed element
                break;
            }
            if (e.qual.empty() && e.str == "fail") {
                if (e.args.size() != 1)
                    err(e.line, "fail takes one message: fail(\"why it failed\")");
                Type mt = checkExpr(*e.args[0]);
                if (mt.kind != TypeKind::Str)
                    err(e.args[0]->line, "fail's message must be a str, got " + tn(mt));
                e.type = {TypeKind::Error};
                break;
            }
            // ---- IO builtins (v0.85) ----
            if (e.qual.empty() && e.str == "argc") {
                if (!e.args.empty()) err(e.line, "argc takes no arguments");
                e.type = intType();
                break;
            }
            if (e.qual.empty() && e.str == "arg") {
                if (e.args.size() != 1) err(e.line, "arg takes one index: arg(i)");
                Type it2 = checkExpr(*e.args[0]);
                if (!isInt(it2))
                    err(e.args[0]->line, "arg's index must be an int, got " + tn(it2));
                e.type = {TypeKind::Str};
                break;
            }
            if (e.qual.empty() && e.str == "input" || e.str == "read_all") {
                if (!e.args.empty()) err(e.line, e.str + " takes no arguments");
                e.type = {TypeKind::Str};
                break;
            }
            if (e.qual.empty() && e.str == "read_file") {
                if (e.args.size() != 1)
                    err(e.line, "read_file takes one path: read_file(\"a.txt\")");
                Type pt = checkExpr(*e.args[0]);
                if (pt.kind != TypeKind::Str)
                    err(e.args[0]->line, "read_file's path must be a str, got " + tn(pt));
                e.type = {TypeKind::Multi}; // (str, error) — see builtinRets
                break;
            }
            if (e.qual.empty() && e.str == "write_file") {
                if (e.args.size() != 2)
                    err(e.line, "write_file takes a path and contents: "
                                "write_file(\"a.txt\", data)");
                for (int k = 0; k < 2; k++) {
                    Type at = checkExpr(*e.args[k]);
                    if (at.kind != TypeKind::Str)
                        err(e.args[k]->line, std::string(k == 0 ? "write_file's path"
                                                                : "write_file's contents") +
                                                 " must be a str, got " + tn(at));
                }
                e.type = {TypeKind::Error};
                break;
            }
            if (e.qual.empty() && e.str == "exit") {
                if (e.args.size() != 1) err(e.line, "exit takes one status code: exit(1)");
                Type et = checkExpr(*e.args[0]);
                if (!isInt(et))
                    err(e.args[0]->line, "exit's status must be an int, got " + tn(et));
                e.type = {TypeKind::Void};
                break;
            }
            Function* f = resolveFn(e.str, e.qual, e.line);
            if (!f)
                err(e.line, "call to undefined function '" + e.str + "'");
            if (f->variadic ? e.args.size() < f->params.size()
                            : e.args.size() != f->params.size())
                err(e.line, "'" + e.str + "' takes " +
                                (f->variadic ? "at least " : "") +
                                std::to_string(f->params.size()) + " argument(s), got " +
                                std::to_string(e.args.size()));
            for (size_t i = 0; i < e.args.size(); i++) {
                Type t = checkExpr(*e.args[i]);
                if (i >= f->params.size()) continue; // extern variadic tail
                coerce(*e.args[i], f->params[i].type);
                t = e.args[i]->type;
                if (!assignable(f->params[i].type, t))
                    err(e.args[i]->line, "argument " + std::to_string(i + 1) + " of '" + e.str +
                                             "' must be " + tn(f->params[i].type) + ", got " + tn(t));
            }
            e.type = f->ret;
            e.str = f->linkName; // codegen resolves by unique link name
            e.qual.clear();
            break;
        }
        }
        return e.type;
    }

    // Conservative "does every path return" analysis.
    static bool stmtsReturn(const std::vector<StmtPtr>& stmts) {
        for (auto& s : stmts)
            if (stmtReturns(*s)) return true;
        return false;
    }
    static bool stmtReturns(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Return: return true;
        case StmtKind::Block: return stmtsReturn(s.body);
        case StmtKind::If:
            return !s.elseBody.empty() && stmtsReturn(s.body) && stmtsReturn(s.elseBody);
        default: return false; // while/for may run zero times
        }
    }
};

} // namespace

void analyze(Program& prog) {
    Sema().run(prog);
}
