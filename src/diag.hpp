#pragma once
#include <string>

// A compile error at a source line. Thrown by every stage, caught in main.
// `file` is filled in by whoever knows which file the line belongs to
// (the driver for parse errors, sema for body errors) — v0.9 multi-file.
struct CompileError {
    int line;
    std::string msg;
    std::string file;
};

[[noreturn]] inline void err(int line, const std::string& msg) {
    throw CompileError{line, msg, ""};
}
