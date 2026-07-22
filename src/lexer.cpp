#include "lexer.hpp"
#include "diag.hpp"
#include <cctype>
#include <cstdlib>
#include <unordered_map>

const char* tokName(Tok t) {
    switch (t) {
    case Tok::Eof:        return "end of file";
    case Tok::Ident:      return "identifier";
    case Tok::IntLit:     return "integer literal";
    case Tok::FloatLit:   return "float literal";
    case Tok::StrLit:     return "string literal";
    case Tok::KwFn:       return "'fn'";
    case Tok::KwLet:      return "'let'";
    case Tok::KwMut:      return "'mut'";
    case Tok::KwReturn:   return "'return'";
    case Tok::KwIf:       return "'if'";
    case Tok::KwElse:     return "'else'";
    case Tok::KwWhile:    return "'while'";
    case Tok::KwTrue:     return "'true'";
    case Tok::KwFalse:    return "'false'";
    case Tok::KwBreak:    return "'break'";
    case Tok::KwContinue: return "'continue'";
    case Tok::KwStruct:   return "'struct'";
    case Tok::KwFor:      return "'for'";
    case Tok::KwIn:       return "'in'";
    case Tok::KwChan:     return "'chan'";
    case Tok::KwSpawn:    return "'spawn'";
    case Tok::KwUnsafe:   return "'unsafe'";
    case Tok::KwExtern:   return "'extern'";
    case Tok::KwNull:     return "'null'";
    case Tok::KwList:     return "'list'";
    case Tok::KwMap:      return "'map'";
    case Tok::KwImport:   return "'import'";
    case Tok::KwAs:       return "'as'";
    case Tok::Amp:        return "'&'";
    case Tok::Pipe:       return "'|'";
    case Tok::Caret:      return "'^'";
    case Tok::Tilde:      return "'~'";
    case Tok::Shl:        return "'<<'";
    case Tok::Shr:        return "'>>'";
    case Tok::LParen:     return "'('";
    case Tok::RParen:     return "')'";
    case Tok::LBrace:     return "'{'";
    case Tok::RBrace:     return "'}'";
    case Tok::LBracket:   return "'['";
    case Tok::RBracket:   return "']'";
    case Tok::Comma:      return "','";
    case Tok::Semi:       return "';'";
    case Tok::Colon:      return "':'";
    case Tok::Arrow:      return "'->'";
    case Tok::Dot:        return "'.'";
    case Tok::DotDot:     return "'..'";
    case Tok::Plus:       return "'+'";
    case Tok::Minus:      return "'-'";
    case Tok::Star:       return "'*'";
    case Tok::Slash:      return "'/'";
    case Tok::Percent:    return "'%'";
    case Tok::Assign:     return "'='";
    case Tok::EqEq:       return "'=='";
    case Tok::NotEq:      return "'!='";
    case Tok::Lt:         return "'<'";
    case Tok::Le:         return "'<='";
    case Tok::Gt:         return "'>'";
    case Tok::Ge:         return "'>='";
    case Tok::Bang:       return "'!'";
    case Tok::AndAnd:     return "'&&'";
    case Tok::OrOr:       return "'||'";
    }
    return "?";
}

static const std::unordered_map<std::string, Tok> kKeywords = {
    {"fn", Tok::KwFn},         {"let", Tok::KwLet},     {"mut", Tok::KwMut},
    {"return", Tok::KwReturn}, {"if", Tok::KwIf},       {"else", Tok::KwElse},
    {"while", Tok::KwWhile},   {"true", Tok::KwTrue},   {"false", Tok::KwFalse},
    {"break", Tok::KwBreak},   {"continue", Tok::KwContinue},
    {"struct", Tok::KwStruct}, {"for", Tok::KwFor},     {"in", Tok::KwIn},
    {"chan", Tok::KwChan},     {"spawn", Tok::KwSpawn},
    {"unsafe", Tok::KwUnsafe}, {"extern", Tok::KwExtern},
    {"null", Tok::KwNull},     {"list", Tok::KwList},   {"map", Tok::KwMap},
    {"import", Tok::KwImport}, {"as", Tok::KwAs},
};

