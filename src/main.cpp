#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <limits.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "codegen.hpp"
#include "diag.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "qbe.h"
#include "sema.hpp"
#include "vectorize.hpp"

static void usage() {
    std::cerr <<
        "simplec — the Simple language compiler\n"
        "\n"
        "usage:\n"
        "  simplec <file.simp> [-o <output>]   compile to a native executable\n"
        "  simplec run <file.simp>             compile and immediately run\n"
        "  simplec <file.simp> --emit-ssa      print QBE IR instead of compiling\n"
        "  simplec <file.simp> --tokens        print lexer output (debug)\n"
        "  simplec <file.simp> --vec-report    show which loops can vectorize (v0.95)\n"
        "  simplec <file.simp> --no-opt        disable optimization passes\n"
        "  simplec <file.simp> --link NAME     link against libNAME\n"
        "  simplec <file.simp> --libdir DIR    add a library search path\n"
        "  simplec <file.simp> --target <t>    cross-compile: amd64_apple,\n"
        "                                      amd64_sysv, arm64, arm64_apple, rv64\n";
    exit(1);
}

static int runCmd(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 128;
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    bool runAfter = false, emitSsa = false, dumpTokens = false, noOpt = false;
    bool vecReport = false, compileSsa = false;
    std::string src, out, target, archFlag, linkFlags;
    for (size_t i = 0; i < args.size(); i++) {
        std::string& a = args[i];
        if (i == 0 && a == "run") runAfter = true;
        else if (a == "-o") {
            if (i + 1 >= args.size()) usage();
            out = args[++i];
        }
        else if (a == "--link") {          // -lNAME passed to the linker
            if (i + 1 >= args.size()) usage();
            linkFlags += " -l" + args[++i];
        }
        else if (a == "--libdir") {        // -LPATH passed to the linker
            if (i + 1 >= args.size()) usage();
            linkFlags += " -L'" + args[++i] + "'";
        }
        else if (a == "--target") {
            if (i + 1 >= args.size()) usage();
            target = args[++i];
            // tell the assembler/linker which machine we produced code for
            if (target.rfind("amd64", 0) == 0) archFlag = " -arch x86_64";
            else if (target.rfind("arm64", 0) == 0) archFlag = " -arch arm64";
        }
        else if (a == "--run") runAfter = true;
        else if (a == "--emit-ssa") emitSsa = true;
        else if (a == "--vec-report") vecReport = true;
        else if (a == "--compile-ssa") compileSsa = true; // dev: feed raw QBE IR
        else if (a == "--no-opt") noOpt = true;
        else if (a == "--tokens") dumpTokens = true;
        else if (!a.empty() && a[0] == '-') usage();
        else if (src.empty()) src = a;
        else usage();
    }
    if (src.empty()) usage();

    // v0.9 modules: load the root file and everything it imports,
    // transitively, each file exactly once (canonical-path dedup), into
    // one whole-program AST. Import paths are relative to the importer.
    std::string activeFile = src; // for error attribution
    auto readFile = [](const std::string& path, std::string& out) {
        std::ifstream in(path);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    };
    auto canon = [](const std::string& path) {
        char buf[PATH_MAX];
        if (realpath(path.c_str(), buf)) return std::string(buf);
        return path;
    };
    auto dirOf = [](const std::string& path) {
        size_t sl = path.find_last_of('/');
        return sl == std::string::npos ? std::string(".") : path.substr(0, sl);
    };

    try {
        // dev harness: compile a hand-written .ssa straight through the
        // backend (for testing QBE changes in isolation, e.g. v0.95 vectors)
        if (compileSsa) {
            std::string out2 = out.empty() ? src + ".bin" : out;
            std::string asmPath = out2 + ".s";
            if (qbe_compile(src.c_str(), asmPath.c_str(),
                            target.empty() ? nullptr : target.c_str()) != 0) {
                std::cerr << "error: backend failed\n";
                return 1;
            }
            if (runCmd("cc" + archFlag + " '" + asmPath + "' -o '" + out2 + "'" +
                       linkFlags) != 0) {
                std::cerr << "error: assembling/linking failed\n";
                return 1;
            }
            if (runAfter) {
                std::string exe = out2.find('/') == std::string::npos ? "./" + out2 : out2;
                return runCmd("'" + exe + "'");
            }
            std::cout << out2 << "\n";
            return 0;
        }
        if (dumpTokens) {
            std::string code;
            if (!readFile(src, code)) {
                std::cerr << "error: cannot open '" << src << "'\n";
                return 1;
            }
            auto toks = lex(code);
            for (auto& t : toks) {
                std::cout << t.line << ": " << tokName(t.kind);
                if (t.kind == Tok::Ident || t.kind == Tok::StrLit)
                    std::cout << " \"" << t.text << "\"";
                if (t.kind == Tok::IntLit) std::cout << " " << t.ival;
                std::cout << "\n";
            }
            return 0;
        }

        Program prog;
        std::map<std::string, int> loaded; // canonical path -> fileId
        // worklist of (display path, canonical path) still to parse; an id
        // is assigned when a file is first seen, so cycles just resolve
        struct Pending { std::string path; int id; };
        std::vector<Pending> work;
        auto addFile = [&](const std::string& path) {
            std::string c = canon(path);
            auto it = loaded.find(c);
            if (it != loaded.end()) return it->second;
            int id = (int)prog.files.size();
            loaded[c] = id;
            prog.files.push_back(SourceFile{path, {}});
            work.push_back({path, id});
            return id;
        };
        addFile(src);
        while (!work.empty()) {
            Pending cur = work.back();
            work.pop_back();
            activeFile = cur.path;
            std::string code;
            if (!readFile(cur.path, code)) {
                std::cerr << "error: cannot open '" << cur.path << "'\n";
                return 1;
            }
            Program part = parse(lex(code));
            for (auto& im : part.imports) {
                std::string full = im.path;
                if (!full.empty() && full[0] != '/')
                    full = dirOf(cur.path) + "/" + full;
                im.fileId = addFile(full);
            }
            prog.files[cur.id].imports = std::move(part.imports);
            for (auto& d : part.structs) {
                d.fileId = cur.id;
                prog.structs.push_back(std::move(d));
            }
            for (auto& f : part.funcs) {
                f.fileId = cur.id;
                prog.funcs.push_back(std::move(f));
            }
        }
        activeFile = src;
        analyze(prog);
        if (vecReport) {
            vectorReport(prog, std::cout);
            return 0;
        }
        std::string ssa = genQBE(prog, !noOpt);
        if (emitSsa) {
            std::cout << ssa;
            return 0;
        }

        if (out.empty()) {
            out = src;
            const std::string ext = ".simp";
            if (out.size() > ext.size() &&
                out.compare(out.size() - ext.size(), ext.size(), ext) == 0)
                out = out.substr(0, out.size() - ext.size());
            else
                out += ".bin";
        }
        std::string ssaPath = out + ".ssa";
        std::string asmPath = out + ".s";
        {
            std::ofstream f(ssaPath);
            f << ssa;
        }
        if (qbe_compile(ssaPath.c_str(), asmPath.c_str(),
                        target.empty() ? nullptr : target.c_str()) != 0) {
            std::cerr << "error: backend failed\n";
            return 1;
        }
        if (runCmd("cc" + archFlag + " '" + asmPath + "' -o '" + out + "'" +
                   linkFlags) != 0) {
            std::cerr << "error: assembling/linking failed\n";
            return 1;
        }
        if (runAfter) {
            std::string exe = out.find('/') == std::string::npos ? "./" + out : out;
            return runCmd("'" + exe + "'");
        }
        std::cout << out << "\n";
        return 0;
    } catch (const CompileError& e) {
        const std::string& f = e.file.empty() ? activeFile : e.file;
        std::cerr << f << ":" << e.line << ": error: " << e.msg << "\n";
        return 1;
    }
}
