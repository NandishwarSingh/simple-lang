#include "parser.hpp"
#include "diag.hpp"
#include <set>
#include <utility>

namespace {

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    Program parseProgram() {
        Program p;
        // imports come first, so `alias.fn(...)` can be recognized while
        // parsing the rest of the file
        while (at(Tok::KwImport)) {
            Import im;
            im.line = next().line;
            im.path = expect(Tok::StrLit, "a file path string").text;
            if (eat(Tok::KwAs)) {
                im.alias = expect(Tok::Ident, "import alias").text;
                if (!aliases_.insert(im.alias).second)
                    err(im.line, "alias '" + im.alias + "' is used twice");
            }
            expect(Tok::Semi);
            p.imports.push_back(std::move(im));
        }
        while (!at(Tok::Eof)) {
            if (at(Tok::KwStruct)) {
                p.structs.push_back(parseStructDecl());
            } else if (at(Tok::KwFn)) {
                p.funcs.push_back(parseFunction());
            } else if (at(Tok::KwExtern)) {
                p.funcs.push_back(parseExtern());
            } else if (at(Tok::KwImport)) {
                err(peek().line, "imports must come before any declaration");
            } else {
                err(peek().line,
                    std::string("expected 'fn', 'struct', or 'extern' at top level, found ") +
                        tokName(peek().kind));
            }
        }
        return p;
    }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;
    std::set<std::string> aliases_; // this file's `as` names

    const Token& peek2() const {
        return toks_[pos_ + 1 < toks_.size() ? pos_ + 1 : pos_];
    }

    const Token& peek() const { return toks_[pos_]; }
    Token next() {
        Token t = toks_[pos_];
        if (t.kind != Tok::Eof) pos_++;
        return t;
    }
    bool at(Tok k) const { return peek().kind == k; }
    bool eat(Tok k) {
        if (at(k)) { pos_++; return true; }
        return false;
    }
    Token expect(Tok k, const char* what = nullptr) {
        if (!at(k))
            err(peek().line, std::string("expected ") + (what ? what : tokName(k)) +
                                 ", found " + tokName(peek().kind));
        return next();
    }

    // A type is a base name (int/bool/str or a struct name) followed by any
    // number of [N] suffixes. Dims fold right-to-left so int[2][4] means
    // "2 arrays of (4 ints)" and g[r][c] indexes in the written order.
    // int / i8..i64 / u8..u64 / bool / str / a struct name — or 0 if not a
    // type name at all.
    static bool baseTypeFromName(const std::string& s, Type& out) {
        if (s == "int") { out = intType(64, false); return true; }
        if (s == "float") { out = floatType(64); return true; }
        if (s == "f64") { out = floatType(64); return true; }
        if (s == "f32") { out = floatType(32); return true; }
        if (s == "bool") { out = Type{TypeKind::Bool}; return true; }
        if (s == "str") { out = Type{TypeKind::Str}; return true; }
        if (s == "error") { out = Type{TypeKind::Error}; return true; }
        if ((s[0] == 'i' || s[0] == 'u') && s.size() >= 2) {
            std::string d = s.substr(1);
            if (d == "8" || d == "16" || d == "32" || d == "64") {
                out = intType(std::stoi(d), s[0] == 'u');
                return true;
            }
        }
        return false;
    }