std::vector<Token> lex(const std::string& src) {
    std::vector<Token> toks;
    size_t i = 0, n = src.size();
    int line = 1;
    auto push = [&](Tok k) {
        Token t; t.kind = k; t.line = line;
        toks.push_back(t);
    };

    while (i < n) {
        char c = src[i];
        if (c == '\n') { line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }

        // comments
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            int start = line;
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') line++;
                i++;
            }
            if (i + 1 >= n) err(start, "unterminated block comment");
            i += 2;
            continue;
        }

        // identifiers & keywords
        if (isalpha((unsigned char)c) || c == '_') {
            size_t s = i;
            while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '_')) i++;
            std::string word = src.substr(s, i - s);
            auto it = kKeywords.find(word);
            if (it != kKeywords.end()) {
                push(it->second);
            } else {
                Token t; t.kind = Tok::Ident; t.text = word; t.line = line;
                toks.push_back(t);
            }
            continue;
        }

        // integer literals (decimal or 0x hex)
        if (isdigit((unsigned char)c)) {
            // the full u64 range is allowed; the bits are kept as-is and the
            // type system decides how to read them
            const unsigned long long kMax = 18446744073709551615ULL;
            unsigned long long v = 0;
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                if (i >= n || !isxdigit((unsigned char)src[i]))
                    err(line, "invalid hex literal");
                while (i < n && isxdigit((unsigned char)src[i])) {
                    char h = src[i];
                    int d = isdigit((unsigned char)h) ? h - '0' : tolower(h) - 'a' + 10;
                    if (v > (kMax - (unsigned long long)d) / 16) err(line, "integer literal too large");
                    v = v * 16 + (unsigned long long)d;
                    i++;
                }
            } else {
                size_t intStart = i;
                while (i < n && isdigit((unsigned char)src[i])) {
                    int d = src[i] - '0';
                    if (v > (kMax - (unsigned long long)d) / 10) err(line, "integer literal too large");
                    v = v * 10 + (unsigned long long)d;
                    i++;
                }
                // a float literal: `1.5`, `0.25`, `2e10`, `1.5e-3`. The dot
                // must be followed by a digit so `0..5` (a range) and `p.x`
                // stay separate tokens.
                bool isFloat = false;
                if (i + 1 < n && src[i] == '.' && isdigit((unsigned char)src[i + 1])) {
                    isFloat = true;
                    i++; // the dot
                    while (i < n && isdigit((unsigned char)src[i])) i++;
                }
                if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                    size_t j = i + 1;
                    if (j < n && (src[j] == '+' || src[j] == '-')) j++;
                    if (j < n && isdigit((unsigned char)src[j])) {
                        isFloat = true;
                        i = j;
                        while (i < n && isdigit((unsigned char)src[i])) i++;
                    }
                }
                if (isFloat) {
                    Token t; t.kind = Tok::FloatLit;
                    t.fval = strtod(src.substr(intStart, i - intStart).c_str(), nullptr);
                    t.line = line;
                    toks.push_back(t);
                    continue;
                }
            }
            Token t; t.kind = Tok::IntLit; t.ival = (int64_t)v; t.line = line;
            toks.push_back(t);
            continue;
        }

        // character literal: 'A' is just the byte value 65, an int literal.
        // Supports the same escapes as strings.
        if (c == '\'') {
            i++;
            if (i >= n) err(line, "unterminated character literal");
            int val;
            if (src[i] == '\\') {
                if (i + 1 >= n) err(line, "unterminated character literal");
                char e = src[i + 1];
                switch (e) {
                case 'n':  val = '\n'; break;
                case 't':  val = '\t'; break;
                case 'r':  val = '\r'; break;
                case '\\': val = '\\'; break;
                case '\'': val = '\''; break;
                case '0':  val = '\0'; break;
                default:   err(line, std::string("unknown escape '\\") + e + "'");
                }
                i += 2;
            } else if (src[i] == '\'') {
                err(line, "empty character literal");
            } else {
                val = (unsigned char)src[i];
                i++;
            }
            if (i >= n || src[i] != '\'')
                err(line, "character literal must be one character in single quotes");
            i++; // closing quote
            Token t; t.kind = Tok::IntLit; t.ival = val; t.line = line;
            toks.push_back(t);
            continue;
        }

        // string literals
        if (c == '"') {
            int start = line;
            i++;
            std::string val;
            while (i < n && src[i] != '"') {
                char d = src[i];
                if (d == '\n') err(start, "unterminated string literal");
                if (d == '\\') {
                    if (i + 1 >= n) err(start, "unterminated string literal");
                    char e = src[i + 1];
                    switch (e) {
                    case 'n':  val += '\n'; break;
                    case 't':  val += '\t'; break;
                    case 'r':  val += '\r'; break;
                    case '\\': val += '\\'; break;
                    case '"':  val += '"';  break;
                    case '0':  val += '\0'; break;
                    default:   err(line, std::string("unknown escape '\\") + e + "'");
                    }
                    i += 2;
                } else {
                    val += d;
                    i++;
                }
            }
            if (i >= n) err(start, "unterminated string literal");
            i++; // closing quote
            Token t; t.kind = Tok::StrLit; t.text = val; t.line = start;
            toks.push_back(t);
            continue;
        }

        // operators & punctuation
        auto two = [&](char b) { return i + 1 < n && src[i + 1] == b; };
        switch (c) {
        case '(': push(Tok::LParen); i++; continue;
        case ')': push(Tok::RParen); i++; continue;
        case '{': push(Tok::LBrace); i++; continue;
        case '}': push(Tok::RBrace); i++; continue;
        case '[': push(Tok::LBracket); i++; continue;
        case ']': push(Tok::RBracket); i++; continue;
        case '.':
            if (two('.')) { push(Tok::DotDot); i += 2; } else { push(Tok::Dot); i++; }
            continue;
        case ',': push(Tok::Comma); i++; continue;
        case ';': push(Tok::Semi); i++; continue;
        case ':': push(Tok::Colon); i++; continue;
        case '+': push(Tok::Plus); i++; continue;
        case '*': push(Tok::Star); i++; continue;
        case '/': push(Tok::Slash); i++; continue;
        case '%': push(Tok::Percent); i++; continue;
        case '-':
            if (two('>')) { push(Tok::Arrow); i += 2; } else { push(Tok::Minus); i++; }
            continue;
        case '=':
            if (two('=')) { push(Tok::EqEq); i += 2; } else { push(Tok::Assign); i++; }
            continue;
        case '!':
            if (two('=')) { push(Tok::NotEq); i += 2; } else { push(Tok::Bang); i++; }
            continue;
        case '<':
            if (two('=')) { push(Tok::Le); i += 2; }
            else if (two('<')) { push(Tok::Shl); i += 2; }
            else { push(Tok::Lt); i++; }
            continue;
        case '>':
            if (two('=')) { push(Tok::Ge); i += 2; }
            else if (two('>')) { push(Tok::Shr); i += 2; }
            else { push(Tok::Gt); i++; }
            continue;
        case '&':
            if (two('&')) { push(Tok::AndAnd); i += 2; } else { push(Tok::Amp); i++; }
            continue;
        case '|':
            if (two('|')) { push(Tok::OrOr); i += 2; } else { push(Tok::Pipe); i++; }
            continue;
        case '^': push(Tok::Caret); i++; continue;
        case '~': push(Tok::Tilde); i++; continue;
        default:
            err(line, std::string("unexpected character '") + c + "'");
        }
    }
    push(Tok::Eof);
    return toks;
}
