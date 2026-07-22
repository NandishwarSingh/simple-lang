#pragma once
#include <cstdint>
#include <string>

enum class Tok {
    Eof, Ident, IntLit, FloatLit, StrLit,
    // keywords
    KwFn, KwLet, KwMut, KwReturn, KwIf, KwElse, KwWhile,
    KwTrue, KwFalse, KwBreak, KwContinue, KwStruct, KwFor, KwIn,
    KwChan, KwSpawn, KwUnsafe, KwExtern, KwNull, KwList, KwMap,
    KwImport, KwAs,
    // punctuation & operators
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semi, Colon, Arrow, Dot, DotDot,
    Plus, Minus, Star, Slash, Percent,
    Assign, EqEq, NotEq, Lt, Le, Gt, Ge, Bang, AndAnd, OrOr,
    Amp, Pipe, Caret, Tilde, Shl, Shr,
};

struct Token {
    Tok kind = Tok::Eof;
    std::string text;  // identifier name or string literal value
    int64_t ival = 0;  // integer literal value
    double fval = 0;   // float literal value
    int line = 1;
};

// Printable name for error messages, e.g. "';'" or "identifier".
const char* tokName(Tok t);