    Type parseType() {
        if (eat(Tok::KwChan)) {
            Type c;
            c.kind = TypeKind::Chan;
            c.elem = std::make_shared<Type>(parseType());
            return c;
        }
        if (eat(Tok::KwList)) {
            Type l;
            l.kind = TypeKind::List;
            l.elem = std::make_shared<Type>(parseType());
            return l;
        }
        if (eat(Tok::KwMap)) { // map K V — key type, then value type
            Type m;
            m.kind = TypeKind::Map;
            m.key = std::make_shared<Type>(parseType());
            m.elem = std::make_shared<Type>(parseType());
            return m;
        }
        if (eat(Tok::Star)) { // raw pointer: *T
            Type p;
            p.kind = TypeKind::Ptr;
            p.elem = std::make_shared<Type>(parseType());
            return p;
        }
        Token t = expect(Tok::Ident, "type name");
        Type base;
        if (!baseTypeFromName(t.text, base)) {
            base.kind = TypeKind::Struct;
            base.sname = t.text;
        }
        std::vector<int64_t> dims;
        while (eat(Tok::LBracket)) {
            Token n = expect(Tok::IntLit, "array length");
            if (n.ival <= 0) err(n.line, "array length must be at least 1");
            if (n.ival > 100000000) err(n.line, "array length too large");
            expect(Tok::RBracket);
            dims.push_back(n.ival);
        }
        for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
            Type arr;
            arr.kind = TypeKind::Array;
            arr.alen = (int)*it;
            arr.elem = std::make_shared<Type>(std::move(base));
            base = std::move(arr);
        }
        return base;
    }

    StructDecl parseStructDecl() {
        StructDecl d;
        d.line = expect(Tok::KwStruct).line;
        d.name = expect(Tok::Ident, "struct name").text;
        expect(Tok::LBrace);
        while (!at(Tok::RBrace)) {
            Param f;
            Token id = expect(Tok::Ident, "field name");
            f.name = id.text;
            f.line = id.line;
            expect(Tok::Colon);
            f.type = parseType();
            d.fields.push_back(f);
            if (!eat(Tok::Comma)) break;
        }
        expect(Tok::RBrace);
        return d;
    }

    Function parseFunction() {
        Function f;
        f.line = expect(Tok::KwFn).line;
        f.name = expect(Tok::Ident, "function name").text;
        expect(Tok::LParen);
        if (!at(Tok::RParen)) {
            do {
                Param prm;
                Token id = expect(Tok::Ident, "parameter name");
                prm.name = id.text;
                prm.line = id.line;
                expect(Tok::Colon);
                prm.type = parseType();
                f.params.push_back(prm);
            } while (eat(Tok::Comma));
        }
        expect(Tok::RParen);
        if (eat(Tok::Arrow)) {
            // -> (T1, T2, ...) declares several return values
            if (eat(Tok::LParen)) {
                do f.rets.push_back(parseType());
                while (eat(Tok::Comma));
                expect(Tok::RParen);
                if (f.rets.size() < 2)
                    err(f.line, "a single return type needs no parentheses");
                f.ret.kind = TypeKind::Multi;
            } else {
                f.ret = parseType();
            }
        }
        f.body = parseBlock();
        return f;
    }

    // extern fn name(a: T, ...) -> T;   — implemented outside Simple (C)
    Function parseExtern() {
        Function f;
        f.isExtern = true;
        f.line = expect(Tok::KwExtern).line;
        expect(Tok::KwFn, "'fn' after 'extern'");
        f.name = expect(Tok::Ident, "function name").text;
        expect(Tok::LParen);
        if (!at(Tok::RParen)) {
            do {
                if (at(Tok::DotDot)) { // `..` reused as the variadic marker
                    next();
                    f.variadic = true;
                    break;
                }
                Param prm;
                Token id = expect(Tok::Ident, "parameter name");
                prm.name = id.text;
                prm.line = id.line;
                expect(Tok::Colon);
                prm.type = parseType();
                f.params.push_back(prm);
            } while (eat(Tok::Comma));
        }
        expect(Tok::RParen);
        if (eat(Tok::Arrow)) {
            if (at(Tok::LParen))
                err(peek().line, "extern functions cannot return multiple values");
            f.ret = parseType();
        }
        expect(Tok::Semi);
        return f;
    }

    std::vector<StmtPtr> parseBlock() {
        expect(Tok::LBrace);
        std::vector<StmtPtr> stmts;
        while (!at(Tok::RBrace) && !at(Tok::Eof)) stmts.push_back(parseStmt());
        expect(Tok::RBrace);
        return stmts;
    }

    StmtPtr mkStmt(StmtKind k, int line) {
        auto s = std::make_unique<Stmt>();
        s->kind = k;
        s->line = line;
        return s;
    }

    StmtPtr parseStmt() {
        int line = peek().line;
        if (at(Tok::KwLet)) {
            next();
            auto s = mkStmt(StmtKind::Let, line);
            s->isMut = eat(Tok::KwMut);
            // let (a, b) = f();  — receive several return values ("_" discards)
            if (eat(Tok::LParen)) {
                do s->names.push_back(expect(Tok::Ident, "variable name").text);
                while (eat(Tok::Comma));
                expect(Tok::RParen);
                if (s->names.size() < 2)
                    err(line, "a single variable needs no parentheses");
                expect(Tok::Assign);
                s->expr = parseExpr();
                expect(Tok::Semi);
                return s;
            }
            s->name = expect(Tok::Ident, "variable name").text;
            if (eat(Tok::Colon)) {
                s->hasType = true;
                s->declType = parseType();
            }
            expect(Tok::Assign);
            s->expr = parseExpr();
            expect(Tok::Semi);
            return s;
        }
        if (at(Tok::KwIf)) return parseIf();
        if (at(Tok::KwWhile)) {
            next();
            auto s = mkStmt(StmtKind::While, line);
            expect(Tok::LParen);
            s->expr = parseExpr();
            expect(Tok::RParen);
            s->body = parseBlock();
            return s;
        }
        if (at(Tok::KwFor)) {
            next();
            auto s = mkStmt(StmtKind::For, line);
            expect(Tok::LParen);
            s->name = expect(Tok::Ident, "loop variable").text;
            expect(Tok::KwIn);
            s->expr = parseExpr();
            // `for (i in a..b)` is a range; `for (k in m)` walks a map's keys
            if (eat(Tok::DotDot)) s->expr2 = parseExpr();
            expect(Tok::RParen);
            s->body = parseBlock();
            return s;
        }
        if (at(Tok::KwReturn)) {
            next();
            auto s = mkStmt(StmtKind::Return, line);
            if (!at(Tok::Semi)) {
                s->expr = parseExpr();
                if (at(Tok::Comma)) { // return a, b, ...;  (multi-value)
                    s->exprs.push_back(std::move(s->expr));
                    while (eat(Tok::Comma)) s->exprs.push_back(parseExpr());
                }
            }
            expect(Tok::Semi);
            return s;
        }
        if (at(Tok::KwSpawn)) {
            next();
            auto s = mkStmt(StmtKind::Spawn, line);
            s->expr = parseExpr();
            if (s->expr->kind != ExprKind::Call)
                err(line, "spawn needs a function call: `spawn worker(args);`");
            expect(Tok::Semi);
            return s;
        }
        if (at(Tok::KwBreak)) {
            next();
            expect(Tok::Semi);
            return mkStmt(StmtKind::Break, line);
        }
        if (at(Tok::KwContinue)) {
            next();
            expect(Tok::Semi);
            return mkStmt(StmtKind::Continue, line);
        }
        if (at(Tok::LBrace)) {
            auto s = mkStmt(StmtKind::Block, line);
            s->body = parseBlock();
            return s;
        }
        if (at(Tok::KwUnsafe)) {
            next();
            auto s = mkStmt(StmtKind::Unsafe, line);
            s->body = parseBlock();
            return s;
        }
        // expression statement or assignment
        ExprPtr e = parseExpr();
        if (at(Tok::Assign)) {
            if (e->kind != ExprKind::Var && e->kind != ExprKind::Field &&
                e->kind != ExprKind::Index && e->kind != ExprKind::Deref)
                err(peek().line,
                    "invalid assignment target (assign to a variable, field, or element)");
            next();
            auto s = mkStmt(StmtKind::Assign, e->line);
            s->lhs = std::move(e);
            s->expr = parseExpr();
            expect(Tok::Semi);
            return s;
        }
        auto s = mkStmt(StmtKind::ExprStmt, e->line);
        s->expr = std::move(e);
        expect(Tok::Semi);
        return s;
    }

    StmtPtr parseIf() {
        int line = expect(Tok::KwIf).line;
        auto s = mkStmt(StmtKind::If, line);
        expect(Tok::LParen);
        s->expr = parseExpr();
        expect(Tok::RParen);
        s->body = parseBlock();
        if (eat(Tok::KwElse)) {
            if (at(Tok::KwIf))
                s->elseBody.push_back(parseIf());
            else
                s->elseBody = parseBlock();
        }
        return s;
    }

    // ---- expressions, lowest to highest precedence ----

    ExprPtr mkExpr(ExprKind k, int line) {
        auto e = std::make_unique<Expr>();
        e->kind = k;
        e->line = line;
        return e;
    }

    ExprPtr mkBinary(Tok op, ExprPtr lhs, ExprPtr rhs, int line) {
        auto e = mkExpr(ExprKind::Binary, line);
        e->op = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }

    ExprPtr parseExpr() { return parseOr(); }

    ExprPtr parseOr() {
        ExprPtr e = parseAnd();
        while (at(Tok::OrOr)) {
            int line = next().line;
            e = mkBinary(Tok::OrOr, std::move(e), parseAnd(), line);
        }
        return e;
    }

    ExprPtr parseAnd() {
        ExprPtr e = parseEquality();
        while (at(Tok::AndAnd)) {
            int line = next().line;
            e = mkBinary(Tok::AndAnd, std::move(e), parseEquality(), line);
        }
        return e;
    }

    ExprPtr parseEquality() {
        ExprPtr e = parseComparison();
        while (at(Tok::EqEq) || at(Tok::NotEq)) {
            Token t = next();
            e = mkBinary(t.kind, std::move(e), parseComparison(), t.line);
        }
        return e;
    }

    ExprPtr parseComparison() {
        ExprPtr e = parseAdd();
        while (at(Tok::Lt) || at(Tok::Le) || at(Tok::Gt) || at(Tok::Ge)) {
            Token t = next();
            e = mkBinary(t.kind, std::move(e), parseAdd(), t.line);
        }
        return e;
    }

    // Go's precedence for bitwise ops, not C's: `|` and `^` sit with the
    // additives and `&` with the multiplicatives, so `a & b == c` parses
    // the way people read it instead of C's famous trap.
    ExprPtr parseAdd() {
        ExprPtr e = parseMul();
        while (at(Tok::Plus) || at(Tok::Minus) || at(Tok::Pipe) || at(Tok::Caret)) {
            Token t = next();
            e = mkBinary(t.kind, std::move(e), parseMul(), t.line);
        }
        return e;
    }

    ExprPtr parseMul() {
        ExprPtr e = parseUnary();
        while (at(Tok::Star) || at(Tok::Slash) || at(Tok::Percent) ||
               at(Tok::Amp) || at(Tok::Shl) || at(Tok::Shr)) {
            Token t = next();
            e = mkBinary(t.kind, std::move(e), parseUnary(), t.line);
        }
        return e;
    }

    ExprPtr parseUnary() {
        if (at(Tok::Minus) || at(Tok::Bang) || at(Tok::Tilde)) {
            Token t = next();
            auto e = mkExpr(ExprKind::Unary, t.line);
            e->op = t.kind;
            e->lhs = parseUnary();
            return e;
        }
        if (at(Tok::Amp)) { // &x — address of (unsafe only)
            Token t = next();
            auto e = mkExpr(ExprKind::AddrOf, t.line);
            e->lhs = parseUnary();
            return e;
        }
        if (at(Tok::Star)) { // *p — dereference (unsafe only)
            Token t = next();
            auto e = mkExpr(ExprKind::Deref, t.line);
            e->lhs = parseUnary();
            return e;
        }
        return parsePostfix();
    }

    // postfix: field access and indexing chain onto any primary
    ExprPtr parsePostfix() {
        ExprPtr e = parsePrimary();
        while (true) {
            if (at(Tok::Dot)) {
                int line = next().line;
                Token f = expect(Tok::Ident, "field name");
                auto n = mkExpr(ExprKind::Field, line);
                n->str = f.text;
                n->lhs = std::move(e);
                e = std::move(n);
            } else if (at(Tok::LBracket)) {
                int line = next().line;
                auto n = mkExpr(ExprKind::Index, line);
                n->lhs = std::move(e);
                n->rhs = parseExpr();
                expect(Tok::RBracket);
                e = std::move(n);
            } else {
                break;
            }
        }
        return e;
    }

    ExprPtr parsePrimary() {
        Token t = peek();
        if (t.kind == Tok::IntLit) {
            next();
            auto e = mkExpr(ExprKind::IntLit, t.line);
            e->ival = t.ival;
            return e;
        }
        if (t.kind == Tok::FloatLit) {
            next();
            auto e = mkExpr(ExprKind::FloatLit, t.line);
            e->fval = t.fval;
            return e;
        }
        if (t.kind == Tok::StrLit) {
            next();
            auto e = mkExpr(ExprKind::StrLit, t.line);
            e->str = t.text;
            return e;
        }
        if (t.kind == Tok::KwNull) {   // the null pointer (unsafe only)
            next();
            return mkExpr(ExprKind::NullLit, t.line);
        }
        if (t.kind == Tok::KwTrue || t.kind == Tok::KwFalse) {
            next();
            auto e = mkExpr(ExprKind::BoolLit, t.line);
            e->ival = t.kind == Tok::KwTrue ? 1 : 0;
            return e;
        }
        if (t.kind == Tok::LParen) {
            next();
            ExprPtr e = parseExpr();
            expect(Tok::RParen);
            return e;
        }
        if (t.kind == Tok::LBracket) { // array literal, or [value; count]
            next();
            auto e = mkExpr(ExprKind::ArrayLit, t.line);
            e->args.push_back(parseExpr());
            if (eat(Tok::Semi)) { // repeat form: every element the same
                Token n = expect(Tok::IntLit, "array length");
                if (n.ival <= 0) err(n.line, "array length must be at least 1");
                e->ival = n.ival;
                expect(Tok::RBracket);
                return e;
            }
            while (eat(Tok::Comma)) e->args.push_back(parseExpr());
            expect(Tok::RBracket);
            return e;
        }
        if (t.kind == Tok::KwChan) { // channel creation: chan int(16)
            next();
            auto e = mkExpr(ExprKind::ChanNew, t.line);
            e->type.kind = TypeKind::Chan;
            e->type.elem = std::make_shared<Type>(parseType());
            expect(Tok::LParen);
            e->args.push_back(parseExpr()); // capacity
            expect(Tok::RParen);
            return e;
        }
        if (t.kind == Tok::KwList) { // empty-list creation: list int
            next();
            auto e = mkExpr(ExprKind::ListNew, t.line);
            e->type.kind = TypeKind::List;
            e->type.elem = std::make_shared<Type>(parseType());
            return e;
        }
        if (t.kind == Tok::KwMap) { // empty-map creation: map str int
            next();
            auto e = mkExpr(ExprKind::MapNew, t.line);
            e->type.kind = TypeKind::Map;
            e->type.key = std::make_shared<Type>(parseType());
            e->type.elem = std::make_shared<Type>(parseType());
            return e;
        }
        // `ident.name(` is only ever a qualified call — Simple has no methods,
        // so a field access can't be called. If the head isn't a known alias,
        // say so plainly instead of a confusing "expected ')'".
        if (t.kind == Tok::Ident && !aliases_.count(t.text) &&
            peek2().kind == Tok::Dot) {
            size_t save = pos_;
            next(); next(); // ident, dot
            if (at(Tok::Ident)) {
                next();
                if (at(Tok::LParen))
                    err(t.line, "'" + t.text + "' is not an import alias "
                                "(add `import \"...\" as " + t.text + ";` to qualify "
                                "calls, or drop the prefix)");
            }
            pos_ = save; // not a call: fall through to normal field access
        }
        if (t.kind == Tok::Ident && aliases_.count(t.text) && peek2().kind == Tok::Dot) {
            // alias.fn(args) — a call into a specific imported file
            next();
            next(); // the dot
            Token nm = expect(Tok::Ident, "function name after import alias");
            expect(Tok::LParen, "'(' (an import alias qualifies function calls)");
            auto e = mkExpr(ExprKind::Call, t.line);
            e->str = nm.text;
            e->qual = t.text;
            if (!at(Tok::RParen)) {
                do {
                    e->args.push_back(parseExpr());
                } while (eat(Tok::Comma));
            }
            expect(Tok::RParen);
            return e;
        }
        if (t.kind == Tok::Ident) {
            next();
            // a type name followed by ( is a conversion: u8(x), i32(n)
            Type ct;
            if (at(Tok::LParen) && baseTypeFromName(t.text, ct) &&
                (ct.kind == TypeKind::Int || ct.kind == TypeKind::Float ||
                 ct.kind == TypeKind::Str)) {
                next();
                auto e = mkExpr(ExprKind::Cast, t.line);
                e->type = ct;
                e->lhs = parseExpr();
                expect(Tok::RParen);
                return e;
            }
            if (eat(Tok::LParen)) { // call
                auto e = mkExpr(ExprKind::Call, t.line);
                e->str = t.text;
                if (!at(Tok::RParen)) {
                    do {
                        e->args.push_back(parseExpr());
                    } while (eat(Tok::Comma));
                }
                expect(Tok::RParen);
                return e;
            }
            if (at(Tok::LBrace)) { // struct literal: Name { field: value, ... }
                next();
                auto e = mkExpr(ExprKind::StructLit, t.line);
                e->str = t.text;
                while (!at(Tok::RBrace)) {
                    Token f = expect(Tok::Ident, "field name");
                    expect(Tok::Colon);
                    e->fieldNames.push_back(f.text);
                    e->args.push_back(parseExpr());
                    if (!eat(Tok::Comma)) break;
                }
                expect(Tok::RBrace);
                return e;
            }
            auto e = mkExpr(ExprKind::Var, t.line);
            e->str = t.text;
            return e;
        }
        err(t.line, std::string("expected an expression, found ") + tokName(t.kind));
    }
};

} // namespace

Program parse(std::vector<Token> toks) {
    return Parser(std::move(toks)).parseProgram();
}
