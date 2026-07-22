#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "token.hpp"

// Multi never appears inside a Type tree: it is only the `ret` sentinel of a
// function returning several values (the real types live in Function::rets).
enum class TypeKind { Int, Bool, Str, Void, Struct, Array, Chan, Ptr, Float, List, Error, Multi, Map };

struct Type {
    TypeKind kind = TypeKind::Void;
    std::string sname;          // Struct: the struct's name
    int alen = 0;               // Array: element count
    std::shared_ptr<Type> elem; // Array/Chan/Ptr/List/Map: element (value) type
    std::shared_ptr<Type> key;  // Map: key type (Str or Int)
    int bits = 64;              // Int: 8/16/32/64
    bool uns = false;           // Int: unsigned?
};

inline bool operator==(const Type& a, const Type& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == TypeKind::Int) return a.bits == b.bits && a.uns == b.uns;
    if (a.kind == TypeKind::Float) return a.bits == b.bits;
    if (a.kind == TypeKind::Struct) return a.sname == b.sname;
    if (a.kind == TypeKind::Array) return a.alen == b.alen && *a.elem == *b.elem;
    if (a.kind == TypeKind::Chan || a.kind == TypeKind::Ptr ||
        a.kind == TypeKind::List) return *a.elem == *b.elem;
    if (a.kind == TypeKind::Map) return *a.key == *b.key && *a.elem == *b.elem;
    return true;
}
inline bool operator!=(const Type& a, const Type& b) { return !(a == b); }

inline bool isAggregate(const Type& t) {
    return t.kind == TypeKind::Struct || t.kind == TypeKind::Array;
}
inline bool isInt(const Type& t) { return t.kind == TypeKind::Int; }
inline bool isFloat(const Type& t) { return t.kind == TypeKind::Float; }
inline bool isNumeric(const Type& t) { return isInt(t) || isFloat(t); }

inline Type intType(int bits = 64, bool uns = false) {
    Type t;
    t.kind = TypeKind::Int;
    t.bits = bits;
    t.uns = uns;
    return t;
}
inline Type floatType(int bits = 64) {
    Type t;
    t.kind = TypeKind::Float;
    t.bits = bits;
    return t;
}

inline std::string typeName(const Type& t) {
    switch (t.kind) {
    case TypeKind::Int:
        if (t.bits == 64 && !t.uns) return "int";
        return (t.uns ? "u" : "i") + std::to_string(t.bits);
    case TypeKind::Float:
        return t.bits == 64 ? "float" : "f32";
    case TypeKind::Bool: return "bool";
    case TypeKind::Str:  return "str";
    case TypeKind::Error: return "error";
    case TypeKind::Multi: return "(multiple values)";
    case TypeKind::Void: return "void";
    case TypeKind::Struct: return t.sname;
    case TypeKind::Chan: return "chan " + typeName(*t.elem);
    case TypeKind::List: return "list " + typeName(*t.elem);
    case TypeKind::Map:  return "map " + typeName(*t.key) + " " + typeName(*t.elem);
    case TypeKind::Ptr: return "*" + typeName(*t.elem);
    case TypeKind::Array: {
        // print dims outside-in so int[2][4] displays as written
        std::string dims;
        const Type* c = &t;
        while (c->kind == TypeKind::Array) {
            dims += "[" + std::to_string(c->alen) + "]";
            c = c->elem.get();
        }
        return typeName(*c) + dims;
    }
    }
    return "?";
}

// ---- expressions ----

enum class ExprKind {
    IntLit, FloatLit, BoolLit, StrLit, Var, Unary, Binary, Call,
    ArrayLit, Index, Field, StructLit, ChanNew,
    Cast, AddrOf, Deref, NullLit, ListNew, MapNew,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind;
    int line = 0;
    Type type;               // filled in by sema
    int64_t ival = 0;        // IntLit value, BoolLit 0/1
    double fval = 0;         // FloatLit value
    std::string str;         // StrLit value, Var name, Call callee, Field name, StructLit name
    std::string qual;        // Call: import alias in `alias.fn(...)` (v0.9)
    Tok op = Tok::Eof;       // Unary/Binary operator
    ExprPtr lhs, rhs;        // Binary operands; Unary uses lhs; Index base/index; Field base=lhs
    std::vector<ExprPtr> args;           // Call args, ArrayLit elements, StructLit values
    std::vector<std::string> fieldNames; // StructLit: name per args entry
};

// ---- statements ----

enum class StmtKind {
    Let, Assign, ExprStmt, Return, If, While, For, Block, Break, Continue, Spawn,
    Unsafe,
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
    StmtKind kind;
    int line = 0;
    std::string name;              // Let variable / For loop variable
    bool isMut = false;            // Let
    bool hasType = false;          // Let: explicit type annotation present
    Type declType;                 // Let annotation
    ExprPtr lhs;                   // Assign target (Var, Field, or Index chain)
    ExprPtr expr;                  // init / value / condition / return value / For start
    ExprPtr expr2;                 // For end bound
    std::vector<ExprPtr> exprs;    // Return: the values of `return a, b;` (multi only)
    std::vector<std::string> names;// Let: `let (a, b) = f();` targets, "_" = discard
    std::vector<StmtPtr> body;     // If-then / While / For body / Block
    std::vector<StmtPtr> elseBody; // If-else (an `else if` is one nested If stmt)
};

// ---- declarations ----

struct Param {
    std::string name;
    Type type;
    int line = 0;
};

struct StructDecl {
    std::string name;
    std::vector<Param> fields;
    int line = 0;
    int fileId = 0;          // which source file declared it (v0.9)
};

struct Function {
    std::string name;        // as written in source
    std::string linkName;    // unique across the program (sema fills; = name
                             // unless the name repeats in another file)
    int fileId = 0;          // which source file declared it (v0.9)
    std::vector<Param> params;
    Type ret{TypeKind::Void};       // TypeKind::Multi when rets is non-empty
    std::vector<Type> rets;         // `-> (T1, T2, ...)`: the individual types
    std::vector<StmtPtr> body;
    int line = 0;
    bool isExtern = false;  // `extern fn` — declared elsewhere (C), no body
    bool variadic = false;  // extern only: trailing `...`
};

// v0.9 modules: `import "file.simp";` makes that file's names usable here
// unqualified; `import "file.simp" as x;` makes them usable as `x.name(...)`.
struct Import {
    std::string path;        // as written
    std::string alias;       // empty for a plain import
    int line = 0;
    int fileId = -1;         // resolved by the driver
};

struct SourceFile {
    std::string path;                // canonical path (diagnostics, identity)
    std::vector<Import> imports;
};

struct Program {
    std::vector<StructDecl> structs;
    std::vector<Function> funcs;
    std::vector<SourceFile> files;   // files[0] is the root file
    std::vector<Import> imports;     // parser scratch: the current file's imports
};
