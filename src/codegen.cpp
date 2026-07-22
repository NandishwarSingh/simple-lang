#include "codegen.hpp"
#include "diag.hpp"
#include "vectorize.hpp"
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

// Lowering strategy (see docs/internals/05-codegen.md):
// - Scalars are SSA temporaries; variables live in stack slots hoisted into
//   @start, which QBE promotes to registers.
// - Aggregates are stack storage; aggregate expressions evaluate to POINTERS.
//   Value semantics come from `blit` copies at every boundary.
// - ARC (v0.3): heap strings carry a 16-byte header {refcount, len} at
//   pointer-16; literals are emitted with an immortal count (-1). Counting
//   is non-atomic by design (channels will deep-copy strings on send).
//   Ownership conventions: function results are +1 (caller owns); plain
//   arguments are borrowed (no retain); `let` copies retain; reassignment
//   releases the old value; scope exit releases owned live strings;
//   `return local` elides its retain+release pair. Statement temporaries
//   (e.g. the a+b inside f(a+b)) are released at end of statement.
//   Aggregates containing strings get generated $rc_ret_*/$rc_rel_* helpers.

namespace {

// ---- MIR (v0.5): functions of basic blocks of parsed instructions ----
// Built by the same emission logic as before; passes transform it; a dumb
// printer emits QBE text. Untouched instructions keep their original text,
// so with --no-opt the output is byte-identical to direct emission.

struct MInst {
    std::string text; // exact QBE line (sans leading tab)
    std::string dst;  // "%t3" or ""
    char ty = 0;      // result type ('l'/'w') when dst present
    std::string op;   // "add", "call", "storel", "jnz", "jmp", "ret", ...
    std::vector<std::string> args; // operands; call: [0]=callee, then "T %v" pairs
};

struct MBlock {
    std::string label; // "@start", "@if_then_1", ...
    std::vector<MInst> ins;
};

struct MFunc {
    std::string sig;  // full signature line "export function w $main(...) {"
    std::string name;
    Function* fn = nullptr;
    bool aggRet = false;
    std::vector<std::string> allocs; // "\t%x_1 =l alloc8 8" lines
    std::vector<MBlock> blocks;      // blocks[0] is @start
};

static std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t c = s.find(", ", pos);
        if (c == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, c - pos));
        pos = c + 2;
    }
    return out;
}

static MInst parseInst(const std::string& s) {
    MInst m;
    m.text = s;
    std::string rest = s;
    if (s[0] == '%') {
        size_t eq = s.find(" =");
        m.dst = s.substr(0, eq);
        m.ty = s[eq + 2];
        rest = s.substr(eq + 4);
    }
    size_t sp = rest.find(' ');
    if (sp == std::string::npos) {
        m.op = rest; // "ret", "hlt"
        return m;
    }
    m.op = rest.substr(0, sp);
    std::string tail = rest.substr(sp + 1);
    if (m.op == "call") {
        size_t lp = tail.find('(');
        m.args.push_back(tail.substr(0, lp));
        std::string inner = tail.substr(lp + 1, tail.rfind(')') - lp - 1);
        if (!inner.empty())
            for (auto& a : splitArgs(inner)) m.args.push_back(a);
    } else {
        for (auto& a : splitArgs(tail)) m.args.push_back(a);
    }
    return m;
}

static std::string renderInst(const MInst& m) {
    std::string s;
    if (!m.dst.empty()) s = m.dst + " =" + std::string(1, m.ty) + " ";
    s += m.op;
    if (m.op == "call") {
        s += " " + m.args[0] + "(";
        for (size_t i = 1; i < m.args.size(); i++) {
            if (i > 1) s += ", ";
            s += m.args[i];
        }
        s += ")";
    } else if (!m.args.empty()) {
        s += " ";
        for (size_t i = 0; i < m.args.size(); i++) {
            if (i) s += ", ";
            s += m.args[i];
        }
    }
    return s;
}

static MInst mkMInst(const std::string& dst, char ty, const std::string& op,
                     std::vector<std::string> args) {
    MInst m;
    m.dst = dst;
    m.ty = ty;
    m.op = op;
    m.args = std::move(args);
    m.text = renderInst(m);
    return m;
}

static bool isIntConst(const std::string& s, long long& out) {
    if (s.empty()) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i >= s.size()) return false;
    for (size_t j = i; j < s.size(); j++)
        if (!isdigit((unsigned char)s[j])) return false;
    out = strtoll(s.c_str(), nullptr, 10);
    return true;
}

static bool isPow2(long long v, int& k) {
    if (v <= 0 || (v & (v - 1)) != 0) return false;
    k = 0;
    while ((1LL << k) < v) k++;
    return true;
}

struct Slot {
    std::string addr;
    Type type;
    bool ownsRefs = true; // false for borrowed str params: no release at exit
};

struct Layout {
    long size = 0;
    long align = 1;
    std::unordered_map<std::string, std::pair<long, Type>> fields; // name -> {offset, type}
};

struct LoopCtx {
    std::string cont, brk;
    size_t depth; // scope index: break/continue release scopes >= depth
};

class Codegen {
public:
    std::string run(Program& p, bool optimize) {
        optimize_ = optimize;
        for (auto& d : p.structs) structs_[d.name] = &d;
        for (auto& f : p.funcs) fns_[f.linkName] = &f; // unique per program (v0.9)
        // extern declarations have no body: they are just names to call
        for (auto& f : p.funcs)
            if (!f.isExtern) genFunction(f);

        if (optimize_) {
            inlinePass();
            strengthPass();
            foldDcePass();
        }

        std::ostringstream funcs;
        for (auto& f : mfuncs_) funcs << printFunc(f) << "\n";

        std::ostringstream mod;
        mod << "# generated by simplec\n";
        // main always stores argc/argv here (3 instructions); the IO
        // builtins read them. Unconditional so main's prologue never
        // references an undefined symbol.
        mod << "data $simple_argc = { l 0 }\n";
        mod << "data $simple_argv = { l 0 }\n";
        // read_file/write_file failure messages concat these prefixes with
        // the path. Registered before the string-data flush below; the IO
        // runtime also needs the error wrappers and concat.
        std::string ioOpenL, ioWriteL;
        if (needIo_) {
            needErr_ = true;
            needConcat_ = true;
            needOob_ = true; // $simple_arg bounds-checks via $simple_oob
            ioOpenL = strLabel("cannot open ");
            ioWriteL = strLabel("cannot write ");
        }
        if (needFmtInt_)
            mod << "data $fmt_int = { b \"%lld\", b 10, b 0 }\n";
        if (needFmtFlt_)
            mod << "data $fmt_flt = { b \"%g\", b 10, b 0 }\n";
        if (needBoolStrs_) {
            mod << "data $str_true = { b \"true\", b 0 }\n";
            mod << "data $str_false = { b \"false\", b 0 }\n";
        }
        for (auto& d : strData_) mod << d << "\n";
        mod << "\n" << funcs.str();
        if (needStrMove_) { needStrCopy_ = true; needRC_ = true; } // strmove uses both
        if (needErr_) { needStrCopy_ = true; needRC_ = true; } // err_copy/msg use strcopy
        if (needMap_) { needStrCopy_ = true; needRC_ = true; } // str keys: retain/release/copy
        for (auto& h : rcFuncs_) mod << h;
        if (needStrCopy_) {
            // immortal strings pass through unshared-copy-free: they are
            // read-only forever, so sharing them across threads is safe
            mod << "function l $simple_strcopy(l %p) {\n@start\n"
                   "\t%h =l sub %p, 16\n"
                   "\t%c =l loadl %h\n"
                   "\t%neg =w csltl %c, 0\n"
                   "\tjnz %neg, @imm, @copy\n@imm\n"
                   "\tret %p\n@copy\n"
                   "\t%lp =l sub %p, 8\n"
                   "\t%n =l loadl %lp\n"
                   "\t%sz =l add %n, 17\n"
                   "\t%nh =l call $malloc(l %sz)\n"
                   "\tstorel 1, %nh\n"
                   "\t%nl =l add %nh, 8\n"
                   "\tstorel %n, %nl\n"
                   "\t%np =l add %nh, 16\n"
                   "\t%n1 =l add %n, 1\n"
                   "\tcall $memcpy(l %np, l %p, l %n1)\n"
                   "\tret %np\n}\n";
        }
        if (needStrMove_) {
            // uniquely-owned (or immortal) → transfer the pointer, no copy;
            // shared → copy for the ring, drop our reference
            mod << "function l $simple_strmove(l %p) {\n@start\n"
                   "\t%h =l sub %p, 16\n"
                   "\t%c =l loadl %h\n"
                   "\t%one =w ceql %c, 1\n"
                   "\tjnz %one, @take, @chk\n@chk\n"
                   "\t%neg =w csltl %c, 0\n"
                   "\tjnz %neg, @take, @cp\n@take\n"
                   "\tret %p\n@cp\n"
                   "\t%q =l call $simple_strcopy(l %p)\n"
                   "\tcall $simple_release(l %p)\n"
                   "\tret %q\n}\n";
        }
        if (needChan_) {
            // channel block layout: 0 refcount, 8 elemsize, 16 cap, 24 count,
            // 32 head, 40 tail, 48 dtor, 56 mutex[64], 120 not-full cond[64],
            // 184 not-empty cond[64], 248 ring buffer (cap * elemsize)
            mod << "function l $simple_chan_new(l %es, l %cap, l %dtor) {\n@start\n"
                   "\t%bsz =l mul %cap, %es\n"
                   "\t%tot =l add %bsz, 248\n"
                   "\t%h =l call $malloc(l %tot)\n"
                   "\tstorel 1, %h\n"
                   "\t%a1 =l add %h, 8\n\tstorel %es, %a1\n"
                   "\t%a2 =l add %h, 16\n\tstorel %cap, %a2\n"
                   "\t%a3 =l add %h, 24\n\tstorel 0, %a3\n"
                   "\t%a4 =l add %h, 32\n\tstorel 0, %a4\n"
                   "\t%a5 =l add %h, 40\n\tstorel 0, %a5\n"
                   "\t%a6 =l add %h, 48\n\tstorel %dtor, %a6\n"
                   "\t%m =l add %h, 56\n\tcall $pthread_mutex_init(l %m, l 0)\n"
                   "\t%c1 =l add %h, 120\n\tcall $pthread_cond_init(l %c1, l 0)\n"
                   "\t%c2 =l add %h, 184\n\tcall $pthread_cond_init(l %c2, l 0)\n"
                   "\tret %h\n}\n";
            mod << "function $simple_chan_send(l %ch, l %src) {\n@start\n"
                   "\t%m =l add %ch, 56\n"
                   "\tcall $pthread_mutex_lock(l %m)\n"
                   "\t%pc =l add %ch, 24\n"
                   "\t%pcap =l add %ch, 16\n"
                   "\t%cap =l loadl %pcap\n"
                   "@chk\n"
                   "\t%cnt =l loadl %pc\n"
                   "\t%full =w ceql %cnt, %cap\n"
                   "\tjnz %full, @wait, @put\n"
                   "@wait\n"
                   "\t%nf =l add %ch, 120\n"
                   "\tcall $pthread_cond_wait(l %nf, l %m)\n"
                   "\tjmp @chk\n"
                   "@put\n"
                   "\t%pes =l add %ch, 8\n"
                   "\t%es =l loadl %pes\n"
                   "\t%pt =l add %ch, 40\n"
                   "\t%tail =l loadl %pt\n"
                   "\t%off =l mul %tail, %es\n"
                   "\t%buf =l add %ch, 248\n"
                   "\t%dst =l add %buf, %off\n"
                   "\tcall $memcpy(l %dst, l %src, l %es)\n"
                   "\t%t1 =l add %tail, 1\n"
                   "\t%wr =w ceql %t1, %cap\n"
                   "\tjnz %wr, @wrap, @stt\n"
                   "@wrap\n\tstorel 0, %pt\n\tjmp @bump\n"
                   "@stt\n\tstorel %t1, %pt\n\tjmp @bump\n"
                   "@bump\n"
                   "\t%c1 =l add %cnt, 1\n"
                   "\tstorel %c1, %pc\n"
                   "\t%ne =l add %ch, 184\n"
                   "\tcall $pthread_cond_signal(l %ne)\n"
                   "\tcall $pthread_mutex_unlock(l %m)\n"
                   "\tret\n}\n";
            mod << "function $simple_chan_recv(l %ch, l %dst) {\n@start\n"
                   "\t%m =l add %ch, 56\n"
                   "\tcall $pthread_mutex_lock(l %m)\n"
                   "\t%pc =l add %ch, 24\n"
                   "@chk\n"
                   "\t%cnt =l loadl %pc\n"
                   "\t%empty =w ceql %cnt, 0\n"
                   "\tjnz %empty, @wait, @take\n"
                   "@wait\n"
                   "\t%ne =l add %ch, 184\n"
                   "\tcall $pthread_cond_wait(l %ne, l %m)\n"
                   "\tjmp @chk\n"
                   "@take\n"
                   "\t%pes =l add %ch, 8\n"
                   "\t%es =l loadl %pes\n"
                   "\t%ph =l add %ch, 32\n"
                   "\t%head =l loadl %ph\n"
                   "\t%off =l mul %head, %es\n"
                   "\t%buf =l add %ch, 248\n"
                   "\t%src =l add %buf, %off\n"
                   "\tcall $memcpy(l %dst, l %src, l %es)\n"
                   "\t%pcap =l add %ch, 16\n"
                   "\t%cap =l loadl %pcap\n"
                   "\t%h1 =l add %head, 1\n"
                   "\t%wr =w ceql %h1, %cap\n"
                   "\tjnz %wr, @wrap, @sth\n"
                   "@wrap\n\tstorel 0, %ph\n\tjmp @bump\n"
                   "@sth\n\tstorel %h1, %ph\n\tjmp @bump\n"
                   "@bump\n"
                   "\t%c1 =l sub %cnt, 1\n"
                   "\tstorel %c1, %pc\n"
                   "\t%nf =l add %ch, 120\n"
                   "\tcall $pthread_cond_signal(l %nf)\n"
                   "\tcall $pthread_mutex_unlock(l %m)\n"
                   "\tret\n}\n";
            mod << "function $simple_chan_retain(l %ch) {\n@start\n"
                   "\t%m =l add %ch, 56\n"
                   "\tcall $pthread_mutex_lock(l %m)\n"
                   "\t%c =l loadl %ch\n"
                   "\t%c1 =l add %c, 1\n"
                   "\tstorel %c1, %ch\n"
                   "\tcall $pthread_mutex_unlock(l %m)\n"
                   "\tret\n}\n";
            mod << "function $simple_chan_release(l %ch) {\n@start\n"
                   "\t%m =l add %ch, 56\n"
                   "\tcall $pthread_mutex_lock(l %m)\n"
                   "\t%c =l loadl %ch\n"
                   "\t%c1 =l sub %c, 1\n"
                   "\tstorel %c1, %ch\n"
                   "\t%z =w ceql %c1, 0\n"
                   "\tcall $pthread_mutex_unlock(l %m)\n"
                   "\tjnz %z, @destroy, @done\n"
                   "@destroy\n"
                   "\t%pd =l add %ch, 48\n"
                   "\t%dtor =l loadl %pd\n"
                   "\t%zd =w ceql %dtor, 0\n"
                   "\tjnz %zd, @teardown, @drain\n"
                   "@drain\n" // free items still buffered via the element dtor
                   "\t%pc =l add %ch, 24\n"
                   "\t%cnt =l loadl %pc\n"
                   "\t%ph =l add %ch, 32\n"
                   "\t%head =l loadl %ph\n"
                   "\t%pcap =l add %ch, 16\n"
                   "\t%cap =l loadl %pcap\n"
                   "\t%pes =l add %ch, 8\n"
                   "\t%es =l loadl %pes\n"
                   "\t%buf =l add %ch, 248\n"
                   "\t%ip =l alloc8 8\n"
                   "\tstorel 0, %ip\n"
                   "@dloop\n"
                   "\t%i =l loadl %ip\n"
                   "\t%more =w csltl %i, %cnt\n"
                   "\tjnz %more, @dbody, @teardown\n"
                   "@dbody\n"
                   "\t%hi =l add %head, %i\n"
                   "\t%idx =l rem %hi, %cap\n"
                   "\t%ioff =l mul %idx, %es\n"
                   "\t%iaddr =l add %buf, %ioff\n"
                   "\tcall %dtor(l %iaddr)\n"
                   "\t%i1 =l add %i, 1\n"
                   "\tstorel %i1, %ip\n"
                   "\tjmp @dloop\n"
                   "@teardown\n"
                   "\t%m2 =l add %ch, 56\n"
                   "\tcall $pthread_mutex_destroy(l %m2)\n"
                   "\t%cc1 =l add %ch, 120\n"
                   "\tcall $pthread_cond_destroy(l %cc1)\n"
                   "\t%cc2 =l add %ch, 184\n"
                   "\tcall $pthread_cond_destroy(l %cc2)\n"
                   "\tcall $free(l %ch)\n"
                   "\tjmp @done\n"
                   "@done\n\tret\n}\n";
        }
        if (needConcat_) {
            mod << "function l $simple_concat(l %a, l %b) {\n@start\n"
                   "\t%lap =l sub %a, 8\n"
                   "\t%la =l loadl %lap\n"
                   "\t%lbp =l sub %b, 8\n"
                   "\t%lb =l loadl %lbp\n"
                   "\t%n =l add %la, %lb\n"
                   "\t%sz =l add %n, 17\n"
                   "\t%h =l call $malloc(l %sz)\n"
                   "\tstorel 1, %h\n"
                   "\t%hl =l add %h, 8\n"
                   "\tstorel %n, %hl\n"
                   "\t%p =l add %h, 16\n"
                   "\tcall $memcpy(l %p, l %a, l %la)\n"
                   "\t%q =l add %p, %la\n"
                   "\t%lb1 =l add %lb, 1\n"
                   "\tcall $memcpy(l %q, l %b, l %lb1)\n"
                   "\tret %p\n}\n";
        }
        if (needStrEq_) {
            // content equality over the *length-prefixed* bytes. strcmp would
            // stop at an embedded NUL (a str may hold any byte via '\\0',
            // s[i], substr); this checks length first, then memcmp the whole
            // span — the same logic the map uses for str keys.
            mod << "function w $simple_streq(l %a, l %b) {\n@start\n"
                   "\t%lap =l sub %a, 8\n\t%la =l loadl %lap\n"
                   "\t%lbp =l sub %b, 8\n\t%lb =l loadl %lbp\n"
                   "\t%le =w ceql %la, %lb\n"
                   "\tjnz %le, @cmp, @no\n@cmp\n"
                   "\t%r =w call $memcmp(l %a, l %b, l %la)\n"
                   "\t%z =w ceqw %r, 0\n\tret %z\n@no\n\tret 0\n}\n";
        }
        if (needSubstr_) {
            // substr(s, a, b): a new string of bytes [a, b), each bound
            // clamped into range so out-of-range arguments never crash
            mod << "function l $simple_substr(l %s, l %a0, l %b0) {\n@start\n"
                   "\t%lp =l sub %s, 8\n\t%len =l loadl %lp\n"
                   "\t%ap =l alloc8 8\n\t%bp =l alloc8 8\n"
                   "\tstorel %a0, %ap\n\tstorel %b0, %bp\n"
                   "\t%a1 =l loadl %ap\n\t%az =w csltl %a1, 0\n"
                   "\tjnz %az, @az, @ah\n"
                   "@az\n\tstorel 0, %ap\n\tjmp @ah\n"
                   "@ah\n\t%a2 =l loadl %ap\n\t%ag =w csgtl %a2, %len\n"
                   "\tjnz %ag, @ac, @bh\n"
                   "@ac\n\tstorel %len, %ap\n\tjmp @bh\n"
                   "@bh\n\t%b1 =l loadl %bp\n\t%bg =w csgtl %b1, %len\n"
                   "\tjnz %bg, @bc, @bl\n"
                   "@bc\n\tstorel %len, %bp\n\tjmp @bl\n"
                   "@bl\n\t%a3 =l loadl %ap\n\t%b2 =l loadl %bp\n"
                   "\t%blt =w csltl %b2, %a3\n\tjnz %blt, @bs, @mk\n"
                   "@bs\n\tstorel %a3, %bp\n\tjmp @mk\n"
                   "@mk\n\t%af =l loadl %ap\n\t%bf =l loadl %bp\n"
                   "\t%n =l sub %bf, %af\n\t%sz =l add %n, 17\n"
                   "\t%h =l call $malloc(l %sz)\n\tstorel 1, %h\n"
                   "\t%hl =l add %h, 8\n\tstorel %n, %hl\n"
                   "\t%p =l add %h, 16\n\t%src =l add %s, %af\n"
                   "\tcall $memcpy(l %p, l %src, l %n)\n"
                   "\t%z =l add %p, %n\n\tstoreb 0, %z\n"
                   "\tret %p\n}\n";
        }
        if (needIntToStr_) {
            // format a signed 64-bit integer as decimal into a fresh string
            mod << "data $i2s_neg = { b \"-\", b 0 }\n"
                   "function l $simple_int_to_str(l %n) {\n@start\n"
                   "\t%buf =l alloc4 24\n"
                   "\t%endp =l add %buf, 23\n\tstoreb 0, %endp\n"
                   "\t%pp =l alloc8 8\n\tstorel %endp, %pp\n"
                   "\t%neg =w csltl %n, 0\n"
                   "\t%vp =l alloc8 8\n"
                   "\tjnz %neg, @isneg, @ispos\n"
                   "@isneg\n\t%nn =l sub 0, %n\n\tstorel %nn, %vp\n\tjmp @loop\n"
                   "@ispos\n\tstorel %n, %vp\n\tjmp @loop\n"
                   "@loop\n\t%v =l loadl %vp\n\t%d =l rem %v, 10\n"
                   "\t%ch =l add %d, 48\n"
                   "\t%cur =l loadl %pp\n\t%cur1 =l sub %cur, 1\n"
                   "\tstoreb %ch, %cur1\n\tstorel %cur1, %pp\n"
                   "\t%v2 =l div %v, 10\n\tstorel %v2, %vp\n"
                   "\t%more =w cnel %v2, 0\n\tjnz %more, @loop, @sign\n"
                   "@sign\n\tjnz %neg, @putneg, @done\n"
                   "@putneg\n\t%c2 =l loadl %pp\n\t%c3 =l sub %c2, 1\n"
                   "\tstoreb 45, %c3\n\tstorel %c3, %pp\n\tjmp @done\n"
                   "@done\n\t%start =l loadl %pp\n"
                   "\t%lenb =l sub %endp, %start\n"
                   "\t%sz =l add %lenb, 17\n"
                   "\t%h =l call $malloc(l %sz)\n\tstorel 1, %h\n"
                   "\t%hl =l add %h, 8\n\tstorel %lenb, %hl\n"
                   "\t%p =l add %h, 16\n"
                   "\t%cpy =l add %lenb, 1\n"
                   "\tcall $memcpy(l %p, l %start, l %cpy)\n"
                   "\tret %p\n}\n";
        }
        if (needStrToInt_) {
            // parse an optional sign then leading digits; stop at the first
            // non-digit. Non-numeric input yields 0.
            mod << "function l $simple_str_to_int(l %s) {\n@start\n"
                   "\t%ip =l alloc8 8\n\tstorel 0, %ip\n"
                   "\t%accp =l alloc8 8\n\tstorel 0, %accp\n"
                   "\t%signp =l alloc8 8\n\tstorel 1, %signp\n"
                   "\t%c0 =l loadub %s\n\t%isneg =w ceql %c0, 45\n"
                   "\t%ispos =w ceql %c0, 43\n\t%sgn =w or %isneg, %ispos\n"
                   "\tjnz %sgn, @skip, @loop\n"
                   "@skip\n\tstorel 1, %ip\n"
                   "\tjnz %isneg, @setneg, @loop\n"
                   "@setneg\n\tstorel -1, %signp\n\tjmp @loop\n"
                   "@loop\n\t%i =l loadl %ip\n\t%cp =l add %s, %i\n"
                   "\t%c =l loadub %cp\n"
                   "\t%ge =w csgel %c, 48\n\t%le =w cslel %c, 57\n"
                   "\t%dig =w and %ge, %le\n\tjnz %dig, @acc, @fin\n"
                   "@acc\n\t%a =l loadl %accp\n\t%a10 =l mul %a, 10\n"
                   "\t%dv =l sub %c, 48\n\t%a2 =l add %a10, %dv\n"
                   "\tstorel %a2, %accp\n"
                   "\t%i1 =l add %i, 1\n\tstorel %i1, %ip\n\tjmp @loop\n"
                   "@fin\n\t%acc =l loadl %accp\n\t%sg =l loadl %signp\n"
                   "\t%r =l mul %acc, %sg\n\tret %r\n}\n";
        }
        if (needRC_) {
            mod << "function $simple_retain(l %p) {\n@start\n"
                   "\t%h =l sub %p, 16\n"
                   "\t%c =l loadl %h\n"
                   "\t%neg =w csltl %c, 0\n"
                   "\tjnz %neg, @done, @inc\n@inc\n"
                   "\t%c1 =l add %c, 1\n"
                   "\tstorel %c1, %h\n"
                   "\tjmp @done\n@done\n\tret\n}\n";
            mod << "function $simple_release(l %p) {\n@start\n"
                   "\t%h =l sub %p, 16\n"
                   "\t%c =l loadl %h\n"
                   "\t%neg =w csltl %c, 0\n"
                   "\tjnz %neg, @done, @dec\n@dec\n"
                   "\t%c1 =l sub %c, 1\n"
                   "\t%z =w ceql %c1, 0\n"
                   "\tjnz %z, @fre, @st\n@fre\n"
                   "\tcall $free(l %h)\n"
                   "\tjmp @done\n@st\n"
                   "\tstorel %c1, %h\n"
                   "\tjmp @done\n@done\n\tret\n}\n";
        }
        if (needErr_) {
            // an `error` is a nullable string (null == ok). These wrappers add
            // the null guard so ok values are safe to retain/release/read.
            mod << "data $simple_str_empty = { l -1, l 0, b 0 }\n"
                   "function $simple_err_retain(l %e) {\n@start\n"
                   "\tjnz %e, @do, @skip\n@do\n"
                   "\tcall $simple_retain(l %e)\n\tret\n@skip\n\tret\n}\n"
                   "function $simple_err_release(l %e) {\n@start\n"
                   "\tjnz %e, @do, @skip\n@do\n"
                   "\tcall $simple_release(l %e)\n\tret\n@skip\n\tret\n}\n"
                   "function l $simple_err_copy(l %e) {\n@start\n"
                   "\tjnz %e, @do, @skip\n@do\n"
                   "\t%c =l call $simple_strcopy(l %e)\n\tret %c\n@skip\n\tret 0\n}\n"
                   "function l $simple_err_msg(l %e) {\n@start\n"
                   "\tjnz %e, @do, @skip\n@do\n"
                   "\t%c =l call $simple_strcopy(l %e)\n\tret %c\n@skip\n"
                   "\t%z =l add $simple_str_empty, 16\n\tret %z\n}\n";
        }
        if (needIo_) {
            mod << "data $io_rb = { b \"rb\", b 0 }\n"
                   "data $io_wb = { b \"wb\", b 0 }\n";
            // arg(i): bounds-checked argv[i], wrapped as a fresh rc string
            mod << "function l $simple_arg(l %i) {\n@start\n"
                   "\t%n =l loadl $simple_argc\n"
                   "\t%lt =w csltl %i, 0\n"
                   "\t%ge =w csgel %i, %n\n"
                   "\t%bad =w or %lt, %ge\n"
                   "\tjnz %bad, @oob, @ok\n@oob\n"
                   "\tcall $simple_oob(l %i, l %n)\n\thlt\n@ok\n"
                   "\t%av =l loadl $simple_argv\n"
                   "\t%off =l mul %i, 8\n"
                   "\t%pp =l add %av, %off\n"
                   "\t%cs =l loadl %pp\n"
                   "\t%len =l call $strlen(l %cs)\n"
                   "\t%sz =l add %len, 17\n"
                   "\t%h =l call $malloc(l %sz)\n"
                   "\tstorel 1, %h\n"
                   "\t%hl =l add %h, 8\n\tstorel %len, %hl\n"
                   "\t%p =l add %h, 16\n"
                   "\t%l1 =l add %len, 1\n"
                   "\tcall $memcpy(l %p, l %cs, l %l1)\n"
                   "\tret %p\n}\n";
            // input() / read_all(): getchar loop (no stdin symbol — portable),
            // growable buffer, result is a fresh rc string. %stopnl selects
            // stop-at-newline (input) vs read-to-EOF (read_all); EOF -> "".
            mod << "function l $simple_read_stream(l %stopnl) {\n@start\n"
                   "\t%bufs =l alloc8 8\n\t%caps =l alloc8 8\n\t%lens =l alloc8 8\n"
                   "\t%b0 =l call $malloc(l 64)\n"
                   "\tstorel %b0, %bufs\n\tstorel 64, %caps\n\tstorel 0, %lens\n"
                   "@loop\n"
                   "\t%c =w call $getchar()\n"
                   "\t%eof =w csltw %c, 0\n"
                   "\tjnz %eof, @done, @chknl\n@chknl\n"
                   "\t%isnl =w ceqw %c, 10\n"
                   "\t%sn =w cnel %stopnl, 0\n"
                   "\t%both =w and %sn, %isnl\n"
                   "\tjnz %both, @done, @append\n@append\n"
                   "\t%len =l loadl %lens\n"
                   "\t%cap =l loadl %caps\n"
                   "\t%full =w ceql %len, %cap\n"
                   "\tjnz %full, @grow, @put\n@grow\n"
                   "\t%ncap =l mul %cap, 2\n"
                   "\t%ob =l loadl %bufs\n"
                   "\t%nb =l call $realloc(l %ob, l %ncap)\n"
                   "\tstorel %nb, %bufs\n\tstorel %ncap, %caps\n"
                   "\tjmp @put\n@put\n"
                   "\t%len2 =l loadl %lens\n"
                   "\t%buf =l loadl %bufs\n"
                   "\t%dst =l add %buf, %len2\n"
                   "\tstoreb %c, %dst\n"
                   "\t%len3 =l add %len2, 1\n"
                   "\tstorel %len3, %lens\n"
                   "\tjmp @loop\n@done\n"
                   "\t%flen =l loadl %lens\n"
                   "\t%fbuf =l loadl %bufs\n"
                   "\t%hsz =l add %flen, 17\n"
                   "\t%h =l call $malloc(l %hsz)\n"
                   "\tstorel 1, %h\n"
                   "\t%hl =l add %h, 8\n\tstorel %flen, %hl\n"
                   "\t%p =l add %h, 16\n"
                   "\tcall $memcpy(l %p, l %fbuf, l %flen)\n"
                   "\t%z =l add %p, %flen\n\tstoreb 0, %z\n"
                   "\tcall $free(l %fbuf)\n"
                   "\tret %p\n}\n";
            // read_file: fills a (str, error) buffer — str at +0, error at +8.
            // Failure: ("", "cannot open " + path). Mirrors the multi-return ABI.
            mod << "function $simple_read_file(l %out, l %path) {\n@start\n"
                   "\t%o8 =l add %out, 8\n"
                   "\t%f =l call $fopen(l %path, l $io_rb)\n"
                   "\tjnz %f, @ok, @fail\n@fail\n"
                   "\t%pre =l add " << ioOpenL << ", 16\n"
                   "\t%msg =l call $simple_concat(l %pre, l %path)\n"
                   "\t%es =l add $simple_str_empty, 16\n"
                   "\tstorel %es, %out\n"
                   "\tstorel %msg, %o8\n"
                   "\tret\n@ok\n"
                   "\tcall $fseek(l %f, l 0, w 2)\n"       // SEEK_END
                   "\t%sz =l call $ftell(l %f)\n"
                   "\tcall $fseek(l %f, l 0, w 0)\n"       // SEEK_SET
                   "\t%hsz =l add %sz, 17\n"
                   "\t%h =l call $malloc(l %hsz)\n"
                   "\tstorel 1, %h\n"
                   "\t%hl =l add %h, 8\n\tstorel %sz, %hl\n"
                   "\t%p =l add %h, 16\n"
                   "\tcall $fread(l %p, l 1, l %sz, l %f)\n"
                   "\tcall $fclose(l %f)\n"
                   "\t%z =l add %p, %sz\n\tstoreb 0, %z\n"
                   "\tstorel %p, %out\n"
                   "\tstorel 0, %o8\n"
                   "\tret\n}\n";
            // write_file: returns an error (0 = ok)
            mod << "function l $simple_write_file(l %path, l %data) {\n@start\n"
                   "\t%f =l call $fopen(l %path, l $io_wb)\n"
                   "\tjnz %f, @ok, @fail\n@fail\n"
                   "\t%pre =l add " << ioWriteL << ", 16\n"
                   "\t%msg =l call $simple_concat(l %pre, l %path)\n"
                   "\tret %msg\n@ok\n"
                   "\t%lp =l sub %data, 8\n"
                   "\t%n =l loadl %lp\n"
                   "\t%wr =l call $fwrite(l %data, l 1, l %n, l %f)\n"
                   "\tcall $fclose(l %f)\n"
                   "\t%bad =w cnel %wr, %n\n"
                   "\tjnz %bad, @werr, @done\n@werr\n"
                   "\t%pre2 =l add " << ioWriteL << ", 16\n"
                   "\t%msg2 =l call $simple_concat(l %pre2, l %path)\n"
                   "\tret %msg2\n@done\n"
                   "\tret 0\n}\n";
        }
        if (needOob_) {
            mod << "data $oob_msg = { b \"runtime error: index %lld out of bounds "
                   "(length %lld)\", b 10, b 0 }\n"
                   "function $simple_oob(l %i, l %n) {\n@start\n"
                   "\tcall $printf(l $oob_msg, ..., l %i, l %n)\n"
                   "\tcall $exit(w 1)\n"
                   "\thlt\n}\n";
        }
        if (needList_) mod << listRuntime();
        if (needMap_) mod << mapRuntime();
        return mod.str();
    }

private:
    std::unordered_map<std::string, StructDecl*> structs_;
    std::unordered_map<std::string, Function*> fns_;
    std::unordered_map<std::string, Layout> layouts_;
    std::map<std::string, std::string> strPool_;
    std::vector<std::string> strData_;
    std::set<std::string> rcEmitted_;
    std::vector<std::string> rcFuncs_;
    bool needFmtInt_ = false, needFmtFlt_ = false, needBoolStrs_ = false;
    bool needConcat_ = false, needOob_ = false, needRC_ = false, needStrEq_ = false;
    bool needChan_ = false, needStrCopy_ = false, needStrMove_ = false;
    bool needErr_ = false, needIo_ = false, needMap_ = false;
    bool needList_ = false;
    bool needSubstr_ = false, needIntToStr_ = false, needStrToInt_ = false;

    // the module under construction
    std::vector<MFunc> mfuncs_;
    bool optimize_ = true;

    // per-function state
    Function* cur_ = nullptr;
    std::vector<MBlock> blocks_;
    std::vector<std::string> allocs_;
    std::vector<std::unordered_map<std::string, Slot>> scopes_;
    std::vector<LoopCtx> loops_;
    std::vector<std::pair<std::string, Type>> stmtTemps_; // owned values awaiting consumption
    std::string retOutSlot_;
    std::string destHint_; // in-place build target for the next aggregate literal
    int tmpN_ = 0, lblN_ = 0, slotN_ = 0;
    bool terminated_ = false;
    long vecLaneOff_ = 0; // element offset added to vector array reads (unroll)

    // The list runtime, emitted as QBE. A list handle points at a header
    //   [0] refcount  [8] len  [16] cap  [24] elemsize  [32] data ptr
    // Value semantics via refcount + copy-on-write: `let b = a` shares the
    // block (retain); the first mutation of a shared block copies it. Data
    // lives in a separate malloc'd buffer so growth reallocs the data, not
    // the header, keeping the handle stable except when COW makes a new one.
    static std::string listRuntime() {
        std::string s;
        // new(elemsize) -> empty list
        s += "function l $simple_list_new(l %es) {\n@start\n"
             "\t%b =l call $malloc(l 40)\n"
             "\tstorel 1, %b\n"
             "\t%p1 =l add %b, 8\n\tstorel 0, %p1\n"
             "\t%p2 =l add %b, 16\n\tstorel 0, %p2\n"
             "\t%p3 =l add %b, 24\n\tstorel %es, %p3\n"
             "\t%p4 =l add %b, 32\n\tstorel 0, %p4\n"
             "\tret %b\n}\n";
        // deep copy (retainfn per element, 0 if none)
        s += "function l $simple_list_copy(l %b, l %rf) {\n@start\n"
             "\t%lp =l add %b, 8\n\t%len =l loadl %lp\n"
             "\t%ep =l add %b, 24\n\t%es =l loadl %ep\n"
             "\t%nb =l call $malloc(l 40)\n"
             "\tstorel 1, %nb\n"
             "\t%q1 =l add %nb, 8\n\tstorel %len, %q1\n"
             "\t%q2 =l add %nb, 16\n\tstorel %len, %q2\n"
             "\t%q3 =l add %nb, 24\n\tstorel %es, %q3\n"
             "\t%bytes =l mul %len, %es\n"
             "\t%sz =l add %bytes, 1\n"           // avoid malloc(0)
             "\t%nd =l call $malloc(l %sz)\n"
             "\t%q4 =l add %nb, 32\n\tstorel %nd, %q4\n"
             "\t%odp =l add %b, 32\n\t%od =l loadl %odp\n"
             "\tcall $memcpy(l %nd, l %od, l %bytes)\n"
             "\t%rz =w ceql %rf, 0\n"
             "\tjnz %rz, @done, @rl\n"
             "@rl\n\t%ip =l alloc8 8\n\tstorel 0, %ip\n"
             "@rloop\n\t%i =l loadl %ip\n\t%mo =w csltl %i, %len\n"
             "\tjnz %mo, @rb, @done\n"
             "@rb\n\t%ro =l mul %i, %es\n\t%ra =l add %nd, %ro\n"
             "\tcall %rf(l %ra)\n"
             "\t%i1 =l add %i, 1\n\tstorel %i1, %ip\n\tjmp @rloop\n"
             "@done\n\tret %nb\n}\n";
        // retain / release the whole list handle
        s += "function $simple_list_retain(l %b) {\n@start\n"
             "\t%c =l loadl %b\n\t%c1 =l add %c, 1\n\tstorel %c1, %b\n\tret\n}\n";
        s += "function $simple_list_release(l %b, l %df) {\n@start\n"
             "\t%z =w ceql %b, 0\n\tjnz %z, @out, @go\n"
             "@go\n\t%c =l loadl %b\n\t%c1 =l sub %c, 1\n\tstorel %c1, %b\n"
             "\t%dead =w ceql %c1, 0\n\tjnz %dead, @free, @out\n"
             "@free\n"
             "\t%dz =w ceql %df, 0\n\tjnz %dz, @fd, @dl\n"
             "@dl\n\t%lp =l add %b, 8\n\t%len =l loadl %lp\n"
             "\t%ep =l add %b, 24\n\t%es =l loadl %ep\n"
             "\t%dp =l add %b, 32\n\t%d =l loadl %dp\n"
             "\t%ip =l alloc8 8\n\tstorel 0, %ip\n"
             "@dloop\n\t%i =l loadl %ip\n\t%mo =w csltl %i, %len\n"
             "\tjnz %mo, @db, @fd\n"
             "@db\n\t%o =l mul %i, %es\n\t%a =l add %d, %o\n"
             "\tcall %df(l %a)\n"
             "\t%i1 =l add %i, 1\n\tstorel %i1, %ip\n\tjmp @dloop\n"
             "@fd\n\t%dp2 =l add %b, 32\n\t%d2 =l loadl %dp2\n"
             "\tcall $free(l %d2)\n\tcall $free(l %b)\n\tjmp @out\n"
             "@out\n\tret\n}\n";
        // ensure the block is uniquely owned (COW), returning the handle to use
        s += "function l $simple_list_unique(l %b, l %rf, l %df) {\n@start\n"
             "\t%c =l loadl %b\n\t%sh =w csgtl %c, 1\n"
             "\tjnz %sh, @cow, @keep\n"
             "@cow\n\t%nb =l call $simple_list_copy(l %b, l %rf)\n"
             "\tcall $simple_list_release(l %b, l %df)\n\tret %nb\n"
             "@keep\n\tret %b\n}\n";
        // grow to hold one more element if needed; data may move (realloc)
        s += "function $simple_list_reserve(l %b) {\n@start\n"
             "\t%lp =l add %b, 8\n\t%len =l loadl %lp\n"
             "\t%cp =l add %b, 16\n\t%cap =l loadl %cp\n"
             "\t%full =w ceql %len, %cap\n\tjnz %full, @grow, @out\n"
             "@grow\n\t%cz =w ceql %cap, 0\n\t%czl =l extuw %cz\n"
             "\t%ext =l mul %czl, 4\n\t%dbl =l mul %cap, 2\n\t%nc =l add %dbl, %ext\n"
             "\t%ep =l add %b, 24\n\t%es =l loadl %ep\n"
             "\t%bytes =l mul %nc, %es\n"
             "\t%dp =l add %b, 32\n\t%od =l loadl %dp\n"
             "\t%nd =l call $realloc(l %od, l %bytes)\n"
             "\tstorel %nd, %dp\n\tstorel %nc, %cp\n\tjmp @out\n"
             "@out\n\tret\n}\n";
        return s;
    }

    // ---- sizes & layouts ----

    long sizeOf(const Type& t) {
        switch (t.kind) {
        case TypeKind::Int: case TypeKind::Float: return t.bits / 8;
        case TypeKind::Bool: return 1;
        case TypeKind::Str: case TypeKind::Chan: case TypeKind::Ptr:
        case TypeKind::List: case TypeKind::Map: return 8;
        case TypeKind::Struct: return layout(t.sname).size;
        case TypeKind::Array: return (long)t.alen * sizeOf(*t.elem);
        default: return 8;
        }
    }
    // natural alignment: a value is aligned to its own size (structs to
    // their widest member), so hardware layouts come out as expected
    long alignOf(const Type& t) {
        switch (t.kind) {
        case TypeKind::Struct: return layout(t.sname).align;
        case TypeKind::Array: return alignOf(*t.elem);
        default: return sizeOf(t);
        }
    }

    // A multi-return function writes its values into one caller-provided
    // buffer, packed like struct fields (aligned, in declaration order).
    static long alignUp(long v, long a) { return (v + a - 1) / a * a; }
    long multiOffset(const std::vector<Type>& rets, size_t i) {
        long off = 0;
        for (size_t k = 0;; k++) {
            off = alignUp(off, alignOf(rets[k]));
            if (k == i) return off;
            off += sizeOf(rets[k]);
        }
    }
    long multiSize(const std::vector<Type>& rets) {
        size_t last = rets.size() - 1;
        return alignUp(multiOffset(rets, last) + sizeOf(rets[last]), 8);
    }
    long multiOffset(const Function& f, size_t i) { return multiOffset(f.rets, i); }
    long multiSize(const Function& f) { return multiSize(f.rets); }

    // Multi-return BUILTINS (v0.85 IO): their result types, or empty.
    static std::vector<Type> builtinRets(const std::string& name) {
        if (name == "read_file")
            return {Type{TypeKind::Str}, Type{TypeKind::Error}};
        return {};
    }
    static long roundUp(long v, long a) { return (v + a - 1) / a * a; }

    const Layout& layout(const std::string& name) {
        auto it = layouts_.find(name);
        if (it != layouts_.end()) return it->second;
        // reserve first so a self-reference can't recurse forever (sema
        // already rejected those, this just keeps the map stable)
        Layout& lay = layouts_[name];
        long off = 0, maxAlign = 1;
        for (auto& f : structs_[name]->fields) {
            long a = alignOf(f.type);
            if (a > maxAlign) maxAlign = a;
            off = roundUp(off, a);
            lay.fields[f.name] = {off, f.type};
            off += sizeOf(f.type);
        }
        lay.align = maxAlign;
        lay.size = roundUp(off, maxAlign);
        return lay;
    }

    bool typeHasRc(const Type& t) {
        if (t.kind == TypeKind::Str || t.kind == TypeKind::Chan ||
            t.kind == TypeKind::List || t.kind == TypeKind::Error ||
            t.kind == TypeKind::Map) return true;
        if (t.kind == TypeKind::Array) return typeHasRc(*t.elem);
        if (t.kind == TypeKind::Struct) {
            for (auto& f : structs_[t.sname]->fields)
                if (typeHasRc(f.type)) return true;
        }
        return false;
    }

    static std::string mangle(const Type& t) {
        switch (t.kind) {
        case TypeKind::Int: return "i";
        case TypeKind::Bool: return "b";
        case TypeKind::Str: return "s";
        case TypeKind::Error: return "e";
        case TypeKind::Struct: return "S" + t.sname;
        case TypeKind::Array: return "A" + std::to_string(t.alen) + "_" + mangle(*t.elem);
        case TypeKind::Chan: return "C" + mangle(*t.elem);
        case TypeKind::List: return "L" + mangle(*t.elem);
        case TypeKind::Map: return "M" + mangle(*t.key) + "_" + mangle(*t.elem);
        default: return "v";
        }
    }

    // Generates (once) a helper operating on every rc value (string/channel)
    // reachable in storage of type t, and returns its name.
    //   mode 'r' = retain all;  'd' = drop (release) all;
    //   mode 'c' = deep-copy in place, for send/spawn: replaces each string
    //              field with a private copy, retains each channel handle.
    // Also accepts scalar Str/Chan types (a "slot" helper: load, then op) —
    // used as the element destructor a channel runs on still-buffered items.
    std::string rcHelper(const Type& t, char mode) {
        std::string name = std::string("$rc_") +
                           (mode == 'r' ? "ret_" : mode == 'd' ? "rel_" : "cpy_") + mangle(t);
        if (rcEmitted_.count(name)) return name;
        rcEmitted_.insert(name);
        needRC_ = true;
        if (mode == 'c') needStrCopy_ = true;
        std::ostringstream f;
        int n = 0;
        auto op = [&](const std::string& addr, const Type& ft) {
            std::string v = "%h" + std::to_string(++n);
            f << "\t" << v << " =l loadl " << addr << "\n";
            if (ft.kind == TypeKind::Str) {
                if (mode == 'r') f << "\tcall $simple_retain(l " << v << ")\n";
                else if (mode == 'd') f << "\tcall $simple_release(l " << v << ")\n";
                else {
                    std::string c = "%h" + std::to_string(++n);
                    f << "\t" << c << " =l call $simple_strcopy(l " << v << ")\n";
                    f << "\tstorel " << c << ", " << addr << "\n";
                }
            } else if (ft.kind == TypeKind::Error) { // null-safe (null == ok)
                needErr_ = true;
                if (mode == 'r') f << "\tcall $simple_err_retain(l " << v << ")\n";
                else if (mode == 'd') f << "\tcall $simple_err_release(l " << v << ")\n";
                else {
                    std::string c = "%h" + std::to_string(++n);
                    f << "\t" << c << " =l call $simple_err_copy(l " << v << ")\n";
                    f << "\tstorel " << c << ", " << addr << "\n";
                }
            } else if (ft.kind == TypeKind::Map) {
                needMap_ = true;
                if (mode == 'r') f << "\tcall $simple_map_retain(l " << v << ")\n";
                else if (mode == 'd') {
                    std::string dt = typeHasRc(*ft.elem) ? mapValDtor(ft) : "0";
                    f << "\tcall $simple_map_release(l " << v << ", l " << dt << ")\n";
                } else {
                    std::string sc = typeHasRc(*ft.elem) ? mapValSendCopy(ft) : "0";
                    std::string c = "%h" + std::to_string(++n);
                    f << "\t" << c << " =l call $simple_map_copy(l " << v << ", l "
                      << sc << ")\n";
                    f << "\tstorel " << c << ", " << addr << "\n";
                }
            } else { // Chan: copy of a handle = share it, so 'c' retains too
                needChan_ = true;
                f << "\tcall $simple_chan_" << (mode == 'd' ? "release" : "retain")
                  << "(l " << v << ")\n";
            }
        };
        f << "function " << name << "(l %p) {\n@start\n";
        if (t.kind == TypeKind::Str || t.kind == TypeKind::Chan ||
            t.kind == TypeKind::Error || t.kind == TypeKind::Map) {
            op("%p", t);
            f << "\tret\n}\n";
        } else if (t.kind == TypeKind::Struct) {
            const Layout& lay = layout(t.sname);
            for (auto& fld : structs_[t.sname]->fields) {
                if (!typeHasRc(fld.type)) continue;
                long off = lay.fields.at(fld.name).first;
                std::string a = "%p";
                if (off != 0) {
                    a = "%h" + std::to_string(++n);
                    f << "\t" << a << " =l add %p, " << off << "\n";
                }
                if (isRcScalar(fld.type)) op(a, fld.type);
                else f << "\tcall " << rcHelper(fld.type, mode) << "(l " << a << ")\n";
            }
            f << "\tret\n}\n";
        } else { // array
            long esz = sizeOf(*t.elem);
            f << "\t%ip =l alloc8 8\n\tstorel 0, %ip\n@loop\n"
              << "\t%i =l loadl %ip\n"
              << "\t%c =w csltl %i, " << t.alen << "\n"
              << "\tjnz %c, @body, @done\n@body\n"
              << "\t%off =l mul %i, " << esz << "\n"
              << "\t%a =l add %p, %off\n";
            if (isRcScalar(*t.elem)) op("%a", *t.elem);
            else f << "\tcall " << rcHelper(*t.elem, mode) << "(l %a)\n";
            f << "\t%i1 =l add %i, 1\n\tstorel %i1, %ip\n\tjmp @loop\n@done\n\tret\n}\n";
        }
        rcFuncs_.push_back(f.str());
        return name;
    }

    // ==================== map runtime (v0.9) ====================
    // Python-dict layout: an insertion-ordered entries array plus an
    // open-addressed index of entry positions. Iteration walks entries in
    // order — deterministic across runs and architectures, because hashing
    // is pure integer math (FNV-1a for str keys, splitmix64 for int keys)
    // and probing is linear.
    //
    // header: [0]rc [8]count(live) [16]nentries(incl dead) [24]ecap
    //         [32]entries [40]index [48]icap(pow2) [56]stride [64]keyIsStr
    // entry:  [0]state(1 live, 0 dead) [8]key(str ptr or int) [16..]value
    // index:  l slots: entry position, -1 empty, -2 deleted (probe continues)
    static std::string mapRuntime() {
        return
        "data $map_nf_s = { b \"runtime error: key not found: %s\", b 10, b 0 }\n"
        "data $map_nf_i = { b \"runtime error: key not found: %lld\", b 10, b 0 }\n"
        // hash: branch on key kind; both sides are exact integer recipes
        "function l $simple_map_hash(l %m, l %k) {\n@start\n"
        "\t%ip =l alloc8 8\n\t%hp =l alloc8 8\n"
        "\t%kisp =l add %m, 64\n\t%kis =l loadl %kisp\n"
        "\tjnz %kis, @hs, @hi\n@hi\n"
        "\t%x1 =l shr %k, 30\n\t%x2 =l xor %k, %x1\n"
        "\t%x3 =l mul %x2, -4658895280553007687\n"
        "\t%x4 =l shr %x3, 27\n\t%x5 =l xor %x3, %x4\n"
        "\t%x6 =l mul %x5, -7723592293110705173\n"
        "\t%x7 =l shr %x6, 31\n\t%x8 =l xor %x6, %x7\n"
        "\tret %x8\n@hs\n"
        "\t%lp =l sub %k, 8\n\t%len =l loadl %lp\n"
        "\tstorel 0, %ip\n"
        "\tstorel -3750763034362895579, %hp\n"
        "@hloop\n"
        "\t%i =l loadl %ip\n"
        "\t%c =w csltl %i, %len\n"
        "\tjnz %c, @hbody, @hdone\n@hbody\n"
        "\t%bp =l add %k, %i\n\t%b =l loadub %bp\n"
        "\t%h =l loadl %hp\n"
        "\t%hx =l xor %h, %b\n"
        "\t%hm =l mul %hx, 1099511628211\n"
        "\tstorel %hm, %hp\n"
        "\t%i1 =l add %i, 1\n\tstorel %i1, %ip\n"
        "\tjmp @hloop\n@hdone\n"
        "\t%hf =l loadl %hp\n\tret %hf\n}\n"
        // key equality: int compares, str compares length then bytes
        "function w $simple_map_keyeq(l %m, l %a, l %b) {\n@start\n"
        "\t%kisp =l add %m, 64\n\t%kis =l loadl %kisp\n"
        "\tjnz %kis, @ks, @ki\n@ki\n"
        "\t%eq =w ceql %a, %b\n\tret %eq\n@ks\n"
        "\t%lap =l sub %a, 8\n\t%la =l loadl %lap\n"
        "\t%lbp =l sub %b, 8\n\t%lb =l loadl %lbp\n"
        "\t%le =w ceql %la, %lb\n"
        "\tjnz %le, @kcmp, @kno\n@kcmp\n"
        "\t%r =w call $memcmp(l %a, l %b, l %la)\n"
        "\t%z =w ceqw %r, 0\n\tret %z\n@kno\n\tret 0\n}\n"
        "function l $simple_map_new(l %stride, l %kis) {\n@start\n"
        "\t%h =l call $malloc(l 88)\n"
        "\tstorel 1, %h\n"
        "\t%a1 =l add %h, 8\n\tstorel 0, %a1\n"
        "\t%a2 =l add %h, 16\n\tstorel 0, %a2\n"
        "\t%a3 =l add %h, 24\n\tstorel 8, %a3\n"
        "\t%esz =l mul 8, %stride\n"
        "\t%ent =l call $malloc(l %esz)\n"
        "\t%a4 =l add %h, 32\n\tstorel %ent, %a4\n"
        "\t%idx =l call $calloc(l 16, l 4)\n"
        "\t%a5 =l add %h, 40\n\tstorel %idx, %a5\n"
        "\t%a6 =l add %h, 48\n\tstorel 16, %a6\n"
        "\t%a7 =l add %h, 56\n\tstorel %stride, %a7\n"
        "\t%a8 =l add %h, 64\n\tstorel %kis, %a8\n"
        "\t%a9 =l add %h, 72\n\tstorel 0, %a9\n"
        "\t%a10 =l add %h, 80\n\tstorel 0, %a10\n"
        "\tret %h\n}\n"
        // find: entry pointer, or 0 when absent
        "function l $simple_map_find(l %m, l %k) {\n@start\n"
        "\t%sp =l alloc8 8\n"
        "\t%kisp =l add %m, 64\n\t%kis =l loadl %kisp\n"
        "\t%icp =l add %m, 48\n\t%ic =l loadl %icp\n"
        "\t%mask =l sub %ic, 1\n"
        "\t%idxp =l add %m, 40\n\t%idx =l loadl %idxp\n"
        "\t%entp =l add %m, 32\n\t%ent =l loadl %entp\n"
        "\t%stp =l add %m, 56\n\t%st =l loadl %stp\n"
        "\tjnz %kis, @hs, @hi\n"
        // int keys: inline splitmix64, inline compare
        "@hi\n"
        "\t%x1 =l shr %k, 30\n\t%x2 =l xor %k, %x1\n"
        "\t%x3 =l mul %x2, -4658895280553007687\n"
        "\t%x4 =l shr %x3, 27\n\t%x5 =l xor %x3, %x4\n"
        "\t%x6 =l mul %x5, -7723592293110705173\n"
        "\t%x7 =l shr %x6, 31\n\t%hi2 =l xor %x6, %x7\n"
        "\t%si0 =l and %hi2, %mask\n\tstorel %si0, %sp\n"
        "@iloop\n"
        "\t%is =l loadl %sp\n"
        "\t%ioff =l mul %is, 4\n\t%iivp =l add %idx, %ioff\n\t%iiv0 =l loadsw %iivp\n"
        "\t%iemp =w ceql %iiv0, 0\n"
        "\tjnz %iemp, @inone, @ichkd\n@ichkd\n"
        "\t%idum =w ceql %iiv0, 1\n"
        "\tjnz %idum, @inext, @ichk\n@ichk\n"
        "\t%iiv =l sub %iiv0, 2\n"
        "\t%ieoff =l mul %iiv, %st\n\t%ie =l add %ent, %ieoff\n"
        "\t%ist =l loadl %ie\n"
        "\tjnz %ist, @icmp, @inext\n@icmp\n"
        "\t%ikp =l add %ie, 8\n\t%ike =l loadl %ikp\n"
        "\t%ieq =w ceql %k, %ike\n"
        "\tjnz %ieq, @ihit, @inext\n@ihit\n"
        "\tret %ie\n@inext\n"
        "\t%is1 =l add %is, 1\n\t%is2 =l and %is1, %mask\n\tstorel %is2, %sp\n"
        "\tjmp @iloop\n@inone\n\tret 0\n"
        // str keys: inline FNV-1a, length-gated memcmp compare
        "@hs\n"
        "\t%lp =l sub %k, 8\n\t%len =l loadl %lp\n"
        "\t%hp =l alloc8 8\n\t%ip =l alloc8 8\n"
        "\tstorel -3750763034362895579, %hp\n\tstorel 0, %ip\n"
        "@hloop\n"
        "\t%i =l loadl %ip\n"
        "\t%c =w csltl %i, %len\n"
        "\tjnz %c, @hbody, @hdone\n@hbody\n"
        "\t%bp =l add %k, %i\n\t%b =l loadub %bp\n"
        "\t%h0 =l loadl %hp\n"
        "\t%hx =l xor %h0, %b\n"
        "\t%hm =l mul %hx, 1099511628211\n"
        "\tstorel %hm, %hp\n"
        "\t%i1 =l add %i, 1\n\tstorel %i1, %ip\n\tjmp @hloop\n@hdone\n"
        "\t%hs2 =l loadl %hp\n"
        "\t%ss0 =l and %hs2, %mask\n\tstorel %ss0, %sp\n"
        "@sloop\n"
        "\t%ss =l loadl %sp\n"
        "\t%soff =l mul %ss, 4\n\t%sivp =l add %idx, %soff\n\t%siv0 =l loadsw %sivp\n"
        "\t%semp =w ceql %siv0, 0\n"
        "\tjnz %semp, @snone, @schkd\n@schkd\n"
        "\t%sdum =w ceql %siv0, 1\n"
        "\tjnz %sdum, @snext, @schk\n@schk\n"
        "\t%siv =l sub %siv0, 2\n"
        "\t%seoff =l mul %siv, %st\n\t%se =l add %ent, %seoff\n"
        "\t%sst =l loadl %se\n"
        "\tjnz %sst, @scmp, @snext\n@scmp\n"
        "\t%skp =l add %se, 8\n\t%ske =l loadl %skp\n"
        "\t%kelp =l sub %ske, 8\n\t%kel =l loadl %kelp\n"
        "\t%sle =w ceql %len, %kel\n"
        "\tjnz %sle, @smem, @snext\n@smem\n"
        "\t%mr =w call $memcmp(l %k, l %ske, l %len)\n"
        "\t%mz =w ceqw %mr, 0\n"
        "\tjnz %mz, @shit, @snext\n@shit\n"
        "\tret %se\n@snext\n"
        "\t%ss1 =l add %ss, 1\n\t%ss2 =l and %ss1, %mask\n\tstorel %ss2, %sp\n"
        "\tjmp @sloop\n@snone\n\tret 0\n}\n"
        "function l $simple_map_get(l %m, l %k) {\n@start\n"
        "\t%lep =l add %m, 72\n\t%le =l loadl %lep\n"
        "\tjnz %le, @cchk, @slow\n@cchk\n"
        "\t%lst =l loadl %le\n"
        "\tjnz %lst, @cver, @slow\n@cver\n"
        "\t%ckp =l add %le, 8\n\t%cke =l loadl %ckp\n"
        "\t%ceq =w ceql %cke, %k\n"
        "\tjnz %ceq, @chit, @slow\n@chit\n"
        "\t%cvp =l add %le, 16\n\tret %cvp\n@slow\n"
        "\t%e =l call $simple_map_find(l %m, l %k)\n"
        "\tjnz %e, @ok, @nf\n@ok\n"
        "\tstorel %e, %lep\n"
        "\t%vp =l add %e, 16\n\tret %vp\n@nf\n"
        "\t%kisp =l add %m, 64\n\t%kis =l loadl %kisp\n"
        "\tjnz %kis, @nfs, @nfi\n@nfs\n"
        "\tcall $printf(l $map_nf_s, ..., l %k)\n"
        "\tcall $exit(w 1)\n\thlt\n@nfi\n"
        "\tcall $printf(l $map_nf_i, ..., l %k)\n"
        "\tcall $exit(w 1)\n\thlt\n}\n"
        "function l $simple_map_has(l %m, l %k) {\n@start\n"
        "\t%e =l call $simple_map_find(l %m, l %k)\n"
        "\tjnz %e, @y, @n\n@y\n"
        "\t%lep =l add %m, 72\n\tstorel %e, %lep\n"
        "\tret 1\n@n\n\tret 0\n}\n"
        // grow/compact: fresh arrays, live entries kept in insertion order
        "function $simple_map_grow(l %m) {\n@start\n"
        "\t%psp =l alloc8 8\n\t%fip =l alloc8 8\n\t%jp =l alloc8 8\n\t%wp =l alloc8 8\n\t%necp =l alloc8 8\n\t%nicp =l alloc8 8\n"
        "\t%cp =l add %m, 8\n\t%count =l loadl %cp\n"
        "\t%c1 =l add %count, 1\n\t%nec0 =l mul %c1, 2\n"
        "\t%small =w csltl %nec0, 8\n"
        "\tjnz %small, @min, @keep\n@min\n"
        "\tstorel 8, %necp\n\tjmp @icalc\n@keep\n"
        "\tstorel %nec0, %necp\n\tjmp @icalc\n@icalc\n"
        "\t%nec =l loadl %necp\n"
        "\t%want =l mul %nec, 2\n"
        "\tstorel 16, %nicp\n"
        "@dloop\n"
        "\t%nic =l loadl %nicp\n"
        "\t%enough =w csgel %nic, %want\n"
        "\tjnz %enough, @alloc, @dbl\n@dbl\n"
        "\t%nic2 =l mul %nic, 2\n\tstorel %nic2, %nicp\n\tjmp @dloop\n@alloc\n"
        "\t%nicf =l loadl %nicp\n"
        "\t%stp =l add %m, 56\n\t%st =l loadl %stp\n"
        "\t%nesz =l mul %nec, %st\n\t%ne =l call $malloc(l %nesz)\n"
        "\t%ni =l call $calloc(l %nicf, l 4)\n"
        "\t%nep =l add %m, 16\n\t%nold =l loadl %nep\n"
        "\t%entp =l add %m, 32\n\t%ent =l loadl %entp\n"
        "\t%mask =l sub %nicf, 1\n"
        "\tstorel 0, %jp\n"
        "\tstorel 0, %wp\n"
        "@wloop\n"
        "\t%j =l loadl %jp\n"
        "\t%wc =w csltl %j, %nold\n"
        "\tjnz %wc, @wbody, @wdone\n@wbody\n"
        "\t%eoff =l mul %j, %st\n\t%e =l add %ent, %eoff\n"
        "\t%estate =l loadl %e\n"
        "\tjnz %estate, @live, @wnext\n@live\n"
        "\t%w =l loadl %wp\n"
        "\t%doff =l mul %w, %st\n\t%d =l add %ne, %doff\n"
        "\tcall $memcpy(l %d, l %e, l %st)\n"
        "\t%kp =l add %e, 8\n\t%key =l loadl %kp\n"
        "\t%hh =l call $simple_map_hash(l %m, l %key)\n"
        "\t%ps0 =l and %hh, %mask\n\tstorel %ps0, %psp\n"
        "@rloop\n"
        "\t%ps =l loadl %psp\n"
        "\t%poff =l mul %ps, 4\n\t%pp =l add %ni, %poff\n\t%pv =l loadsw %pp\n"
        "\t%pemp =w ceql %pv, 0\n"
        "\tjnz %pemp, @place, @rnext\n@rnext\n"
        "\t%ps1 =l add %ps, 1\n\t%ps2 =l and %ps1, %mask\n\tstorel %ps2, %psp\n"
        "\tjmp @rloop\n@place\n"
        "\t%wb =l add %w, 2\n\t%ww =w copy %wb\n\tstorew %ww, %pp\n"
        "\t%w1 =l add %w, 1\n\tstorel %w1, %wp\n"
        "\tjmp @wnext\n@wnext\n"
        "\t%j1 =l add %j, 1\n\tstorel %j1, %jp\n\tjmp @wloop\n@wdone\n"
        "\t%oldent =l loadl %entp\n\tcall $free(l %oldent)\n"
        "\t%idxp =l add %m, 40\n\t%oldidx =l loadl %idxp\n\tcall $free(l %oldidx)\n"
        "\tstorel %ne, %entp\n"
        "\tstorel %ni, %idxp\n"
        "\t%icp =l add %m, 48\n\tstorel %nicf, %icp\n"
        "\t%ecp =l add %m, 24\n\tstorel %nec, %ecp\n"
        "\t%wf =l loadl %wp\n\tstorel %wf, %nep\n"
        "\t%clkp =l add %m, 72\n\tstorel 0, %clkp\n"
        "\tret\n}\n"
        // put: value slot for the key (dtor runs on an overwritten value;
        // a new slot is zeroed, its key retained). Caller stores the value.
        "function l $simple_map_put(l %m, l %k, l %dtor) {\n@start\n"
        "\t%sp =l alloc8 8\n\t%zp =l alloc8 8\n\t%finsp =l alloc8 8\n"
        "\t%hp =l alloc8 8\n\t%ep =l alloc8 8\n\t%ivpp =l alloc8 8\n"
        "\t%fip2 =l alloc8 8\n"
        "\t%clep =l add %m, 72\n"
        "\t%cle =l loadl %clep\n"
        "\tjnz %cle, @cchk, @nocache\n@cchk\n"
        "\t%clst =l loadl %cle\n"
        "\tjnz %clst, @cver, @nocache\n@cver\n"
        "\t%cckp =l add %cle, 8\n\t%ccke =l loadl %cckp\n"
        "\t%cceq =w ceql %ccke, %k\n"
        "\tjnz %cceq, @chit, @nocache\n@chit\n"
        "\tstorel %cle, %ep\n\tjmp @upd\n@nocache\n"
        "\t%kisp =l add %m, 64\n\t%kis =l loadl %kisp\n"
        "\t%icp =l add %m, 48\n\t%ic =l loadl %icp\n"
        "\t%mask =l sub %ic, 1\n"
        "\t%idxp =l add %m, 40\n\t%idx =l loadl %idxp\n"
        "\t%entp =l add %m, 32\n\t%ent =l loadl %entp\n"
        "\t%stp =l add %m, 56\n\t%st =l loadl %stp\n"
        "\tstorel -1, %finsp\n"
        "\tjnz %kis, @hs, @hi\n"
        "@hi\n"
        "\t%x1 =l shr %k, 30\n\t%x2 =l xor %k, %x1\n"
        "\t%x3 =l mul %x2, -4658895280553007687\n"
        "\t%x4 =l shr %x3, 27\n\t%x5 =l xor %x3, %x4\n"
        "\t%x6 =l mul %x5, -7723592293110705173\n"
        "\t%x7 =l shr %x6, 31\n\t%hi2 =l xor %x6, %x7\n"
        "\tstorel %hi2, %hp\n"
        "\t%si0 =l and %hi2, %mask\n\tstorel %si0, %sp\n"
        "@iloop\n"
        "\t%is =l loadl %sp\n"
        "\t%ioff =l mul %is, 4\n\t%iivp =l add %idx, %ioff\n\t%iiv0 =l loadsw %iivp\n"
        "\t%iemp =w ceql %iiv0, 0\n"
        "\tjnz %iemp, @absent, @ichkd\n@ichkd\n"
        "\t%idum =w ceql %iiv0, 1\n"
        "\tjnz %idum, @idummy, @ichk\n@idummy\n"
        "\t%ifi =l loadl %finsp\n"
        "\t%ifneg =w ceql %ifi, -1\n"
        "\tjnz %ifneg, @iremember, @inext\n@iremember\n"
        "\tstorel %is, %finsp\n\tjmp @inext\n@ichk\n"
        "\t%iiv =l sub %iiv0, 2\n"
        "\t%ieoff =l mul %iiv, %st\n\t%ie =l add %ent, %ieoff\n"
        "\t%ist =l loadl %ie\n"
        "\tjnz %ist, @icmp, @inext\n@icmp\n"
        "\t%ikp =l add %ie, 8\n\t%ike =l loadl %ikp\n"
        "\t%ieq =w ceql %k, %ike\n"
        "\tjnz %ieq, @ihit, @inext\n@ihit\n"
        "\tstorel %ie, %ep\n\tjmp @upd\n@inext\n"
        "\t%is1 =l add %is, 1\n\t%is2 =l and %is1, %mask\n\tstorel %is2, %sp\n"
        "\tjmp @iloop\n"
        "@hs\n"
        "\t%lp =l sub %k, 8\n\t%len =l loadl %lp\n"
        "\tstorel -3750763034362895579, %hp\n\tstorel 0, %zp\n"
        "@hloop\n"
        "\t%i =l loadl %zp\n"
        "\t%c =w csltl %i, %len\n"
        "\tjnz %c, @hbody, @hdone\n@hbody\n"
        "\t%bp =l add %k, %i\n\t%b =l loadub %bp\n"
        "\t%h0 =l loadl %hp\n"
        "\t%hx =l xor %h0, %b\n"
        "\t%hm =l mul %hx, 1099511628211\n"
        "\tstorel %hm, %hp\n"
        "\t%i1 =l add %i, 1\n\tstorel %i1, %zp\n\tjmp @hloop\n@hdone\n"
        "\t%hs2 =l loadl %hp\n"
        "\t%ss0 =l and %hs2, %mask\n\tstorel %ss0, %sp\n"
        "@sloop\n"
        "\t%ss =l loadl %sp\n"
        "\t%soff =l mul %ss, 4\n\t%sivp =l add %idx, %soff\n\t%siv0 =l loadsw %sivp\n"
        "\t%semp =w ceql %siv0, 0\n"
        "\tjnz %semp, @absent, @schkd\n@schkd\n"
        "\t%sdum =w ceql %siv0, 1\n"
        "\tjnz %sdum, @sdummy, @schk\n@sdummy\n"
        "\t%sfi =l loadl %finsp\n"
        "\t%sfneg =w ceql %sfi, -1\n"
        "\tjnz %sfneg, @sremember, @snext\n@sremember\n"
        "\tstorel %ss, %finsp\n\tjmp @snext\n@schk\n"
        "\t%siv =l sub %siv0, 2\n"
        "\t%seoff =l mul %siv, %st\n\t%se =l add %ent, %seoff\n"
        "\t%sst =l loadl %se\n"
        "\tjnz %sst, @scmp, @snext\n@scmp\n"
        "\t%skp =l add %se, 8\n\t%ske =l loadl %skp\n"
        "\t%kelp =l sub %ske, 8\n\t%kel =l loadl %kelp\n"
        "\t%sle =w ceql %len, %kel\n"
        "\tjnz %sle, @smem, @snext\n@smem\n"
        "\t%mr =w call $memcmp(l %k, l %ske, l %len)\n"
        "\t%mz =w ceqw %mr, 0\n"
        "\tjnz %mz, @shit, @snext\n@shit\n"
        "\tstorel %se, %ep\n\tjmp @upd\n@snext\n"
        "\t%ss1 =l add %ss, 1\n\t%ss2 =l and %ss1, %mask\n\tstorel %ss2, %sp\n"
        "\tjmp @sloop\n"
        "@upd\n"
        "\t%e =l loadl %ep\n"
        "\tstorel %e, %clep\n"
        "\t%vp =l add %e, 16\n"
        "\tjnz %dtor, @rundt, @updone\n@rundt\n"
        "\tcall %dtor(l %vp)\n\tjmp @updone\n@updone\n"
        "\tret %vp\n@absent\n"
        // key not present: grow if needed (then reprobe a clean table),
        // otherwise insert at the first dummy seen, or where probing stopped
        "\t%nep =l add %m, 16\n\t%ne =l loadl %nep\n"
        "\t%ecp =l add %m, 24\n\t%ec =l loadl %ecp\n"
        "\t%n1 =l add %ne, 1\n\t%n3 =l mul %n1, 3\n"
        "\t%i2 =l mul %ic, 2\n"
        "\t%over =w csgel %n3, %i2\n"
        "\tjnz %over, @dogrow, @entchk\n@entchk\n"
        "\t%full =w ceql %ne, %ec\n"
        "\tjnz %full, @egrow, @pick\n@egrow\n"
        // entries full but the index has room: double the entry array in
        // place — positions are stable, so the index needs no rebuild
        "\t%gec =l mul %ec, 2\n"
        "\t%gsz =l mul %gec, %st\n"
        "\t%gent =l call $realloc(l %ent, l %gsz)\n"
        "\t%gentp =l add %m, 32\n\tstorel %gent, %gentp\n"
        "\tstorel %gec, %ecp\n"
        "\tstorel 0, %clep\n"      // entries may have moved: kill the cache
        "\tjmp @pick\n@pick\n"
        "\t%fi3 =l loadl %finsp\n"
        "\t%fneg2 =w ceql %fi3, -1\n"
        "\tjnz %fneg2, @useprobe, @usedummy\n@usedummy\n"
        "\t%doff =l mul %fi3, 4\n\t%divp =l add %idx, %doff\n"
        "\tstorel %divp, %ivpp\n\tjmp @ins2\n@useprobe\n"
        "\t%us =l loadl %sp\n"
        "\t%uoff =l mul %us, 4\n\t%uivp =l add %idx, %uoff\n"
        "\tstorel %uivp, %ivpp\n\tjmp @ins2\n@dogrow\n"
        "\tcall $simple_map_grow(l %m)\n"
        "\t%h =l loadl %hp\n"
        "\t%gicp =l add %m, 48\n\t%gic =l loadl %gicp\n"
        "\t%gmask =l sub %gic, 1\n"
        "\t%gidxp =l add %m, 40\n\t%gidx =l loadl %gidxp\n"
        "\t%gs0 =l and %h, %gmask\n\tstorel %gs0, %fip2\n"
        "@gloop\n"
        "\t%gs =l loadl %fip2\n"
        "\t%goff =l mul %gs, 4\n\t%givp =l add %gidx, %goff\n\t%giv =l loadsw %givp\n"
        "\t%gemp =w ceql %giv, 0\n"
        "\tjnz %gemp, @gins, @gnext\n@gnext\n"
        "\t%gs1 =l add %gs, 1\n\t%gs2 =l and %gs1, %gmask\n\tstorel %gs2, %fip2\n"
        "\tjmp @gloop\n@gins\n"
        "\tstorel %givp, %ivpp\n\tjmp @ins2\n@ins2\n"
        "\t%fivp =l loadl %ivpp\n"
        "\tjnz %kis, @retk, @noret\n@retk\n"
        "\tcall $simple_retain(l %k)\n\tjmp @noret\n@noret\n"
        "\t%nep2 =l add %m, 16\n\t%j =l loadl %nep2\n"
        "\t%jb =l add %j, 2\n\t%jw =w copy %jb\n\tstorew %jw, %fivp\n"
        "\t%entp2 =l add %m, 32\n\t%ent2 =l loadl %entp2\n"
        "\t%stp2 =l add %m, 56\n\t%st2 =l loadl %stp2\n"
        "\t%eoff2 =l mul %j, %st2\n\t%enew =l add %ent2, %eoff2\n"
        "\tstorel 1, %enew\n"
        "\t%kp2 =l add %enew, 8\n\tstorel %k, %kp2\n"
        "\t%vp2 =l add %enew, 16\n"
        "\t%vend =l add %enew, %st2\n"
        "\tstorel %vp2, %zp\n"
        "@zloop\n"
        "\t%z =l loadl %zp\n"
        "\t%zc =w csltl %z, %vend\n"
        "\tjnz %zc, @zbody, @zdone\n@zbody\n"
        "\tstorel 0, %z\n"
        "\t%z1 =l add %z, 8\n\tstorel %z1, %zp\n\tjmp @zloop\n@zdone\n"
        "\t%j1 =l add %j, 1\n\tstorel %j1, %nep2\n"
        "\t%cp =l add %m, 8\n\t%cnt =l loadl %cp\n"
        "\t%cnt1 =l add %cnt, 1\n\tstorel %cnt1, %cp\n"
        "\t%clkp =l add %m, 72\n\tstorel 0, %clkp\n"
        "\tret %vp2\n}\n"
        // del: absent key is a no-op; present key releases and tombstones
        "function $simple_map_del(l %m, l %k, l %dtor) {\n@start\n"
        "\t%h =l call $simple_map_hash(l %m, l %k)\n"
        "\t%icp =l add %m, 48\n\t%ic =l loadl %icp\n"
        "\t%mask =l sub %ic, 1\n"
        "\t%idxp =l add %m, 40\n\t%idx =l loadl %idxp\n"
        "\t%entp =l add %m, 32\n\t%ent =l loadl %entp\n"
        "\t%stp =l add %m, 56\n\t%st =l loadl %stp\n"
        "\t%sp =l alloc8 8\n"
        "\t%s0 =l and %h, %mask\n\tstorel %s0, %sp\n"
        "@ploop\n"
        "\t%s =l loadl %sp\n"
        "\t%off =l mul %s, 4\n\t%ivp =l add %idx, %off\n\t%iv0 =l loadsw %ivp\n"
        "\t%emp =w ceql %iv0, 0\n"
        "\tjnz %emp, @done, @chkdum\n@chkdum\n"
        "\t%dum =w ceql %iv0, 1\n"
        "\tjnz %dum, @next, @chk\n@chk\n"
        "\t%iv =l sub %iv0, 2\n"
        "\t%eoff =l mul %iv, %st\n\t%e =l add %ent, %eoff\n"
        "\t%estate =l loadl %e\n"
        "\tjnz %estate, @cmp, @next\n@cmp\n"
        "\t%kp =l add %e, 8\n\t%ke =l loadl %kp\n"
        "\t%eq =w call $simple_map_keyeq(l %m, l %k, l %ke)\n"
        "\tjnz %eq, @kill, @next\n@kill\n"
        "\tstorew 1, %ivp\n"
        "\tstorel 0, %e\n"
        "\t%clkp =l add %m, 72\n\tstorel 0, %clkp\n"
        "\t%kisp =l add %m, 64\n\t%kis =l loadl %kisp\n"
        "\tjnz %kis, @relk, @nokrel\n@relk\n"
        "\tcall $simple_release(l %ke)\n\tjmp @nokrel\n@nokrel\n"
        "\tjnz %dtor, @rundt, @nodt\n@rundt\n"
        "\t%vp =l add %e, 16\n\tcall %dtor(l %vp)\n\tjmp @nodt\n@nodt\n"
        "\t%cp =l add %m, 8\n\t%cnt =l loadl %cp\n"
        "\t%cnt1 =l sub %cnt, 1\n\tstorel %cnt1, %cp\n"
        "\tret\n@next\n"
        "\t%s1 =l add %s, 1\n\t%s2 =l and %s1, %mask\n\tstorel %s2, %sp\n"
        "\tjmp @ploop\n@done\n\tret\n}\n"
        "function $simple_map_retain(l %m) {\n@start\n"
        "\t%c =l loadl %m\n\t%c1 =l add %c, 1\n\tstorel %c1, %m\n\tret\n}\n"
        "function $simple_map_release(l %m, l %dtor) {\n@start\n"
        "\t%jp =l alloc8 8\n"
        "\t%c =l loadl %m\n\t%c1 =l sub %c, 1\n"
        "\t%z =w ceql %c1, 0\n"
        "\tjnz %z, @fre, @keep\n@keep\n"
        "\tstorel %c1, %m\n\tret\n@fre\n"
        "\t%nep =l add %m, 16\n\t%ne =l loadl %nep\n"
        "\t%entp =l add %m, 32\n\t%ent =l loadl %entp\n"
        "\t%stp =l add %m, 56\n\t%st =l loadl %stp\n"
        "\t%kisp =l add %m, 64\n\t%kis =l loadl %kisp\n"
        "\tstorel 0, %jp\n"
        "@floop\n"
        "\t%j =l loadl %jp\n"
        "\t%fc =w csltl %j, %ne\n"
        "\tjnz %fc, @fbody, @fdone\n@fbody\n"
        "\t%eoff =l mul %j, %st\n\t%e =l add %ent, %eoff\n"
        "\t%estate =l loadl %e\n"
        "\tjnz %estate, @rel, @fnext\n@rel\n"
        "\tjnz %kis, @relk, @nokrel\n@relk\n"
        "\t%kp =l add %e, 8\n\t%key =l loadl %kp\n"
        "\tcall $simple_release(l %key)\n\tjmp @nokrel\n@nokrel\n"
        "\tjnz %dtor, @rundt, @fnext\n@rundt\n"
        "\t%vp =l add %e, 16\n\tcall %dtor(l %vp)\n\tjmp @fnext\n@fnext\n"
        "\t%j1 =l add %j, 1\n\tstorel %j1, %jp\n\tjmp @floop\n@fdone\n"
        "\tcall $free(l %ent)\n"
        "\t%idxp =l add %m, 40\n\t%idx =l loadl %idxp\n\tcall $free(l %idx)\n"
        "\tcall $free(l %m)\n\tret\n}\n"
        // structural clone shared by copy (deep, for threads) and unique (COW)
        "function l $simple_map_clone(l %m) {\n@start\n"
        "\t%n =l call $malloc(l 88)\n"
        "\tcall $memcpy(l %n, l %m, l 88)\n"
        "\tstorel 1, %n\n"
        "\t%clkp =l add %n, 72\n\tstorel 0, %clkp\n"
        "\t%ecp =l add %m, 24\n\t%ec =l loadl %ecp\n"
        "\t%stp =l add %m, 56\n\t%st =l loadl %stp\n"
        "\t%nep =l add %m, 16\n\t%ne =l loadl %nep\n"
        "\t%esz =l mul %ec, %st\n\t%news =l call $malloc(l %esz)\n"
        "\t%entp =l add %m, 32\n\t%ent =l loadl %entp\n"
        "\t%used =l mul %ne, %st\n"
        "\tcall $memcpy(l %news, l %ent, l %used)\n"
        "\t%nentp =l add %n, 32\n\tstorel %news, %nentp\n"
        "\t%icp =l add %m, 48\n\t%ic =l loadl %icp\n"
        "\t%isz =l mul %ic, 4\n\t%newi =l call $malloc(l %isz)\n"
        "\t%idxp =l add %m, 40\n\t%idx =l loadl %idxp\n"
        "\tcall $memcpy(l %newi, l %idx, l %isz)\n"
        "\t%nidxp =l add %n, 40\n\tstorel %newi, %nidxp\n"
        "\tret %n\n}\n"
        // deep copy for a thread boundary: keys copied, values send-copied
        "function l $simple_map_copy(l %m, l %vcopy) {\n@start\n"
        "\t%jp =l alloc8 8\n"
        "\t%n =l call $simple_map_clone(l %m)\n"
        "\t%nep =l add %n, 16\n\t%ne =l loadl %nep\n"
        "\t%entp =l add %n, 32\n\t%ent =l loadl %entp\n"
        "\t%stp =l add %n, 56\n\t%st =l loadl %stp\n"
        "\t%kisp =l add %n, 64\n\t%kis =l loadl %kisp\n"
        "\tstorel 0, %jp\n"
        "@floop\n"
        "\t%j =l loadl %jp\n"
        "\t%fc =w csltl %j, %ne\n"
        "\tjnz %fc, @fbody, @fdone\n@fbody\n"
        "\t%eoff =l mul %j, %st\n\t%e =l add %ent, %eoff\n"
        "\t%estate =l loadl %e\n"
        "\tjnz %estate, @fix, @fnext\n@fix\n"
        "\tjnz %kis, @cpk, @nokey\n@cpk\n"
        "\t%kp =l add %e, 8\n\t%key =l loadl %kp\n"
        "\t%kc =l call $simple_strcopy(l %key)\n"
        "\tstorel %kc, %kp\n\tjmp @nokey\n@nokey\n"
        "\tjnz %vcopy, @runvc, @fnext\n@runvc\n"
        "\t%vp =l add %e, 16\n\tcall %vcopy(l %vp)\n\tjmp @fnext\n@fnext\n"
        "\t%j1 =l add %j, 1\n\tstorel %j1, %jp\n\tjmp @floop\n@fdone\n"
        "\tret %n\n}\n"
        // COW: unshared passes through; shared clones, retaining keys/values
        "function l $simple_map_unique(l %m, l %vret) {\n@start\n"
        "\t%jp =l alloc8 8\n"
        "\t%c =l loadl %m\n"
        "\t%one =w ceql %c, 1\n"
        "\tjnz %one, @mine, @cow\n@mine\n"
        "\tret %m\n@cow\n"
        "\t%n =l call $simple_map_clone(l %m)\n"
        "\t%nep =l add %n, 16\n\t%ne =l loadl %nep\n"
        "\t%entp =l add %n, 32\n\t%ent =l loadl %entp\n"
        "\t%stp =l add %n, 56\n\t%st =l loadl %stp\n"
        "\t%kisp =l add %n, 64\n\t%kis =l loadl %kisp\n"
        "\tstorel 0, %jp\n"
        "@floop\n"
        "\t%j =l loadl %jp\n"
        "\t%fc =w csltl %j, %ne\n"
        "\tjnz %fc, @fbody, @fdone\n@fbody\n"
        "\t%eoff =l mul %j, %st\n\t%e =l add %ent, %eoff\n"
        "\t%estate =l loadl %e\n"
        "\tjnz %estate, @ret2, @fnext\n@ret2\n"
        "\tjnz %kis, @retk, @nokey\n@retk\n"
        "\t%kp =l add %e, 8\n\t%key =l loadl %kp\n"
        "\tcall $simple_retain(l %key)\n\tjmp @nokey\n@nokey\n"
        "\tjnz %vret, @runvr, @fnext\n@runvr\n"
        "\t%vp =l add %e, 16\n\tcall %vret(l %vp)\n\tjmp @fnext\n@fnext\n"
        "\t%j1 =l add %j, 1\n\tstorel %j1, %jp\n\tjmp @floop\n@fdone\n"
        "\t%c2 =l sub %c, 1\n\tstorel %c2, %m\n"
        "\tret %n\n}\n";
    }

    // ---- emission helpers ----

    std::string newTmp() { return "%t" + std::to_string(++tmpN_); }
    std::string newLbl(const char* base) {
        return "@" + std::string(base) + "_" + std::to_string(++lblN_);
    }
    static bool isBool(const Type& t) { return t.kind == TypeKind::Bool; }
    static bool isStr(const Type& t) { return t.kind == TypeKind::Str; }
    static bool isChan(const Type& t) { return t.kind == TypeKind::Chan; }
    static bool isList(const Type& t) { return t.kind == TypeKind::List; }
    static bool isMap(const Type& t) { return t.kind == TypeKind::Map; }
    // an `error` is represented exactly like a str (pointer to a 16-byte
    // string header), except it may be null — null is the `ok` value. All rc
    // ops route through null-safe wrappers ($simple_err_*).
    static bool isErr(const Type& t) { return t.kind == TypeKind::Error; }
    static bool isRcScalar(const Type& t) {
        return isStr(t) || isChan(t) || isList(t) || isErr(t) || isMap(t);
    }
    // QBE has only w (32-bit) and l (64-bit) registers; narrower ints live
    // in w and are truncated/extended at the memory boundary.
    static char intClass(const Type& t) {
        if (isInt(t)) return t.bits == 64 ? 'l' : 'w';
        return 'l';
    }
    // width of an equality comparison: bools are 32-bit, everything else
    // that reaches here (ints, pointers, channels, strings) is 64-bit unless
    // it is a narrow integer
    static const char* cmpSuffix(const Type& t) {
        if (t.kind == TypeKind::Bool) return "w";
        return (isInt(t) && t.bits <= 32) ? "w" : "l";
    }
    static char qbeType(const Type& t) {
        if (isBool(t)) return 'w';
        if (isInt(t)) return intClass(t);
        if (isFloat(t)) return t.bits == 32 ? 's' : 'd';
        return 'l';
    }

    void emit(const std::string& s) { blocks_.back().ins.push_back(parseInst(s)); }
    void placeLabel(const std::string& l) {
        if (!terminated_) emit("jmp " + l);
        blocks_.push_back(MBlock{l, {}});
        terminated_ = false;
    }

    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }

    std::string addSlot(const std::string& name, const Type& t, bool ownsRefs = true) {
        std::string slot = "%" + name + "_" + std::to_string(++slotN_);
        long sz = sizeOf(t), al = isAggregate(t) ? alignOf(t) : sz;
        // simple(v0.95): 16-byte-align arrays/aggregates of 16+ bytes so the
        // vector engine can emit aligned NEON/SSE loads over them. We own the
        // frame layout, so this alignment is free; over-alignment is always
        // sound. (list data is malloc'd, already 16-byte aligned.)
        const char* ins = (isAggregate(t) && sz >= 16) ? "alloc16"
                          : al >= 8 ? "alloc8" : "alloc4";
        if (sz < 4) sz = 4; // keep small scalars in a full word slot
        allocs_.push_back("\t" + slot + " =l " + ins + " " + std::to_string(sz));
        scopes_.back()[name] = {slot, t, ownsRefs};
        return slot;
    }
    std::string hiddenSlot(const char* base, long size) {
        std::string slot = "%" + std::string(base) + "_" + std::to_string(++slotN_);
        allocs_.push_back("\t" + slot + " =l alloc8 " + std::to_string(size));
        return slot;
    }
    Slot& findSlot(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        err(0, "internal error: no slot for variable '" + name + "'");
    }

    // QBE store mnemonic for a scalar type
    static const char* storeOp(const Type& t) {
        if (isInt(t)) {
            switch (t.bits) {
            case 8: return "storeb";
            case 16: return "storeh";
            case 32: return "storew";
            default: return "storel";
            }
        }
        if (isFloat(t)) return t.bits == 32 ? "stores" : "stored";
        if (t.kind == TypeKind::Bool) return "storeb";
        return "storel"; // str, chan, ptr
    }
    // QBE load mnemonic, honouring signedness for narrow ints
    static std::string loadOp(const Type& t) {
        if (isInt(t)) {
            switch (t.bits) {
            case 8: return t.uns ? "loadub" : "loadsb";
            case 16: return t.uns ? "loaduh" : "loadsh";
            case 32: return t.uns ? "loaduw" : "loadsw";
            default: return "loadl";
            }
        }
        if (isFloat(t)) return "load"; // typed by the destination class (s/d)
        if (t.kind == TypeKind::Bool) return "loadub";
        return "loadl";
    }

    // QBE computes in 32/64-bit registers, so arithmetic on an 8- or
    // 16-bit type has to be wrapped back into range explicitly. 32-bit
    // types wrap for free in a w register.
    std::string narrow(const std::string& v, const Type& t) {
        if (!isInt(t) || t.bits >= 32) return v;
        std::string r = newTmp();
        if (t.uns) {
            long mask = (1L << t.bits) - 1;
            emit(r + " =w and " + v + ", " + std::to_string(mask));
        } else {
            emit(r + " =w exts" + std::string(t.bits == 8 ? "b" : "h") + " " + v);
        }
        return r;
    }

    // Copy `size` bytes from src to dst. QBE's `blit` fully unrolls into
    // load/store pairs, which is ideal for a small struct and catastrophic
    // for a large array (a 320 KB copy became 160k instructions), so
    // anything sizeable goes through memcpy instead.
    void emitCopy(const std::string& dst, const std::string& src, long size) {
        if (size <= 128) {
            emit("blit " + src + ", " + dst + ", " + std::to_string(size));
        } else {
            emit("call $memcpy(l " + dst + ", l " + src + ", l " +
                 std::to_string(size) + ")");
        }
    }

    void storeScalar(const std::string& addr, const Type& t, const std::string& val) {
        emit(std::string(storeOp(t)) + " " + val + ", " + addr);
    }
    std::string loadScalar(const std::string& addr, const Type& t) {
        std::string r = newTmp();
        // a 32-bit signed load into an l register needs the wide form
        emit(r + " =" + std::string(1, qbeType(t)) + " " + loadOp(t) + " " + addr);
        return r;
    }

    void storeToSlot(const Slot& s, const std::string& val) {
        if (isAggregate(s.type))
            emitCopy(s.addr, val, sizeOf(s.type));
        else
            storeScalar(s.addr, s.type, val);
    }
    void storeInterior(const std::string& addr, const Type& t, const std::string& val) {
        if (isAggregate(t))
            emitCopy(addr, val, sizeOf(t));
        else
            storeScalar(addr, t, val);
    }

    // ---- ARC helpers ----

    void emitRetainStr(const std::string& v) {
        needRC_ = true;
        emit("call $simple_retain(l " + v + ")");
    }
    void emitReleaseStr(const std::string& v) {
        needRC_ = true;
        emit("call $simple_release(l " + v + ")");
    }
    // retain/release everything reachable from a value of type t
    void emitRetainValue(const std::string& v, const Type& t) {
        if (isStr(t)) emitRetainStr(v);
        else if (isErr(t)) { needErr_ = true; emit("call $simple_err_retain(l " + v + ")"); }
        else if (isChan(t)) { needChan_ = true; emit("call $simple_chan_retain(l " + v + ")"); }
        else if (isList(t)) { needList_ = true; emit("call $simple_list_retain(l " + v + ")"); }
        else if (isMap(t)) { needMap_ = true; emit("call $simple_map_retain(l " + v + ")"); }
        else if (typeHasRc(t)) emit("call " + rcHelper(t, 'r') + "(l " + v + ")");
    }
    void emitReleaseValue(const std::string& v, const Type& t) {
        if (isStr(t)) emitReleaseStr(v);
        else if (isErr(t)) { needErr_ = true; emit("call $simple_err_release(l " + v + ")"); }
        else if (isChan(t)) { needChan_ = true; emit("call $simple_chan_release(l " + v + ")"); }
        else if (isList(t)) {
            needList_ = true;
            std::string dtor = typeHasRc(*t.elem) ? listElemDtor(t) : "0";
            emit("call $simple_list_release(l " + v + ", l " + dtor + ")");
        }
        else if (isMap(t)) {
            needMap_ = true;
            std::string dtor = typeHasRc(*t.elem) ? mapValDtor(t) : "0";
            emit("call $simple_map_release(l " + v + ", l " + dtor + ")");
        }
        else if (typeHasRc(t)) emit("call " + rcHelper(t, 'd') + "(l " + v + ")");
    }
    // Deep-copies one element in place (for sending a list across a thread):
    // replaces a shared rc handle with a private copy so nothing is shared.
    std::string listElemSendCopy(const Type& listT) {
        const Type& e = *listT.elem;
        std::string name = "$list_escpy_" + mangle(e);
        if (rcEmitted_.count(name)) return name;
        rcEmitted_.insert(name);
        std::ostringstream f;
        f << "function " << name << "(l %p) {\n@start\n";
        if (isStr(e)) {
            needStrCopy_ = true;
            f << "\t%v =l loadl %p\n\t%c =l call $simple_strcopy(l %v)\n"
                 "\tstorel %c, %p\n";
        } else if (isErr(e)) {
            needErr_ = true;
            f << "\t%v =l loadl %p\n\t%c =l call $simple_err_copy(l %v)\n"
                 "\tstorel %c, %p\n";
        } else if (isList(e)) {
            needList_ = true;
            std::string inner = typeHasRc(*e.elem) ? listElemSendCopy(e) : "0";
            f << "\t%v =l loadl %p\n\t%c =l call $simple_list_copy(l %v, l " << inner
              << ")\n\tstorel %c, %p\n";
        } else if (isMap(e)) {
            needMap_ = true;
            std::string inner = typeHasRc(*e.elem) ? mapValSendCopy(e) : "0";
            f << "\t%v =l loadl %p\n\t%c =l call $simple_map_copy(l %v, l " << inner
              << ")\n\tstorel %c, %p\n";
        } else if (isChan(e)) {
            // channels are the one intentionally-shared object; retain it
            needChan_ = true;
            f << "\t%v =l loadl %p\n\tcall $simple_chan_retain(l %v)\n";
        } else {
            f << "\tcall " << rcHelper(e, 'c') << "(l %p)\n";
        }
        f << "\tret\n}\n";
        rcFuncs_.push_back(f.str());
        return name;
    }
    // symmetric to listElemDtor: retain one element (used by COW copy).
    std::string listElemRetain(const Type& listT) {
        const Type& e = *listT.elem;
        std::string name = "$list_eret_" + mangle(e);
        if (rcEmitted_.count(name)) return name;
        rcEmitted_.insert(name);
        std::ostringstream f;
        f << "function " << name << "(l %p) {\n@start\n";
        if (isRcScalar(e)) {
            f << "\t%v =l loadl %p\n";
            if (isStr(e)) { needRC_ = true; f << "\tcall $simple_retain(l %v)\n"; }
            else if (isErr(e)) { needErr_ = true; f << "\tcall $simple_err_retain(l %v)\n"; }
            else if (isChan(e)) { needChan_ = true; f << "\tcall $simple_chan_retain(l %v)\n"; }
            else if (isMap(e)) { needMap_ = true; f << "\tcall $simple_map_retain(l %v)\n"; }
            else { needList_ = true; f << "\tcall $simple_list_retain(l %v)\n"; }
        } else {
            f << "\tcall " << rcHelper(e, 'r') << "(l %p)\n";
        }
        f << "\tret\n}\n";
        rcFuncs_.push_back(f.str());
        return name;
    }
    // Map values reuse the list element helpers: both operate on one value
    // slot of the elem type (the mangled name depends only on the elem).
    Type fakeListOf(const Type& elemT) {
        Type f;
        f.kind = TypeKind::List;
        f.elem = std::make_shared<Type>(elemT);
        return f;
    }
    std::string mapValDtor(const Type& mapT) { return listElemDtor(fakeListOf(*mapT.elem)); }
    std::string mapValRetain(const Type& mapT) { return listElemRetain(fakeListOf(*mapT.elem)); }
    std::string mapValSendCopy(const Type& mapT) { return listElemSendCopy(fakeListOf(*mapT.elem)); }

    // a helper that releases one element of a list (called by the runtime
    // over every slot when the list is freed); returns the helper name
    std::string listElemDtor(const Type& listT) {
        const Type& e = *listT.elem;
        std::string name = "$list_edtor_" + mangle(e);
        if (rcEmitted_.count(name)) return name;
        rcEmitted_.insert(name);
        std::ostringstream f;
        f << "function " << name << "(l %p) {\n@start\n";
        // %p points at one element slot; release whatever rc it holds
        if (isRcScalar(e)) {
            std::string ld = e.bits == 0 ? "loadl" : "loadl";
            (void)ld;
            f << "\t%v =l loadl %p\n";
            if (isStr(e)) { needRC_ = true; f << "\tcall $simple_release(l %v)\n"; }
            else if (isErr(e)) { needErr_ = true; f << "\tcall $simple_err_release(l %v)\n"; }
            else if (isChan(e)) { needChan_ = true; f << "\tcall $simple_chan_release(l %v)\n"; }
            else if (isMap(e)) {
                needMap_ = true;
                std::string inner = typeHasRc(*e.elem) ? mapValDtor(e) : "0";
                f << "\tcall $simple_map_release(l %v, l " << inner << ")\n";
            } else { // nested list
                needList_ = true;
                std::string inner = typeHasRc(*e.elem) ? listElemDtor(e) : "0";
                f << "\tcall $simple_list_release(l %v, l " << inner << ")\n";
            }
        } else {
            f << "\tcall " << rcHelper(e, 'd') << "(l %p)\n";
        }
        f << "\tret\n}\n";
        rcFuncs_.push_back(f.str());
        return name;
    }
    void emitReleaseSlot(const Slot& sl) {
        if (isRcScalar(sl.type)) {
            std::string v = newTmp();
            emit(v + " =l loadl " + sl.addr);
            emitReleaseValue(v, sl.type);
        } else if (typeHasRc(sl.type)) {
            emit("call " + rcHelper(sl.type, 'd') + "(l " + sl.addr + ")");
        }
    }

    static bool producesOwned(const Expr& e) {
        return e.kind == ExprKind::Call || e.kind == ExprKind::StructLit ||
               e.kind == ExprKind::ArrayLit || e.kind == ExprKind::ChanNew ||
               e.kind == ExprKind::ListNew || e.kind == ExprKind::MapNew ||
               (e.kind == ExprKind::Binary && e.op == Tok::Plus &&
                e.type.kind == TypeKind::Str);
    }

    void pushTemp(const std::string& v, const Type& t) {
        if (typeHasRc(t)) stmtTemps_.push_back({v, t});
    }
    void removeTemp(const std::string& v) {
        for (auto it = stmtTemps_.rbegin(); it != stmtTemps_.rend(); ++it) {
            if (it->first == v) {
                stmtTemps_.erase(std::next(it).base());
                return;
            }
        }
    }
    void flushTemps() {
        for (auto& [v, t] : stmtTemps_) emitReleaseValue(v, t);
        stmtTemps_.clear();
    }

    // The caller of this takes ownership of value v produced by expression e:
    // owned producers transfer (drop the pending temp), borrowed sources retain.
    void consumeOwned(const Expr& e, const std::string& v) {
        if (!typeHasRc(e.type)) return;
        if (producesOwned(e)) removeTemp(v);
        else if (e.kind != ExprKind::StrLit) emitRetainValue(v, e.type);
        // StrLit: immortal, retain would be a no-op — skip it
    }

    // Prepares value v (from expression e, type t) to cross a thread
    // boundary and stores it at addr in interior format. Strings: deep-copy,
    // or MOVE when we own the reference and its refcount is 1 (checked at
    // runtime — race-free because counts are thread-local by construction).
    // Channels: share the handle (retain). Aggregates: blit + deep-fix.
    void storeSendValue(Expr& e, const std::string& v, const Type& t,
                        const std::string& addr) {
        if (isStr(t)) {
            std::string vs = newTmp();
            if (producesOwned(e)) {
                removeTemp(v);
                needStrMove_ = true;
                emit(vs + " =l call $simple_strmove(l " + v + ")");
            } else {
                needStrCopy_ = true;
                emit(vs + " =l call $simple_strcopy(l " + v + ")");
            }
            emit("storel " + vs + ", " + addr);
        } else if (isErr(t)) { // nullable str: deep-copy, null-safe
            needErr_ = true;
            std::string vs = newTmp();
            emit(vs + " =l call $simple_err_copy(l " + v + ")");
            if (producesOwned(e)) { removeTemp(v); emitReleaseValue(v, t); }
            emit("storel " + vs + ", " + addr);
        } else if (isMap(t)) {
            needMap_ = true;
            std::string sc = typeHasRc(*t.elem) ? mapValSendCopy(t) : "0";
            std::string vs = newTmp();
            emit(vs + " =l call $simple_map_copy(l " + v + ", l " + sc + ")");
            if (producesOwned(e)) {
                removeTemp(v);
                emitReleaseValue(v, t);
            }
            emit("storel " + vs + ", " + addr);
        } else if (isChan(t)) {
            needChan_ = true;
            if (producesOwned(e)) removeTemp(v);
            else emit("call $simple_chan_retain(l " + v + ")");
            emit("storel " + v + ", " + addr);
        } else if (isList(t)) {
            // a list crossing a thread must be a fully independent copy
            // (its refcount is non-atomic); simple_list_copy makes a new
            // header+buffer, and the element send-copy fn deep-copies any
            // rc elements so nothing heap is shared across the boundary
            needList_ = true;
            std::string scf = typeHasRc(*t.elem) ? listElemSendCopy(t) : "0";
            std::string nl = newTmp();
            emit(nl + " =l call $simple_list_copy(l " + v + ", l " + scf + ")");
            emit("storel " + nl + ", " + addr);
            if (producesOwned(e)) emitReleaseValue(v, t); // drop the owned temp
        } else if (isAggregate(t)) {
            emitCopy(addr, v, sizeOf(t));
            if (typeHasRc(t)) emit("call " + rcHelper(t, 'c') + "(l " + addr + ")");
        } else {
            storeInterior(addr, t, v);
        }
    }

    // Generates (once) the pthread entry point for a spawned function:
    // unpack the argument packet, run, release the packet's references, free.
    std::string spawnTramp(Function* f) {
        std::string name = "$spawn_" + f->linkName;
        if (rcEmitted_.count(name)) return name;
        rcEmitted_.insert(name);
        std::ostringstream t;
        t << "function l " << name << "(l %pk) {\n@start\n";
        long off = 0;
        int n = 0;
        std::string args;
        std::vector<std::pair<long, const Type*>> rcParams;
        for (size_t i = 0; i < f->params.size(); i++) {
            const Type& pt = f->params[i].type;
            std::string addr = "%pk";
            if (off != 0) {
                addr = "%a" + std::to_string(++n);
                t << "\t" << addr << " =l add %pk, " << off << "\n";
            }
            std::string arg;
            if (isAggregate(pt)) {
                arg = addr; // pass a pointer into the packet; callee copies
            } else {
                arg = "%v" + std::to_string(++n);
                t << "\t" << arg << " =l loadl " << addr << "\n";
            }
            if (!args.empty()) args += ", ";
            args += std::string(1, qbeType(pt)) + " " + arg;
            if (typeHasRc(pt)) rcParams.push_back({off, &pt});
            off += sizeOf(pt);
        }
        t << "\tcall $" << f->linkName << "(" << args << ")\n";
        for (auto& [o, pt] : rcParams) {
            std::string addr = "%pk";
            if (o != 0) {
                addr = "%r" + std::to_string(++n);
                t << "\t" << addr << " =l add %pk, " << o << "\n";
            }
            if (isRcScalar(*pt)) {
                std::string v = "%r" + std::to_string(++n);
                t << "\t" << v << " =l loadl " << addr << "\n";
                if (isStr(*pt)) {
                    needRC_ = true;
                    t << "\tcall $simple_release(l " << v << ")\n";
                } else if (isErr(*pt)) {
                    needErr_ = true;
                    t << "\tcall $simple_err_release(l " << v << ")\n";
                } else if (isList(*pt)) {
                    needList_ = true;
                    std::string df = typeHasRc(*pt->elem) ? listElemDtor(*pt) : "0";
                    t << "\tcall $simple_list_release(l " << v << ", l " << df << ")\n";
                } else if (isMap(*pt)) {
                    needMap_ = true;
                    std::string df = typeHasRc(*pt->elem) ? mapValDtor(*pt) : "0";
                    t << "\tcall $simple_map_release(l " << v << ", l " << df << ")\n";
                } else {
                    needChan_ = true;
                    t << "\tcall $simple_chan_release(l " << v << ")\n";
                }
            } else {
                t << "\tcall " << rcHelper(*pt, 'd') << "(l " << addr << ")\n";
            }
        }
        t << "\tcall $free(l %pk)\n\tret 0\n}\n";
        rcFuncs_.push_back(t.str());
        return name;
    }

    // release owned strings in scopes [downTo, end), skipping one slot
    void emitUnwind(size_t downTo, const std::string& skipAddr) {
        for (size_t si = scopes_.size(); si-- > downTo;) {
            for (auto& kv : scopes_[si]) {
                Slot& sl = kv.second;
                if (!sl.ownsRefs || sl.addr == skipAddr) continue;
                emitReleaseSlot(sl);
            }
        }
    }

    std::string strLabel(const std::string& bytes) {
        auto it = strPool_.find(bytes);
        if (it != strPool_.end()) return it->second;
        std::string label = "$str_" + std::to_string(strPool_.size());
        strPool_[bytes] = label;
        std::ostringstream d;
        // 16-byte header: immortal refcount (-1), then length
        d << "data " << label << " = { l -1, l " << bytes.size() << ", ";
        bool inStr = false;
        for (unsigned char c : bytes) {
            if (c >= 32 && c < 127 && c != '"' && c != '\\') {
                if (!inStr) { d << "b \""; inStr = true; }
                d << c;
            } else {
                if (inStr) { d << "\", "; inStr = false; }
                d << "b " << (int)c << ", ";
            }
        }
        if (inStr) d << "\", ";
        d << "b 0 }";
        strData_.push_back(d.str());
        return label;
    }

    // ---- functions ----

    // Is this variable name ever the target of an assignment in the body?
    static bool assignsTo(const std::vector<StmtPtr>& body, const std::string& name) {
        for (auto& s : body) {
            if (s->kind == StmtKind::Assign && s->lhs) {
                const Expr* root = s->lhs.get();
                while (root->kind == ExprKind::Field || root->kind == ExprKind::Index)
                    root = root->lhs.get();
                if (root->kind == ExprKind::Var && root->str == name) return true;
            }
            if (assignsTo(s->body, name) || assignsTo(s->elseBody, name)) return true;
        }
        return false;
    }

    void genFunction(Function& f) {
        cur_ = &f;
        blocks_.clear();
        blocks_.push_back(MBlock{"@start", {}});
        allocs_.clear();
        scopes_.clear();
        loops_.clear();
        stmtTemps_.clear();
        retOutSlot_.clear();
        tmpN_ = lblN_ = slotN_ = 0;
        terminated_ = false;

        // multi-return functions use the same hidden out-pointer as
        // aggregate returns: the caller passes a buffer, we fill the slots
        bool aggRet = isAggregate(f.ret) || f.ret.kind == TypeKind::Multi;
        std::ostringstream sig;
        sig << "export function ";
        if (f.name == "main") sig << "w ";
        else if (!aggRet && f.ret.kind != TypeKind::Void) sig << qbeType(f.ret) << " ";
        sig << "$" << f.linkName << "(";
        bool first = true;
        if (aggRet) {
            sig << "l %out";
            first = false;
        }
        if (f.name == "main") { // C entry: capture argc/argv for the IO builtins
            sig << "w %argc, l %argv";
            first = false;
        }
        for (size_t i = 0; i < f.params.size(); i++) {
            if (!first) sig << ", ";
            first = false;
            if (isAggregate(f.params[i].type)) sig << "l %p" << i;
            else sig << qbeType(f.params[i].type) << " %p" << i;
        }
        sig << ") {";

        ranges_.clear();
        addrTaken_.clear();
        collectAddrTaken(f.body, addrTaken_);

        pushScope(); // parameters
        if (aggRet) {
            retOutSlot_ = hiddenSlot("retout", 8);
            emit("storel %out, " + retOutSlot_);
        }
        if (f.name == "main") {
            std::string ac = newTmp();
            emit(ac + " =l extsw %argc");
            emit("storel " + ac + ", $simple_argc");
            emit("storel %argv, $simple_argv");
        }
        for (size_t i = 0; i < f.params.size(); i++) {
            const Param& p = f.params[i];
            // simple(v0.55): an aggregate parameter the body never assigns
            // to needs no copy at all — the caller's storage can be read
            // directly. Sound only because Simple has no aliasing: nothing
            // else can reach that storage to change it during the call.
            // (Same borrowing rule already used for str/chan params.)
            if (isAggregate(p.type) && !assignsTo(f.body, p.name)) {
                scopes_.back()[p.name] = {"%p" + std::to_string(i), p.type, false};
                continue;
            }
            // str/chan params are borrowed (caller pins them): no retain, no
            // release. A LIST is different — it has value semantics and can be
            // mutated (push COWs), so the callee must hold its own reference,
            // or a mutation would leak back to the caller.
            bool owns = !isRcScalar(p.type) || isList(p.type) || isMap(p.type);
            std::string slot = addSlot(p.name, p.type, owns);
            if (isList(p.type) || isMap(p.type)) {
                storeScalar(slot, p.type, "%p" + std::to_string(i));
                emitRetainValue("%p" + std::to_string(i), p.type);   // own a reference
            } else if (isAggregate(p.type)) {
                emitCopy(slot, "%p" + std::to_string(i), sizeOf(p.type));
                if (typeHasRc(p.type)) // the copy shares rc pointers
                    emit("call " + rcHelper(p.type, 'r') + "(l " + slot + ")");
            } else {
                // width-correct: a u8 parameter must be stored with storeb,
                // not storel (the incoming value is in a w register)
                storeScalar(slot, p.type, "%p" + std::to_string(i));
            }
        }
        pushScope(); // body
        genStmts(f.body);

        if (!terminated_) {
            emitUnwind(0, "");
            if (f.name == "main") emit("ret 0");
            else if (aggRet || f.ret.kind == TypeKind::Void) emit("ret");
            else emit("ret 0"); // unreachable: sema proved every path returns
        }

        MFunc mf;
        mf.sig = sig.str();
        mf.name = f.linkName; // symbol-level identity (v0.9)
        mf.fn = &f;
        mf.aggRet = aggRet;
        mf.allocs = std::move(allocs_);
        mf.blocks = std::move(blocks_);
        mfuncs_.push_back(std::move(mf));
    }

    std::string printFunc(const MFunc& f) {
        std::ostringstream out;
        std::string sig = f.sig;
        // optimized builds export only main: internal symbols stay internal
        if (optimize_ && f.name != "main" && sig.rfind("export ", 0) == 0)
            sig = sig.substr(7);
        out << sig << "\n@start\n";
        for (auto& a : f.allocs) out << a << "\n";
        for (size_t i = 0; i < f.blocks.size(); i++) {
            if (i > 0) out << f.blocks[i].label << "\n";
            for (auto& m : f.blocks[i].ins) out << "\t" << m.text << "\n";
        }
        out << "}\n";
        return out.str();
    }

    void genStmts(std::vector<StmtPtr>& stmts) {
        for (auto& s : stmts) {
            genStmt(*s);
            if (!terminated_) flushTemps(); // release unconsumed statement temps
            if (terminated_) break;
        }
    }

    void genStmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::Let: {
            // let (a, b) = f();  — the call fills a buffer; every reference
            // in it transfers to the new slots (or is released for `_`)
            if (!s.names.empty()) {
                std::vector<Type> rets = builtinRets(s.expr->str);
                if (rets.empty()) rets = fns_[s.expr->str]->rets;
                std::string buf = genExpr(*s.expr);
                for (size_t i = 0; i < s.names.size(); i++) {
                    const Type& rt = rets[i];
                    long off = multiOffset(rets, i);
                    std::string addr = buf;
                    if (off != 0) {
                        addr = newTmp();
                        emit(addr + " =l add " + buf + ", " + std::to_string(off));
                    }
                    if (s.names[i] == "_") { // discarded: drop its references
                        if (isRcScalar(rt)) {
                            std::string v = newTmp();
                            emit(v + " =l loadl " + addr);
                            emitReleaseValue(v, rt);
                        } else if (typeHasRc(rt)) {
                            emit("call " + rcHelper(rt, 'd') + "(l " + addr + ")");
                        }
                        continue;
                    }
                    std::string slot = addSlot(s.names[i], rt);
                    if (isAggregate(rt)) {
                        emitCopy(slot, addr, sizeOf(rt));
                    } else {
                        std::string v = loadScalar(addr, rt);
                        storeScalar(slot, rt, v);
                    }
                    ranges_.erase(s.names[i]);
                }
                break;
            }
            // an aggregate literal can be built directly in the new slot
            bool inPlace = isAggregate(s.expr->type) &&
                           (s.expr->kind == ExprKind::ArrayLit ||
                            s.expr->kind == ExprKind::StructLit);
            if (inPlace) {
                std::string slot = addSlot(s.name, s.expr->type);
                destHint_ = slot;
                genExpr(*s.expr);
                destHint_.clear();
                break;
            }
            Range r = isInt(s.expr->type) ? rangeOf(*s.expr) : Range{};
            std::string v = genExpr(*s.expr);
            consumeOwned(*s.expr, v);
            std::string slot = addSlot(s.name, s.expr->type);
            storeToSlot({slot, s.expr->type, true}, v);
            if (isInt(s.expr->type)) ranges_[s.name] = r;
            else ranges_.erase(s.name);
            break;
        }
        case StmtKind::Assign: {
            // m[k] = v: COW the map, then let the runtime find-or-insert the
            // slot (it releases an overwritten value and zeroes a new one),
            // then store the value into the returned slot.
            if (s.lhs->kind == ExprKind::Index &&
                s.lhs->lhs->kind == ExprKind::Var &&
                s.lhs->lhs->type.kind == TypeKind::Map) {
                needMap_ = true;
                const Type& mapT = s.lhs->lhs->type;
                const Type& elemT = *mapT.elem;
                Slot& sl = findSlot(s.lhs->lhs->str);
                std::string vr = typeHasRc(elemT) ? mapValRetain(mapT) : "0";
                std::string vd = typeHasRc(elemT) ? mapValDtor(mapT) : "0";
                std::string h = newTmp();
                emit(h + " =l loadl " + sl.addr);
                std::string h2 = newTmp();
                emit(h2 + " =l call $simple_map_unique(l " + h + ", l " + vr + ")");
                emit("storel " + h2 + ", " + sl.addr);
                std::string key = genExpr(*s.lhs->rhs);
                std::string v = genExpr(*s.expr);
                consumeOwned(*s.expr, v);
                // an aggregate value lives at an address that may point into
                // this very map's entries (m[a] = m[b]) — and put can grow
                // (realloc) the entries. Copy it to a scratch slot first.
                if (isAggregate(elemT)) {
                    std::string tmp = hiddenSlot("mval", sizeOf(elemT));
                    emitCopy(tmp, v, sizeOf(elemT));
                    v = tmp;
                }
                std::string slotp = newTmp();
                emit(slotp + " =l call $simple_map_put(l " + h2 + ", l " + key +
                     ", l " + vd + ")");
                if (isAggregate(elemT)) emitCopy(slotp, v, sizeOf(elemT));
                else storeInterior(slotp, elemT, v);
                break;
            }
            // writing an element of a list mutates it: make it unique (COW)
            // before computing the target address
            if (s.lhs->kind == ExprKind::Index &&
                s.lhs->lhs->kind == ExprKind::Var &&
                s.lhs->lhs->type.kind == TypeKind::List) {
                const Type& listT = s.lhs->lhs->type;
                const Type& elemT = *listT.elem;
                Slot& sl = findSlot(s.lhs->lhs->str);
                std::string rf = typeHasRc(elemT) ? listElemRetain(listT) : "0";
                std::string df = typeHasRc(elemT) ? listElemDtor(listT) : "0";
                std::string h = newTmp();
                emit(h + " =l loadl " + sl.addr);
                std::string h2 = newTmp();
                emit(h2 + " =l call $simple_list_unique(l " + h + ", l " + rf +
                     ", l " + df + ")");
                emit("storel " + h2 + ", " + sl.addr);
            }
            Range newR = isInt(s.expr->type) ? rangeOf(*s.expr) : Range{};
            std::string v = genExpr(*s.expr);
            consumeOwned(*s.expr, v); // retain new before releasing old (self-assign safe)
            if (s.lhs->kind == ExprKind::Var) {
                if (isInt(s.lhs->type)) ranges_[s.lhs->str] = newR;
                else ranges_.erase(s.lhs->str);
            }
            if (s.lhs->kind == ExprKind::Var) {
                Slot& sl = findSlot(s.lhs->str);
                if (isRcScalar(sl.type)) {
                    std::string old = newTmp();
                    emit(old + " =l loadl " + sl.addr);
                    emitReleaseValue(old, sl.type);
                } else if (typeHasRc(sl.type)) {
                    emit("call " + rcHelper(sl.type, 'd') + "(l " + sl.addr + ")");
                }
                storeToSlot(sl, v);
            } else {
                std::string addr = genPlace(*s.lhs);
                const Type& lt = s.lhs->type;
                if (isRcScalar(lt)) {
                    std::string old = newTmp();
                    emit(old + " =l loadl " + addr);
                    emitReleaseValue(old, lt);
                    emit("storel " + v + ", " + addr);
                } else if (typeHasRc(lt)) {
                    emit("call " + rcHelper(lt, 'd') + "(l " + addr + ")");
                    emitCopy(addr, v, sizeOf(lt));
                } else {
                    storeInterior(addr, lt, v);
                }
            }
            break;
        }
        case StmtKind::ExprStmt:
            genExpr(*s.expr);
            break;
        case StmtKind::Return: {
            // return a, b;  — write each value into its slot of the caller's
            // buffer. Named locals are retained here and released by the
            // unwind (no skip-slot: several values may name several locals).
            if (cur_->ret.kind == TypeKind::Multi) {
                std::string o = newTmp();
                emit(o + " =l loadl " + retOutSlot_);
                for (size_t i = 0; i < s.exprs.size(); i++) {
                    Expr& ex = *s.exprs[i];
                    const Type& rt = cur_->rets[i];
                    std::string v = genExpr(ex);
                    if (typeHasRc(rt)) consumeOwned(ex, v);
                    long off = multiOffset(*cur_, i);
                    std::string addr = o;
                    if (off != 0) {
                        addr = newTmp();
                        emit(addr + " =l add " + o + ", " + std::to_string(off));
                    }
                    if (isAggregate(rt)) emitCopy(addr, v, sizeOf(rt));
                    else storeInterior(addr, rt, v);
                }
                flushTemps();
                emitUnwind(0, "");
                emit("ret");
                terminated_ = true;
                break;
            }
            // simple(v0.6): `return <aggregate literal>` builds straight
            // into the caller's return slot. Safe unconditionally because
            // that slot is always a fresh temporary in the caller, so it
            // cannot alias anything the literal reads.
            if (s.expr && !retOutSlot_.empty() &&
                (s.expr->kind == ExprKind::StructLit ||
                 s.expr->kind == ExprKind::ArrayLit)) {
                std::string o = newTmp();
                emit(o + " =l loadl " + retOutSlot_);
                destHint_ = o;
                genExpr(*s.expr);
                destHint_.clear();
                flushTemps();
                emitUnwind(0, "");
                emit("ret");
                terminated_ = true;
                break;
            }
            if (s.expr) {
                std::string v = genExpr(*s.expr);
                std::string skip = "";
                if (typeHasRc(s.expr->type)) {
                    // returning a local by name: transfer instead of retain+release
                    if (s.expr->kind == ExprKind::Var) {
                        Slot& sl = findSlot(s.expr->str);
                        if (sl.ownsRefs) skip = sl.addr;
                        else emitRetainValue(v, s.expr->type); // borrowed str param
                    } else {
                        consumeOwned(*s.expr, v);
                    }
                }
                if (!retOutSlot_.empty()) {
                    std::string o = newTmp();
                    emit(o + " =l loadl " + retOutSlot_);
                    emitCopy(o, v, sizeOf(cur_->ret));
                    flushTemps();
                    emitUnwind(0, skip);
                    emit("ret");
                } else {
                    flushTemps();
                    emitUnwind(0, skip);
                    emit("ret " + v);
                }
            } else {
                flushTemps();
                emitUnwind(0, "");
                if (cur_->name == "main") emit("ret 0");
                else emit("ret");
            }
            terminated_ = true;
            break;
        }
        case StmtKind::If: {
            std::string c = genExpr(*s.expr);
            flushTemps();
            std::string thenL = newLbl("if_then");
            std::string endL = newLbl("if_end");
            std::string elseL = s.elseBody.empty() ? endL : newLbl("if_else");
            emit("jnz " + c + ", " + thenL + ", " + elseL);
            terminated_ = true;
            placeLabel(thenL);
            pushScope();
            auto savedRanges = ranges_;
            refine(*s.expr, true); // the condition holds in this branch
            genStmts(s.body);
            if (!terminated_) emitUnwind(scopes_.size() - 1, "");
            popScope();
            ranges_ = savedRanges;
            if (!terminated_) { emit("jmp " + endL); terminated_ = true; }
            if (!s.elseBody.empty()) {
                placeLabel(elseL);
                pushScope();
                refine(*s.expr, false);
                genStmts(s.elseBody);
                if (!terminated_) emitUnwind(scopes_.size() - 1, "");
                popScope();
                ranges_ = savedRanges;
                if (!terminated_) { emit("jmp " + endL); terminated_ = true; }
            }
            placeLabel(endL);
            // whatever either branch assigned is no longer known
            forgetAssigned(s.body);
            forgetAssigned(s.elseBody);
            break;
        }
        case StmtKind::While: {
            // idiom: bit-clearing count loop is a population count
            std::string xv, nv;
            if (optimize_ && matchPopcount(s, xv, nv)) {
                Slot& xs = findSlot(xv);
                Slot& ns = findSlot(nv);
                if (isInt(xs.type) && xs.type.bits == 64 && isInt(ns.type)) {
                    std::string x = loadScalar(xs.addr, xs.type);
                    std::string p = emitPopcount(x);
                    std::string n = loadScalar(ns.addr, ns.type);
                    std::string sum = newTmp();
                    emit(sum + " =" + std::string(1, qbeType(ns.type)) + " add " + n +
                         ", " + p);
                    storeScalar(ns.addr, ns.type, sum);
                    storeScalar(xs.addr, xs.type, "0"); // the loop ends at zero
                    ranges_.erase(xv);
                    ranges_.erase(nv);
                    break;
                }
            }
            std::string condL = newLbl("while_cond");
            std::string bodyL = newLbl("while_body");
            std::string endL = newLbl("while_end");
            // loop-carried variables are unknown on entry; the condition
            // then re-establishes whatever it guarantees
            auto savedRanges = ranges_;
            widenForLoop(s.body);
            placeLabel(condL);
            std::string c = genExpr(*s.expr);
            flushTemps();
            emit("jnz " + c + ", " + bodyL + ", " + endL);
            terminated_ = true;
            placeLabel(bodyL);
            loops_.push_back({condL, endL, scopes_.size()});
            pushScope();
            refine(*s.expr, true);
            genStmts(s.body);
            if (!terminated_) emitUnwind(scopes_.size() - 1, "");
            popScope();
            loops_.pop_back();
            if (!terminated_) { emit("jmp " + condL); terminated_ = true; }
            placeLabel(endL);
            ranges_ = savedRanges;
            widenForLoop(s.body);   // the body ran: reflect its motion
            // A loop guarded by `x >= k` (possibly the first half of an
            // `&&`) that only steps x down by at most `c` must leave x no
            // lower than k - c: it could not have taken another step
            // without the guard holding first. That single fact is what
            // makes `a[j + 1]` provable after an insertion-sort inner loop.
            {
                const Expr* g = s.expr.get();
                while (g && g->kind == ExprKind::Binary && g->op == Tok::AndAnd)
                    g = g->lhs.get();
                if (g && g->kind == ExprKind::Binary &&
                    (g->op == Tok::Ge || g->op == Tok::Gt) && g->lhs && g->rhs &&
                    g->lhs->kind == ExprKind::Var &&
                    !addrTaken_.count(g->lhs->str) &&
                    motionOf(s.body, g->lhs->str) == Motion::Down) {
                    long long step = maxDownStep(s.body, g->lhs->str);
                    Range b = rangeOf(*g->rhs);
                    if (step > 0 && b.lo > RLO) {
                        long long floor = b.lo + (g->op == Tok::Gt ? 1 : 0) - step;
                        Range& r = ranges_[g->lhs->str];
                        r.lo = std::max(r.lo, floor);
                    }
                }
            }
            break;
        }
        case StmtKind::For: {
            // for (k in m): walk the entries array in insertion order —
            // deterministic everywhere. The loop holds its own reference
            // (snapshot): if the body mutates m, COW copies it away and the
            // iteration keeps seeing the map as it was when the loop began.
            if (!s.expr2) {
                needMap_ = true;
                const Type mapT = s.expr->type;
                const Type keyT = *mapT.key;
                std::string mv = genExpr(*s.expr);
                consumeOwned(*s.expr, mv); // own the snapshot reference
                flushTemps();
                pushScope(); // snapshot scope ("." keeps the name unwritable)
                std::string snap = addSlot("for.map", mapT, true);
                emit("storel " + mv + ", " + snap);
                // geometry is frozen for the whole loop: load it once
                std::string nep = newTmp(), ne = newTmp();
                emit(nep + " =l add " + mv + ", 16");
                emit(ne + " =l loadl " + nep);
                std::string entp = newTmp(), ent = newTmp();
                emit(entp + " =l add " + mv + ", 32");
                emit(ent + " =l loadl " + entp);
                long stride = 16 + alignUp(sizeOf(*mapT.elem), 8);
                std::string iSlot = hiddenSlot("fmidx", 8);
                emit("storel 0, " + iSlot);
                pushScope(); // loop variable + body scope
                std::string kSlot = addSlot(s.name, keyT, false); // borrowed key
                auto savedRanges = ranges_;
                widenForLoop(s.body);
                ranges_.erase(s.name);
                std::string condL = newLbl("fm_cond");
                std::string chkL = newLbl("fm_chk");
                std::string bodyL = newLbl("fm_body");
                std::string incrL = newLbl("fm_incr");
                std::string endL = newLbl("fm_end");
                placeLabel(condL);
                std::string iv = newTmp();
                emit(iv + " =l loadl " + iSlot);
                std::string c = newTmp();
                emit(c + " =w csltl " + iv + ", " + ne);
                emit("jnz " + c + ", " + chkL + ", " + endL);
                terminated_ = true;
                placeLabel(chkL); // skip entries deleted before the loop
                std::string eoff = newTmp(), ep = newTmp();
                emit(eoff + " =l mul " + iv + ", " + std::to_string(stride));
                emit(ep + " =l add " + ent + ", " + eoff);
                std::string st = newTmp();
                emit(st + " =l loadl " + ep);
                emit("jnz " + st + ", " + bodyL + ", " + incrL);
                terminated_ = true;
                placeLabel(bodyL);
                std::string kp = newTmp(), kv = newTmp();
                emit(kp + " =l add " + ep + ", 8");
                emit(kv + " =l loadl " + kp);
                storeScalar(kSlot, keyT, kv);
                loops_.push_back({incrL, endL, scopes_.size() - 1});
                genStmts(s.body);
                if (!terminated_) emitUnwind(scopes_.size() - 1, kSlot);
                loops_.pop_back();
                placeLabel(incrL);
                std::string i2 = newTmp(), i3 = newTmp();
                emit(i2 + " =l loadl " + iSlot);
                emit(i3 + " =l add " + i2 + ", 1");
                emit("storel " + i3 + ", " + iSlot);
                emit("jmp " + condL);
                terminated_ = true;
                placeLabel(endL);
                popScope(); // loop var scope (borrowed key: nothing to release)
                emitUnwind(scopes_.size() - 1, ""); // release the snapshot
                popScope();
                ranges_ = savedRanges;
                break;
            }
            // idiom: constant fill over a whole array is a memset
            std::string farr;
            long long fbyte;
            long long flo, fhi;
            if (optimize_ && matchFill(s, farr, fbyte) && constOf(*s.expr, flo) &&
                constOf(*s.expr2, fhi) && flo >= 0 && fhi >= flo) {
                Slot& as = findSlot(farr);
                if (as.type.kind == TypeKind::Array && fhi <= as.type.alen) {
                    long esz = sizeOf(*as.type.elem);
                    std::string base = as.addr;
                    if (flo != 0) {
                        base = newTmp();
                        emit(base + " =l add " + as.addr + ", " +
                             std::to_string(flo * esz));
                    }
                    emit("call $memset(l " + base + ", w " + std::to_string(fbyte) +
                         ", l " + std::to_string((fhi - flo) * esz) + ")");
                    break;
                }
            }
            // v0.95: a proven-safe element-wise float loop becomes 2-lane
            // NEON (with a scalar remainder). Guarded by the opt/no-opt
            // differential — element-wise SIMD is bit-identical to scalar.
            {
                long long vhi;
                if (vecEligible(s, vhi)) {
                    emitVecLoop(s);
                    break;
                }
                if (reducEligible(s, vhi)) {
                    emitReducLoop(s);
                    break;
                }
            }
            // NOTE: full unrolling of short constant-trip-count loops was
            // implemented and MEASURED, then removed. Nested small loops
            // (nbody's 8x8) compounded into 64 inlined bodies and thrashed
            // the instruction cache: nbody 0.22 -> 0.36 s, sieve 0.04 ->
            // 0.07, sortint 0.05 -> 0.06, with no benchmark improved.
            // Out-of-order cores and branch predictors have made small-loop
            // unrolling much less valuable than it once was.
            std::string start = genExpr(*s.expr);
            std::string end = genExpr(*s.expr2);
            flushTemps();
            std::string endSlot = hiddenSlot("fend", 8);
            emit("storel " + end + ", " + endSlot);
            pushScope();
            std::string iSlot = addSlot(s.name, intType());
            emit("storel " + start + ", " + iSlot);
            // The loop variable is immutable and its bounds were evaluated
            // once, above — so its range is exactly [lo, hi-1]. Anything the
            // body reassigns is forgotten first (it may carry across
            // iterations).
            auto savedRanges = ranges_;
            widenForLoop(s.body);
            Range lor = rangeOf(*s.expr), hir = rangeOf(*s.expr2);
            ranges_[s.name] = {lor.lo, addClamp(hir.hi, -1)};
            std::string condL = newLbl("for_cond");
            std::string bodyL = newLbl("for_body");
            std::string incrL = newLbl("for_incr");
            std::string endL = newLbl("for_end");
            placeLabel(condL);
            std::string iv = newTmp();
            emit(iv + " =l loadl " + iSlot);
            std::string ev = newTmp();
            emit(ev + " =l loadl " + endSlot);
            std::string c = newTmp();
            emit(c + " =w csltl " + iv + ", " + ev);
            emit("jnz " + c + ", " + bodyL + ", " + endL);
            terminated_ = true;
            placeLabel(bodyL);
            loops_.push_back({incrL, endL, scopes_.size() - 1});
            genStmts(s.body);
            if (!terminated_) emitUnwind(scopes_.size() - 1, iSlot);
            loops_.pop_back();
            placeLabel(incrL);
            std::string i2 = newTmp();
            emit(i2 + " =l loadl " + iSlot);
            std::string i3 = newTmp();
            emit(i3 + " =l add " + i2 + ", 1");
            emit("storel " + i3 + ", " + iSlot);
            emit("jmp " + condL);
            terminated_ = true;
            placeLabel(endL);
            popScope();
            ranges_ = savedRanges;
            break;
        }
        case StmtKind::Break:
            emitUnwind(loops_.back().depth, "");
            emit("jmp " + loops_.back().brk);
            terminated_ = true;
            break;
        case StmtKind::Continue:
            emitUnwind(loops_.back().depth, "");
            emit("jmp " + loops_.back().cont);
            terminated_ = true;
            break;
        case StmtKind::Spawn: {
            Expr& call = *s.expr;
            Function* f = fns_[call.str];
            long psize = 0;
            for (auto& p : f->params) psize += sizeOf(p.type);
            if (psize == 0) psize = 8;
            std::string pk = newTmp();
            emit(pk + " =l call $malloc(l " + std::to_string(psize) + ")");
            long off = 0;
            for (size_t i = 0; i < call.args.size(); i++) {
                Expr& a = *call.args[i];
                std::string v = genExpr(a);
                std::string addr = pk;
                if (off != 0) {
                    addr = newTmp();
                    emit(addr + " =l add " + pk + ", " + std::to_string(off));
                }
                storeSendValue(a, v, f->params[i].type, addr);
                off += sizeOf(f->params[i].type);
            }
            std::string tramp = spawnTramp(f);
            std::string tid = hiddenSlot("tid", 8);
            emit("call $pthread_create(l " + tid + ", l 0, l " + tramp + ", l " + pk + ")");
            std::string tv = newTmp();
            emit(tv + " =l loadl " + tid);
            emit("call $pthread_detach(l " + tv + ")");
            break;
        }
        case StmtKind::Block:
        case StmtKind::Unsafe:
            pushScope();
            genStmts(s.body);
            if (!terminated_) emitUnwind(scopes_.size() - 1, "");
            popScope();
            break;
        }
    }

    // ---- loop-idiom recognition ----
    //
    // Some loops are really a single machine operation wearing a disguise.
    // Matching them on the AST (rather than rediscovering the shape from a
    // lowered CFG, as LLVM's LoopIdiomRecognize must) is cheap here because
    // `for` carries its trip count and loop variables are immutable.

    static bool isVar(const Expr* e, const std::string& n) {
        return e && e->kind == ExprKind::Var && e->str == n;
    }
    static bool isBin(const Expr* e, Tok op) {
        return e && e->kind == ExprKind::Binary && e->op == op && e->lhs && e->rhs;
    }
    static bool isConstVal(const Expr* e, long long v) {
        long long c;
        return e && constOf(*e, c) && c == v;
    }

    // while (x != 0) { x = x & (x - 1); n = n + 1; }
    //   ==  n = n + popcount(x);  x = 0;
    // (either statement order)
    static bool matchPopcount(const Stmt& s, std::string& xv, std::string& nv) {
        if (!isBin(s.expr.get(), Tok::NotEq)) return false;
        const Expr* c = s.expr.get();
        if (c->lhs->kind != ExprKind::Var || !isConstVal(c->rhs.get(), 0)) return false;
        xv = c->lhs->str;
        if (s.body.size() != 2) return false;
        bool sawClear = false, sawCount = false;
        for (auto& st : s.body) {
            if (st->kind != StmtKind::Assign || !st->lhs ||
                st->lhs->kind != ExprKind::Var)
                return false;
            const Expr* r = st->expr.get();
            if (st->lhs->str == xv) {
                // x = x & (x - 1)
                if (!isBin(r, Tok::Amp) || !isVar(r->lhs.get(), xv)) return false;
                if (!isBin(r->rhs.get(), Tok::Minus) ||
                    !isVar(r->rhs->lhs.get(), xv) || !isConstVal(r->rhs->rhs.get(), 1))
                    return false;
                sawClear = true;
            } else {
                // n = n + 1
                if (!isBin(r, Tok::Plus)) return false;
                if (!isVar(r->lhs.get(), st->lhs->str) || !isConstVal(r->rhs.get(), 1))
                    return false;
                nv = st->lhs->str;
                sawCount = true;
            }
        }
        return sawClear && sawCount && xv != nv;
    }

    // Emit a branch-free 64-bit population count (SWAR): ~12 instructions
    // in place of one iteration per set bit.
    std::string emitPopcount(const std::string& x) {
        struct { const char* op; const char* c; } steps[] = {};
        (void)steps;
        auto bin = [&](const char* op, const std::string& a, const std::string& b) {
            std::string t = newTmp();
            emit(t + " =l " + op + " " + a + ", " + b);
            return t;
        };
        std::string t1 = bin("shr", x, "1");
        std::string t2 = bin("and", t1, "6148914691236517205");   // 0x5555...
        std::string t3 = bin("sub", x, t2);
        std::string t4 = bin("and", t3, "3689348814741910323");   // 0x3333...
        std::string t5 = bin("shr", t3, "2");
        std::string t6 = bin("and", t5, "3689348814741910323");
        std::string t7 = bin("add", t4, t6);
        std::string t8 = bin("shr", t7, "4");
        std::string t9 = bin("add", t7, t8);
        std::string t10 = bin("and", t9, "1085102592571150095");  // 0x0F0F...
        std::string t11 = bin("mul", t10, "72340172838076673");   // 0x0101...
        return bin("shr", t11, "56");
    }

    // for (i in lo..hi) { a[i] = <constant>; }  ==  memset, when every byte
    // of the stored value is the same (any 1-byte element, or zero).
    bool matchFill(const Stmt& s, std::string& arr, long long& byteVal) {
        if (s.body.size() != 1) return false;
        const Stmt& b = *s.body[0];
        if (b.kind != StmtKind::Assign || !b.lhs) return false;
        if (b.lhs->kind != ExprKind::Index) return false;
        if (!isVar(b.lhs->lhs.get(), b.lhs->lhs->str)) return false;
        if (b.lhs->lhs->kind != ExprKind::Var) return false;
        if (!isVar(b.lhs->rhs.get(), s.name)) return false; // indexed by loop var
        const Type& at = b.lhs->lhs->type;
        if (at.kind != TypeKind::Array) return false;
        const Type& et = *at.elem;
        if (isAggregate(et)) return false;
        long long v;
        if (!constOf(*b.expr, v)) {
            if (b.expr->kind == ExprKind::BoolLit) v = b.expr->ival;
            else return false;
        }
        long esz = sizeOf(et);
        if (esz != 1 && v != 0) return false; // wider values aren't byte-uniform
        arr = b.lhs->lhs->str;
        byteVal = esz == 1 ? (v & 0xFF) : 0;
        return true;
    }

    // ---- bounds-check elimination via integer range analysis ----
    //
    // Every array's length is part of its type, so a bounds check is dead
    // whenever the index provably lies in [0, len). We get the index range
    // from three language guarantees: `for` variables are immutable with
    // bounds evaluated once; no function call can modify a local (there is
    // no aliasing in safe code); and a loop or `if` condition holds inside
    // the code it guards. Loop-carried variables are widened to unknown
    // before their body is analysed, then re-narrowed by the guard — so a
    // cursor like `while (j >= 0 && ...) { ... j = j - 1; }` keeps a usable
    // lower bound without any fixpoint iteration.

    static const long long RLO = -(1LL << 62), RHI = (1LL << 62);
    struct Range {
        long long lo = RLO, hi = RHI;
        bool bounded() const { return lo > RLO || hi < RHI; }
    };
    std::unordered_map<std::string, Range> ranges_;
    std::set<std::string> addrTaken_; // `&x` seen: the value can change unseen

    // ============ v0.95 milestone 5: emit vectorized loops ============
    // Conservative eligibility: 1D stack f64 arrays only, a constant upper
    // bound within every array's length (so a 2-lane load can't read past
    // the end — preserving the bounds guarantee), element-wise (no
    // reduction yet), no casts / calls / lists / unary. Anything else falls
    // through to the scalar loop. Correctness is guarded end-to-end by the
    // opt-vs-no-opt differential: element-wise SIMD is bit-identical to
    // scalar, so any bug (a dropped store, a spilled lane) shows as a diff.

    bool vecIndexOk(const Expr& e, const std::string& iv, long long hi) {
        if (e.kind != ExprKind::Index) return false;
        if (e.lhs->kind != ExprKind::Var) return false;          // 1D
        const Type& at = e.lhs->type;
        if (at.kind != TypeKind::Array || !at.elem) return false; // stack array
        if (at.elem->kind != TypeKind::Float || at.elem->bits != 64) return false;
        if ((long long)at.alen < hi) return false;               // no read past end
        return e.rhs->kind == ExprKind::Var && e.rhs->str == iv; // contiguous
    }
    bool vecExprOk(const Expr& e, const std::string& iv, long long hi) {
        switch (e.kind) {
        case ExprKind::FloatLit: return true;
        case ExprKind::Var:
            return e.str != iv && e.type.kind == TypeKind::Float && e.type.bits == 64;
        case ExprKind::Index: return vecIndexOk(e, iv, hi);
        case ExprKind::Binary:
            switch (e.op) {
            case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash:
                return vecExprOk(*e.lhs, iv, hi) && vecExprOk(*e.rhs, iv, hi);
            default: return false;
            }
        default: return false; // no cast/call/unary in the vector path
        }
    }
    // Count the vector values an expression produces (loads/broadcasts/ops);
    // a reference to an already-computed let temp is free. Used as a
    // conservative register-pressure bound so we never spill a vector (the
    // Kd-typed value would lose its high lane on an 8-byte spill). Loops that
    // would exceed the budget stay scalar — sound, just not accelerated.
    int countVecOps(const Expr& e, const std::set<std::string>& letTemps) {
        switch (e.kind) {
        case ExprKind::FloatLit: return 1;                 // vdup
        case ExprKind::Var: return letTemps.count(e.str) ? 0 : 1; // reuse vs vdup
        case ExprKind::Index: return 1;                    // vload
        case ExprKind::Binary:
            return 1 + countVecOps(*e.lhs, letTemps) + countVecOps(*e.rhs, letTemps);
        default: return 1;
        }
    }
    bool vecEligible(const Stmt& s, long long& hiOut) {
        if (!optimize_ || !s.expr2) return false;
        if (classifyLoop(s).kind != VecKind::ElementWise) return false;
        long long lo, hi;
        if (!constOf(*s.expr, lo) || !constOf(*s.expr2, hi)) return false;
        if (lo < 0 || hi < lo) return false;
        std::set<std::string> letTemps;
        int liveBudget = 0;      // conservative simultaneously-live vector bound
        for (const auto& stp : s.body) {
            const Stmt& st = *stp;
            const Expr* rhs;
            if (st.kind == StmtKind::Let) {
                if (!st.names.empty()) return false;
                rhs = st.expr.get();
                liveBudget++;              // the temp stays live across statements
                letTemps.insert(st.name);
            } else if (st.kind == StmtKind::Assign) {
                if (!vecIndexOk(*st.lhs, s.name, hi)) return false;
                rhs = st.expr.get();
            } else return false;
            if (!vecExprOk(*rhs, s.name, hi)) return false;
            int here = countVecOps(*rhs, letTemps);
            if (liveBudget + here > 24) return false; // stay well under 32 V regs
        }
        hiOut = hi;
        return true;
    }
    // &arr[i] for a 1D stack f64 array (i loaded from iSlot); no bounds check
    // — vecEligible proved i and i+1 are in range.
    std::string vecElemAddr(const Expr& idx, const std::string& iSlot) {
        Slot& sl = findSlot(idx.lhs->str);
        std::string iv = newTmp();  emit(iv + " =l loadl " + iSlot);
        std::string off = newTmp(); emit(off + " =l mul " + iv + ", 8");
        std::string a = newTmp();   emit(a + " =l add " + sl.addr + ", " + off);
        if (vecLaneOff_) {         // unrolled lane group: &arr[i + vecLaneOff_]
            std::string a2 = newTmp();
            emit(a2 + " =l add " + a + ", " + std::to_string(vecLaneOff_ * 8));
            return a2;
        }
        return a;
    }
    // lower a lane-local expr to a 2-lane vector (cls d)
    std::string genVecExpr(Expr& e, const std::string& iSlot,
                           std::map<std::string, std::string>& vtemps) {
        std::string t = newTmp();
        switch (e.kind) {
        case ExprKind::FloatLit:
            emit(t + " =d vdup " + genExpr(e));    // broadcast a constant
            return t;
        case ExprKind::Var: {
            auto it = vtemps.find(e.str);
            if (it != vtemps.end()) return it->second; // a per-lane temp
            emit(t + " =d vdup " + genExpr(e));    // broadcast an invariant scalar
            return t;
        }
        case ExprKind::Index:
            emit(t + " =d vload " + vecElemAddr(e, iSlot));
            return t;
        case ExprKind::Binary: {
            std::string a = genVecExpr(*e.lhs, iSlot, vtemps);
            std::string b = genVecExpr(*e.rhs, iSlot, vtemps);
            const char* op = e.op == Tok::Plus ? "vfadd" : e.op == Tok::Minus ? "vfsub"
                           : e.op == Tok::Star ? "vfmul" : "vfdiv";
            emit(t + " =d " + op + " " + a + ", " + b);
            return t;
        }
        default:
            return "d_0"; // unreachable: vecExprOk gated the shapes
        }
    }
    // A single-statement f64 reduction `acc = acc (+|*) <lane-local>` over
    // 1D stack arrays with a constant in-range bound. The accumulated value
    // must be lane-local (checked). Reassociating into lanes is sound by the
    // v0.95 float-reduction decision; cross-arch identity is the guarantee.
    bool reducEligible(const Stmt& s, long long& hiOut) {
        if (!optimize_ || !s.expr2) return false;
        if (classifyLoop(s).kind != VecKind::Reduction) return false;
        if (s.body.size() != 1) return false;                 // one reduction only
        const Stmt& st = *s.body[0];
        if (st.kind != StmtKind::Assign || st.lhs->kind != ExprKind::Var) return false;
        if (st.lhs->type.kind != TypeKind::Float || st.lhs->type.bits != 64) return false;
        const Expr& rhs = *st.expr;
        if (rhs.kind != ExprKind::Binary ||
            (rhs.op != Tok::Plus && rhs.op != Tok::Star)) return false;
        long long lo, hi;
        if (!constOf(*s.expr, lo) || !constOf(*s.expr2, hi)) return false;
        if (lo < 0 || hi < lo) return false;
        // every leaf: the accumulator, a lane-local read, or an invariant
        if (!reducRhsOk(rhs, s.name, st.lhs->str, hi)) return false;
        hiOut = hi;
        return true;
    }
    bool reducRhsOk(const Expr& e, const std::string& iv,
                    const std::string& acc, long long hi) {
        if (e.kind == ExprKind::Var && e.str == acc) return true; // the accumulator
        if (e.kind == ExprKind::Binary)
            return reducRhsOk(*e.lhs, iv, acc, hi) && reducRhsOk(*e.rhs, iv, acc, hi);
        return vecExprOk(e, iv, hi);
    }
    void emitReducLoop(Stmt& s) {
        // UNROLL independent 2-lane accumulators break the reduction's
        // dependency chain (the loop is latency-bound on a single accumulator:
        // each add waits on the previous). The accumulators are combined in a
        // fixed order after the loop, so the reduction tree — hence the result
        // — is identical on every architecture. This is the real lever behind
        // a vectorizing C compiler's dot-product speed (FMA is not).
        //
        // The accumulators live in MEMORY (vload/vstore each iteration), not in
        // registers via phi. A register-resident phi was tried and is unsafe:
        // when QBE cannot coalesce a phi it resolves it with a copy, and a copy
        // of a `Kd`-typed vector is a 64-bit move that drops the high lane —
        // silently wrong whenever coalescing fails (e.g. amd64, or the product
        // reduction). Register-residency needs the real 128-bit vector class,
        // deferred as high-risk. Memory keeps every vector inside one block, so
        // QBE never copies or spills it. Cost: ~0.10s vs ~0.06s for C's best.
        const int UNROLL = 4;
        const int STEP = UNROLL * 2;           // f64 lanes per accumulator = 2
        Stmt& red = *s.body[0];
        const std::string& acc = red.lhs->str;
        Tok op = red.expr->op; // + or *
        std::string ident = op == Tok::Star ? "d_1" : "d_0";
        std::string aop = op == Tok::Star ? "mul" : "add";
        std::string vop = op == Tok::Star ? "vfmul" : "vfadd";

        std::string start = genExpr(*s.expr);
        std::string end = genExpr(*s.expr2);
        flushTemps();
        std::string endSlot = hiddenSlot("rend", 8);
        emit("storel " + end + ", " + endSlot);
        pushScope();
        std::string iSlot = addSlot(s.name, intType());
        emit("storel " + start + ", " + iSlot);

        // UNROLL accumulators in one region (16 bytes each), all initialised to
        // [identity, identity] with a vector op (vdup) — NEVER a scalar store: a
        // scalar store to this fixed slot would be forwarded by QBE's load-opt
        // straight to the readback below, hopping over the unrecognized vstore
        // and returning stale data. With no scalar store, load-opt finds nothing
        // to forward and reads real memory. The prior scalar acc is folded in
        // after the loop.
        std::string accRegion = hiddenSlot("accv", 16 * UNROLL);
        std::string s0 = findSlot(acc).addr;   // the accumulator scalar's slot
        std::string zv = newTmp(); emit(zv + " =d vdup " + ident);
        std::vector<std::string> accAddr(UNROLL);
        for (int k = 0; k < UNROLL; k++) {
            if (k == 0) accAddr[k] = accRegion;
            else {
                std::string a = newTmp();
                emit(a + " =l add " + accRegion + ", " + std::to_string(16 * k));
                accAddr[k] = a;
            }
            emit("vstore " + zv + ", " + accAddr[k]);
        }

        std::string vcond = newLbl("red_cond"), vbody = newLbl("red_body");
        std::string rcond = newLbl("rred_cond");
        std::string endL = newLbl("red_end");

        placeLabel(vcond);
        std::string iv = newTmp(); emit(iv + " =l loadl " + iSlot);
        std::string ev = newTmp(); emit(ev + " =l loadl " + endSlot);
        std::string iw = newTmp(); emit(iw + " =l add " + iv + ", " + std::to_string(STEP));
        std::string c = newTmp();  emit(c + " =w cslel " + iw + ", " + ev);
        emit("jnz " + c + ", " + vbody + ", " + rcond);
        terminated_ = true;
        placeLabel(vbody);
        {
            for (int k = 0; k < UNROLL; k++) {
                std::string av = newTmp(); emit(av + " =d vload " + accAddr[k]);
                std::map<std::string, std::string> vtemps;
                vtemps[acc] = av;             // each lane accumulates like scalar
                vecLaneOff_ = 2 * k;          // this accumulator's element pair
                std::string nv = genVecExpr(*red.expr, iSlot, vtemps);
                vecLaneOff_ = 0;
                emit("vstore " + nv + ", " + accAddr[k]);
            }
            std::string i2 = newTmp(); emit(i2 + " =l loadl " + iSlot);
            std::string i3 = newTmp(); emit(i3 + " =l add " + i2 + ", " + std::to_string(STEP));
            emit("storel " + i3 + ", " + iSlot);
            emit("jmp " + vcond);
            terminated_ = true;
        }
        // combine: fold the UNROLL accumulators (fixed order), then the 2 lanes
        placeLabel(rcond);
        {
            std::string tot = newTmp(); emit(tot + " =d vload " + accAddr[0]);
            for (int k = 1; k < UNROLL; k++) {
                std::string ak = newTmp(); emit(ak + " =d vload " + accAddr[k]);
                std::string nt = newTmp(); emit(nt + " =d " + vop + " " + tot + ", " + ak);
                tot = nt;
            }
            std::string scr = hiddenSlot("rhz", 16);
            emit("vstore " + tot + ", " + scr);
            std::string l0 = newTmp(); emit(l0 + " =d load " + scr);
            std::string l8 = newTmp(); emit(l8 + " =l add " + scr + ", 8");
            std::string l1 = newTmp(); emit(l1 + " =d load " + l8);
            std::string comb = newTmp();
            emit(comb + " =d " + aop + " " + l0 + ", " + l1);
            std::string so = loadScalar(s0, floatType());  // fold in prior acc
            std::string sf = newTmp();
            emit(sf + " =d " + aop + " " + so + ", " + comb);
            storeScalar(s0, floatType(), sf);
        }
        // scalar remainder for the odd tail (continues from iSlot / acc)
        std::string rc2cond = newLbl("rtail_cond"), rc2body = newLbl("rtail_body");
        placeLabel(rc2cond);
        std::string riv = newTmp(); emit(riv + " =l loadl " + iSlot);
        std::string rev = newTmp(); emit(rev + " =l loadl " + endSlot);
        std::string rcx = newTmp(); emit(rcx + " =w csltl " + riv + ", " + rev);
        emit("jnz " + rcx + ", " + rc2body + ", " + endL);
        terminated_ = true;
        placeLabel(rc2body);
        {
            auto savedRanges = ranges_;
            ranges_.erase(s.name);
            genStmts(s.body);
            if (!terminated_) {
                std::string i2 = newTmp(); emit(i2 + " =l loadl " + iSlot);
                std::string i3 = newTmp(); emit(i3 + " =l add " + i2 + ", 1");
                emit("storel " + i3 + ", " + iSlot);
                emit("jmp " + rc2cond);
                terminated_ = true;
            }
            ranges_ = savedRanges;
        }
        placeLabel(endL);
        popScope();
    }
    // strip-mined: an UNROLL-wide vector loop while i+2*UNROLL <= hi, then a
    // scalar remainder that continues from the same counter for the tail.
    // Unrolling processes UNROLL independent 2-lane vectors per iteration so
    // the CPU can overlap their compute/load/store chains (element-wise phases
    // like `((v*.5+.25)*v+.1)*.5 + y*.5` are a per-element dependency chain).
    // Each vector is computed and stored before the next, so no `Kd` vector is
    // ever live across a store — QBE never has to spill one (which would drop a
    // lane). The opt-vs-no-opt + cross-arch differentials guard against it.
    void emitVecLoop(Stmt& s) {
        const int UNROLL = 2;
        const int STEP = 2 * UNROLL;           // elements handled per iteration
        std::string start = genExpr(*s.expr);
        std::string end = genExpr(*s.expr2);
        flushTemps();
        std::string endSlot = hiddenSlot("vend", 8);
        emit("storel " + end + ", " + endSlot);
        pushScope();
        std::string iSlot = addSlot(s.name, intType());
        emit("storel " + start + ", " + iSlot);

        std::string vcond = newLbl("vec_cond"), vbody = newLbl("vec_body");
        std::string rcond = newLbl("rem_cond"), rbody = newLbl("rem_body");
        std::string endL = newLbl("vfor_end");

        placeLabel(vcond);
        std::string iv = newTmp(); emit(iv + " =l loadl " + iSlot);
        std::string ev = newTmp(); emit(ev + " =l loadl " + endSlot);
        std::string iw = newTmp(); emit(iw + " =l add " + iv + ", " + std::to_string(STEP));
        std::string c = newTmp();  emit(c + " =w cslel " + iw + ", " + ev);
        emit("jnz " + c + ", " + vbody + ", " + rcond);
        terminated_ = true;
        placeLabel(vbody);
        {
            for (int u = 0; u < UNROLL; u++) {
                vecLaneOff_ = 2 * u;           // this vector's element pair
                std::map<std::string, std::string> vtemps; // lets are per-element
                for (auto& stp : s.body) {
                    Stmt& st = *stp;
                    if (st.kind == StmtKind::Let)
                        vtemps[st.name] = genVecExpr(*st.expr, iSlot, vtemps);
                    else {
                        std::string vr = genVecExpr(*st.expr, iSlot, vtemps);
                        std::string addr = vecElemAddr(*st.lhs, iSlot);
                        emit("vstore " + vr + ", " + addr);
                    }
                }
            }
            vecLaneOff_ = 0;
            std::string i2 = newTmp(); emit(i2 + " =l loadl " + iSlot);
            std::string i3 = newTmp(); emit(i3 + " =l add " + i2 + ", " + std::to_string(STEP));
            emit("storel " + i3 + ", " + iSlot);
            emit("jmp " + vcond);
            terminated_ = true;
        }
        placeLabel(rcond);
        std::string riv = newTmp(); emit(riv + " =l loadl " + iSlot);
        std::string rev = newTmp(); emit(rev + " =l loadl " + endSlot);
        std::string rc = newTmp();  emit(rc + " =w csltl " + riv + ", " + rev);
        emit("jnz " + rc + ", " + rbody + ", " + endL);
        terminated_ = true;
        placeLabel(rbody);
        {
            auto savedRanges = ranges_;
            ranges_.erase(s.name);
            genStmts(s.body);
            if (!terminated_) {
                std::string i2 = newTmp(); emit(i2 + " =l loadl " + iSlot);
                std::string i3 = newTmp(); emit(i3 + " =l add " + i2 + ", 1");
                emit("storel " + i3 + ", " + iSlot);
                emit("jmp " + rcond);
                terminated_ = true;
            }
            ranges_ = savedRanges;
        }
        placeLabel(endL);
        popScope();
    }

    static bool constOf(const Expr& e, long long& out) {
        if (e.kind == ExprKind::IntLit) { out = e.ival; return true; }
        if (e.kind == ExprKind::Unary && e.op == Tok::Minus && e.lhs &&
            e.lhs->kind == ExprKind::IntLit) {
            out = -e.lhs->ival;
            return true;
        }
        return false;
    }
    static long long addClamp(long long a, long long b) {
        if (a <= RLO || b <= RLO) return RLO;
        if (a >= RHI || b >= RHI) return RHI;
        long long r = a + b;
        return r < RLO ? RLO : (r > RHI ? RHI : r);
    }

    Range rangeOf(const Expr& e) {
        long long c;
        if (constOf(e, c)) return {c, c};
        switch (e.kind) {
        case ExprKind::Var: {
            if (addrTaken_.count(e.str)) return {};
            auto it = ranges_.find(e.str);
            return it == ranges_.end() ? Range{} : it->second;
        }
        case ExprKind::Binary: {
            if (!e.lhs || !e.rhs) return {};
            Range a = rangeOf(*e.lhs), b = rangeOf(*e.rhs);
            if (e.op == Tok::Plus)
                return {addClamp(a.lo, b.lo), addClamp(a.hi, b.hi)};
            if (e.op == Tok::Minus)
                return {addClamp(a.lo, -b.hi), addClamp(a.hi, -b.lo)};
            if (e.op == Tok::Percent && constOf(*e.rhs, c) && c > 0)
                return a.lo >= 0 ? Range{0, c - 1} : Range{-(c - 1), c - 1};
            return {};
        }
        default:
            return {};
        }
    }

    // Narrow ranges using a condition known to hold (or to fail).
    void refine(const Expr& c, bool holds) {
        if (c.kind == ExprKind::Binary && c.op == Tok::AndAnd && holds) {
            refine(*c.lhs, true);
            refine(*c.rhs, true);
            return;
        }
        if (c.kind != ExprKind::Binary || !c.lhs || !c.rhs) return;
        Tok op = c.op;
        if (!holds) { // invert the comparison
            switch (op) {
            case Tok::Lt: op = Tok::Ge; break;
            case Tok::Le: op = Tok::Gt; break;
            case Tok::Gt: op = Tok::Le; break;
            case Tok::Ge: op = Tok::Lt; break;
            default: return;
            }
        }
        const Expr* v = c.lhs.get();
        if (v->kind != ExprKind::Var || addrTaken_.count(v->str)) return;
        Range b = rangeOf(*c.rhs);
        Range& r = ranges_[v->str];
        switch (op) {
        case Tok::Lt: r.hi = std::min(r.hi, addClamp(b.hi, -1)); break;
        case Tok::Le: r.hi = std::min(r.hi, b.hi); break;
        case Tok::Gt: r.lo = std::max(r.lo, addClamp(b.lo, 1)); break;
        case Tok::Ge: r.lo = std::max(r.lo, b.lo); break;
        default: break;
        }
    }

    // Every variable a statement list assigns to (recursively).
    static void collectAssigned(const std::vector<StmtPtr>& body,
                                std::set<std::string>& out) {
        for (auto& s : body) {
            if ((s->kind == StmtKind::Assign) && s->lhs) {
                const Expr* root = s->lhs.get();
                while (root->kind == ExprKind::Field || root->kind == ExprKind::Index)
                    root = root->lhs.get();
                if (root->kind == ExprKind::Var) out.insert(root->str);
            }
            if (s->kind == StmtKind::Let) out.insert(s->name);
            if (s->kind == StmtKind::For) out.insert(s->name);
            collectAssigned(s->body, out);
            collectAssigned(s->elseBody, out);
        }
    }
    void forgetAssigned(const std::vector<StmtPtr>& body) {
        std::set<std::string> w;
        collectAssigned(body, w);
        for (auto& n : w) ranges_.erase(n);
    }

    // How a loop body changes a variable: every assignment to it is either
    // `x = x - c` (Down), `x = x + c` (Up), or something else (Unknown).
    // A cursor that only moves one way keeps the bound on its other side
    // across the whole loop — which is what makes `while (j >= 0) { ...
    // j = j - 1; }` provably in range at the top *and* bottom.
    enum class Motion { None, Up, Down, Unknown };
    static Motion combine(Motion a, Motion b) {
        if (a == Motion::None) return b;
        if (b == Motion::None) return a;
        return a == b ? a : Motion::Unknown;
    }
    Motion motionOf(const std::vector<StmtPtr>& body, const std::string& name) {
        Motion m = Motion::None;
        for (auto& s : body) {
            if (s->kind == StmtKind::Let && s->name == name) return Motion::Unknown;
            if (s->kind == StmtKind::For && s->name == name) return Motion::Unknown;
            if (s->kind == StmtKind::Assign && s->lhs &&
                s->lhs->kind == ExprKind::Var && s->lhs->str == name) {
                const Expr* r = s->expr.get();
                Motion step = Motion::Unknown;
                if (r && r->kind == ExprKind::Binary &&
                    (r->op == Tok::Plus || r->op == Tok::Minus) && r->lhs && r->rhs &&
                    r->lhs->kind == ExprKind::Var && r->lhs->str == name) {
                    Range d = rangeOf(*r->rhs);
                    if (d.lo >= 0) step = r->op == Tok::Plus ? Motion::Up : Motion::Down;
                }
                m = combine(m, step);
                if (m == Motion::Unknown) return m;
            }
            m = combine(m, motionOf(s->body, name));
            if (m == Motion::Unknown) return m;
            m = combine(m, motionOf(s->elseBody, name));
            if (m == Motion::Unknown) return m;
        }
        return m;
    }

    // Largest single decrement a loop body applies to `name`, or 0 if any
    // step is not a known constant.
    long long maxDownStep(const std::vector<StmtPtr>& body, const std::string& name) {
        long long worst = 0;
        for (auto& s : body) {
            if (s->kind == StmtKind::Assign && s->lhs &&
                s->lhs->kind == ExprKind::Var && s->lhs->str == name) {
                const Expr* r = s->expr.get();
                if (!r || r->kind != ExprKind::Binary || r->op != Tok::Minus ||
                    !r->lhs || !isVar(r->lhs.get(), name))
                    return 0;
                Range d = rangeOf(*r->rhs);
                if (d.hi >= RHI || d.hi < 0) return 0;
                worst = std::max(worst, d.hi);
            }
            long long a = maxDownStep(s->body, name);
            if (!s->body.empty() && a == 0 && motionOf(s->body, name) != Motion::None)
                return 0;
            worst = std::max(worst, a);
            long long b = maxDownStep(s->elseBody, name);
            if (!s->elseBody.empty() && b == 0 &&
                motionOf(s->elseBody, name) != Motion::None)
                return 0;
            worst = std::max(worst, b);
        }
        return worst;
    }

    // Widen for a loop: keep the bound the variable cannot cross.
    void widenForLoop(const std::vector<StmtPtr>& body) {
        std::set<std::string> w;
        collectAssigned(body, w);
        for (auto& n : w) {
            auto it = ranges_.find(n);
            if (it == ranges_.end()) continue;
            switch (motionOf(body, n)) {
            case Motion::Down: it->second.lo = RLO; break; // only falls: hi holds
            case Motion::Up:   it->second.hi = RHI; break; // only rises: lo holds
            case Motion::None: break;                      // untouched
            default: ranges_.erase(it); break;
            }
        }
    }

    // Record every variable whose address is taken anywhere in a function;
    // an `unsafe` write through such a pointer is invisible to this analysis.
    static void collectAddrTaken(const Expr* e, std::set<std::string>& out) {
        if (!e) return;
        if (e->kind == ExprKind::AddrOf) {
            const Expr* root = e->lhs.get();
            while (root && (root->kind == ExprKind::Field ||
                            root->kind == ExprKind::Index))
                root = root->lhs.get();
            if (root && root->kind == ExprKind::Var) out.insert(root->str);
        }
        collectAddrTaken(e->lhs.get(), out);
        collectAddrTaken(e->rhs.get(), out);
        for (auto& a : e->args) collectAddrTaken(a.get(), out);
    }
    static void collectAddrTaken(const std::vector<StmtPtr>& body,
                                 std::set<std::string>& out) {
        for (auto& s : body) {
            collectAddrTaken(s->expr.get(), out);
            collectAddrTaken(s->expr2.get(), out);
            collectAddrTaken(s->lhs.get(), out);
            collectAddrTaken(s->body, out);
            collectAddrTaken(s->elseBody, out);
        }
    }

    // Is this index expression certainly within [0, n)?
    bool indexProvablyInRange(const Expr& idx, long n) {
        Range r = rangeOf(idx);
        return r.lo >= 0 && r.hi < n;
    }

    // ---- places ----

    std::string genPlace(Expr& e) {
        switch (e.kind) {
        case ExprKind::Var:
            return findSlot(e.str).addr;
        case ExprKind::Deref:
            return genExpr(*e.lhs); // the pointer value *is* the address
        case ExprKind::Field: {
            std::string base = genPlace(*e.lhs);
            const Layout& lay = layout(e.lhs->type.sname);
            long off = lay.fields.at(e.str).first;
            if (off == 0) return base;
            std::string t = newTmp();
            emit(t + " =l add " + base + ", " + std::to_string(off));
            return t;
        }
        case ExprKind::Index: {
            // map lookup. Fast path inlined at the call site: if the cached
            // entry's key IS this key (pointer identity — sound because the
            // map retains its keys, so identity to a live key proves
            // content), use it without any call. Slow path traps on absence.
            if (e.lhs->type.kind == TypeKind::Map) {
                needMap_ = true;
                std::string mp = genExpr(*e.lhs);       // the map handle
                std::string key = genExpr(*e.rhs);
                std::string out = hiddenSlot("mget", 8);
                std::string chkL = newLbl("mg_chk"), keyL = newLbl("mg_key");
                std::string hitL = newLbl("mg_hit"), slowL = newLbl("mg_slow");
                std::string doneL = newLbl("mg_done");
                std::string cp = newTmp(), le = newTmp();
                emit(cp + " =l add " + mp + ", 72");
                emit(le + " =l loadl " + cp);
                emit("jnz " + le + ", " + chkL + ", " + slowL);
                terminated_ = true;
                placeLabel(chkL);
                std::string st = newTmp();
                emit(st + " =l loadl " + le);           // entry state: live?
                emit("jnz " + st + ", " + keyL + ", " + slowL);
                terminated_ = true;
                placeLabel(keyL);
                std::string kp = newTmp(), ke = newTmp(), eq = newTmp();
                emit(kp + " =l add " + le + ", 8");
                emit(ke + " =l loadl " + kp);
                emit(eq + " =w ceql " + ke + ", " + key);
                emit("jnz " + eq + ", " + hitL + ", " + slowL);
                terminated_ = true;
                placeLabel(hitL);
                std::string vp = newTmp();
                emit(vp + " =l add " + le + ", 16");
                emit("storel " + vp + ", " + out);
                emit("jmp " + doneL);
                terminated_ = true;
                placeLabel(slowL);
                std::string t = newTmp();
                emit(t + " =l call $simple_map_get(l " + mp + ", l " + key + ")");
                emit("storel " + t + ", " + out);
                emit("jmp " + doneL);
                terminated_ = true;
                placeLabel(doneL);
                std::string r = newTmp();
                emit(r + " =l loadl " + out);
                return r;
            }
            // list indexing: base and length are read from the heap header;
            // the check is always dynamic (length isn't a compile-time fact)
            if (e.lhs->type.kind == TypeKind::List) {
                needList_ = true;
                std::string lp = genExpr(*e.lhs);       // the list handle
                std::string idx = genExpr(*e.rhs);
                long esz = sizeOf(*e.lhs->type.elem);
                std::string lenp = newTmp();
                emit(lenp + " =l add " + lp + ", 8");   // header[8] = len
                std::string len = newTmp();
                emit(len + " =l loadl " + lenp);
                std::string bad = newTmp();
                std::string ltz = newTmp(), gel = newTmp();
                emit(ltz + " =w csltl " + idx + ", 0");
                emit(gel + " =w csgel " + idx + ", " + len);
                emit(bad + " =w or " + ltz + ", " + gel);
                std::string oobL = newLbl("oob");
                std::string okL = newLbl("idx_ok");
                needOob_ = true;
                emit("jnz " + bad + ", " + oobL + ", " + okL);
                terminated_ = true;
                placeLabel(oobL);
                emit("call $simple_oob(l " + idx + ", l " + len + ")");
                emit("hlt");
                terminated_ = true;
                placeLabel(okL);
                // list header: [0]refcount [8]len [16]cap [24]elemsize [32]data
                std::string dp = newTmp();
                emit(dp + " =l add " + lp + ", 32");
                std::string dpv = newTmp();
                emit(dpv + " =l loadl " + dp);
                std::string off = newTmp();
                emit(off + " =l mul " + idx + ", " + std::to_string(esz));
                std::string t = newTmp();
                emit(t + " =l add " + dpv + ", " + off);
                return t;
            }
            std::string base = genPlace(*e.lhs);
            std::string idx = genExpr(*e.rhs);
            long n = e.lhs->type.alen;
            long esz = sizeOf(*e.lhs->type.elem);
            // simple(v0.6): bounds-check elimination. Array lengths live in
            // the type, and a `for (i in a..b)` variable is immutable by
            // language rule with bounds evaluated once — so when the range
            // fits the array, the check is provably dead. No value-range
            // analysis needed; it is arithmetic on facts the type system
            // already holds.
            if (indexProvablyInRange(*e.rhs, n)) {
                std::string off = newTmp();
                emit(off + " =l mul " + idx + ", " + std::to_string(esz));
                std::string t = newTmp();
                emit(t + " =l add " + base + ", " + off);
                return t;
            }
            needOob_ = true;
            std::string lt = newTmp();
            emit(lt + " =w csltl " + idx + ", 0");
            std::string ge = newTmp();
            emit(ge + " =w csgel " + idx + ", " + std::to_string(n));
            std::string bad = newTmp();
            emit(bad + " =w or " + lt + ", " + ge);
            std::string oobL = newLbl("oob");
            std::string okL = newLbl("idx_ok");
            emit("jnz " + bad + ", " + oobL + ", " + okL);
            terminated_ = true;
            placeLabel(oobL);
            emit("call $simple_oob(l " + idx + ", l " + std::to_string(n) + ")");
            emit("hlt");
            terminated_ = true;
            placeLabel(okL);
            std::string off = newTmp();
            emit(off + " =l mul " + idx + ", " + std::to_string(esz));
            std::string t = newTmp();
            emit(t + " =l add " + base + ", " + off);
            return t;
        }
        default:
            return genExpr(e);
        }
    }

    // ---- expressions ----

    std::string genExpr(Expr& e) {
        switch (e.kind) {
        case ExprKind::IntLit:  return std::to_string(e.ival);
        case ExprKind::BoolLit: return e.ival ? "1" : "0";
        case ExprKind::NullLit: return "0";
        case ExprKind::FloatLit: {
            // QBE inline float constant: d_<value> / s_<value>
            char buf[40];
            snprintf(buf, sizeof buf, "%c_%.17g", qbeType(e.type), e.fval);
            return buf;
        }
        case ExprKind::StrLit: {
            std::string t = newTmp();
            emit(t + " =l add " + strLabel(e.str) + ", 16"); // skip header
            return t;
        }
        case ExprKind::Var: {
            Slot& sl = findSlot(e.str);
            if (isAggregate(sl.type)) return sl.addr;
            return loadScalar(sl.addr, sl.type);
        }
        case ExprKind::Index:
            // string indexing: s[i] reads one byte, bounds-checked
            if (e.lhs->type.kind == TypeKind::Str) {
                std::string sp = genExpr(*e.lhs);
                std::string idx = genExpr(*e.rhs);
                std::string lenp = newTmp();
                emit(lenp + " =l sub " + sp + ", 8");     // len at ptr-8
                std::string len = newTmp();
                emit(len + " =l loadl " + lenp);
                needOob_ = true;
                std::string ltz = newTmp(), gel = newTmp(), bad = newTmp();
                emit(ltz + " =w csltl " + idx + ", 0");
                emit(gel + " =w csgel " + idx + ", " + len);
                emit(bad + " =w or " + ltz + ", " + gel);
                std::string oobL = newLbl("oob"), okL = newLbl("idx_ok");
                emit("jnz " + bad + ", " + oobL + ", " + okL);
                terminated_ = true;
                placeLabel(oobL);
                emit("call $simple_oob(l " + idx + ", l " + len + ")");
                emit("hlt");
                terminated_ = true;
                placeLabel(okL);
                std::string a = newTmp();
                emit(a + " =l add " + sp + ", " + idx);
                std::string r = newTmp();
                emit(r + " =l loadub " + a);              // byte -> int (zero-extended)
                return r;
            }
            // fall through to place-based load for arrays/lists
            [[fallthrough]];
        case ExprKind::Field:
        case ExprKind::Deref: {
            // err.msg: not a struct field — the error IS the message string
            // (or null for ok, which reads as ""). Returns an owned copy.
            if (e.kind == ExprKind::Field && isErr(e.lhs->type)) {
                needErr_ = true;
                std::string ev = genExpr(*e.lhs);
                std::string t = newTmp();
                emit(t + " =l call $simple_err_msg(l " + ev + ")");
                pushTemp(t, e.type);
                return t;
            }
            std::string addr = genPlace(e);
            if (isAggregate(e.type)) return addr;
            return loadScalar(addr, e.type);
        }
        case ExprKind::AddrOf:
            return genPlace(*e.lhs);
        case ExprKind::Cast: {
            std::string v = genExpr(*e.lhs);
            const Type& from = e.lhs->type;
            const Type& to = e.type;
            // str(int) and int(str): both call an emitted runtime helper
            if (to.kind == TypeKind::Str) {          // int -> decimal string
                needIntToStr_ = true;
                // widen a narrow int to a full 64-bit value first
                std::string w = v;
                if (from.bits < 64) {
                    w = newTmp();
                    emit(w + " =l ext" + std::string(from.uns ? "u" : "s") +
                         (from.bits == 8 ? "b" : from.bits == 16 ? "h" : "w") + " " + v);
                }
                std::string t = newTmp();
                emit(t + " =l call $simple_int_to_str(l " + w + ")");
                pushTemp(t, e.type);                 // owned new string
                return t;
            }
            if (from.kind == TypeKind::Str) {        // parse leading integer
                needStrToInt_ = true;
                std::string t = newTmp();
                emit(t + " =l call $simple_str_to_int(l " + v + ")");
                return narrow(t, to);
            }
            if (from.kind == TypeKind::Ptr) return v; // pointers are just words
            // conversions touching float go through QBE's dedicated ops
            if (isFloat(from) || isFloat(to)) {
                std::string t = newTmp();
                char tc = qbeType(to);
                if (isFloat(from) && isFloat(to)) {
                    if (from.bits == to.bits) return v;
                    emit(t + " =" + tc + (to.bits == 64 ? " exts " : " truncd ") + v);
                } else if (isInt(from) && isFloat(to)) {
                    // int -> float; widen a narrow int to a full word first
                    std::string iv = v;
                    if (from.bits < 32) {
                        iv = newTmp();
                        emit(iv + " =w ext" + std::string(from.uns ? "u" : "s") +
                             (from.bits == 8 ? "b" : "h") + " " + v);
                    }
                    std::string cv = from.bits == 64 ? "l" : "w";
                    std::string sg = from.uns ? "u" : "s";
                    emit(t + " =" + tc + " " + sg + cv + "tof " + iv);
                } else { // float -> int
                    std::string ic = to.bits == 64 ? "l" : "w";
                    std::string fc = isFloat(from) && from.bits == 32 ? "s" : "d";
                    std::string res = newTmp();
                    emit(res + " =" + ic + " " + fc + "to" + std::string(to.uns ? "ui" : "si") +
                         " " + v);
                    return narrow(res, to);
                }
                return t;
            }
            int fb = from.bits, tb = to.bits;
            // widening keeps the value; narrowing needs an explicit truncate
            if (tb == 64 && fb < 64) {
                std::string t = newTmp();
                emit(t + " =l ext" + std::string(from.uns ? "u" : "s") +
                     (fb == 8 ? "b" : fb == 16 ? "h" : "w") + " " + v);
                return t;
            }
            if (tb < 64 && tb < fb) {
                std::string t = newTmp();
                long mask = (1LL << tb) - 1;
                emit(t + " =w and " + v + ", " + std::to_string(mask));
                if (!to.uns) { // re-extend so the sign is right in a w reg
                    std::string s = newTmp();
                    emit(s + " =w ext" + std::string("s") + (tb == 8 ? "b" : "h") + " " + t);
                    return s;
                }
                return t;
            }
            if (tb < 64 && fb == 64) {
                std::string t = newTmp();
                emit(t + " =w copy " + v);
                if (tb < 32) {
                    std::string m = newTmp();
                    long mask = (1LL << tb) - 1;
                    emit(m + " =w and " + t + ", " + std::to_string(mask));
                    if (!to.uns) {
                        std::string s = newTmp();
                        emit(s + " =w exts" + std::string(tb == 8 ? "b" : "h") + " " + m);
                        return s;
                    }
                    return m;
                }
                return t;
            }
            return v; // same width, only signedness changed: bits unchanged
        }
        case ExprKind::ArrayLit: {
            long esz = sizeOf(*e.type.elem);
            // simple(v0.6): build straight into the destination when one is
            // supplied (a `let` slot), instead of filling a temporary and
            // copying — halves stack use and skips a full array copy.
            std::string slot = destHint_.empty() ? hiddenSlot("arr", sizeOf(e.type))
                                                 : destHint_;
            bool intoDest = !destHint_.empty();
            destHint_.clear();
            if (e.ival > 0) { // [value; count] — evaluate once, fill in a loop
                std::string v = genExpr(*e.args[0]);
                consumeOwned(*e.args[0], v);
                std::string ip = hiddenSlot("fi", 8);
                emit("storel 0, " + ip);
                std::string condL = newLbl("fill_cond");
                std::string bodyL = newLbl("fill_body");
                std::string endL = newLbl("fill_end");
                placeLabel(condL);
                std::string iv = newTmp();
                emit(iv + " =l loadl " + ip);
                std::string c = newTmp();
                emit(c + " =w csltl " + iv + ", " + std::to_string(e.ival));
                emit("jnz " + c + ", " + bodyL + ", " + endL);
                terminated_ = true;
                placeLabel(bodyL);
                std::string off = newTmp();
                emit(off + " =l mul " + iv + ", " + std::to_string(esz));
                std::string addr = newTmp();
                emit(addr + " =l add " + slot + ", " + off);
                // each copy of a shared rc value needs its own reference
                if (typeHasRc(*e.type.elem)) emitRetainValue(v, *e.type.elem);
                storeInterior(addr, *e.type.elem, v);
                std::string i2 = newTmp();
                emit(i2 + " =l add " + iv + ", 1");
                emit("storel " + i2 + ", " + ip);
                emit("jmp " + condL);
                terminated_ = true;
                placeLabel(endL);
                if (typeHasRc(*e.type.elem)) emitReleaseValue(v, *e.type.elem);
                if (!intoDest) pushTemp(slot, e.type);
                return slot;
            }
            for (size_t i = 0; i < e.args.size(); i++) {
                std::string v = genExpr(*e.args[i]);
                consumeOwned(*e.args[i], v);
                std::string addr = slot;
                if (i > 0) {
                    addr = newTmp();
                    emit(addr + " =l add " + slot + ", " + std::to_string((long)i * esz));
                }
                storeInterior(addr, *e.type.elem, v);
            }
            if (!intoDest) pushTemp(slot, e.type);
            return slot;
        }
        case ExprKind::StructLit: {
            const Layout& lay = layout(e.str);
            std::string slot = destHint_.empty() ? hiddenSlot("st", lay.size) : destHint_;
            bool intoDest = !destHint_.empty();
            destHint_.clear();
            for (size_t i = 0; i < e.args.size(); i++) {
                auto& fo = lay.fields.at(e.fieldNames[i]);
                std::string v = genExpr(*e.args[i]);
                consumeOwned(*e.args[i], v);
                std::string addr = slot;
                if (fo.first > 0) {
                    addr = newTmp();
                    emit(addr + " =l add " + slot + ", " + std::to_string(fo.first));
                }
                storeInterior(addr, fo.second, v);
            }
            if (!intoDest) pushTemp(slot, e.type);
            return slot;
        }
        case ExprKind::ChanNew: {
            needChan_ = true;
            std::string cap = genExpr(*e.args[0]);
            const Type& elemT = *e.type.elem;
            // channels run this destructor on items still buffered when the
            // last reference drops, so queued strings/handles are not leaked
            std::string dtor = typeHasRc(elemT) ? rcHelper(elemT, 'd') : "0";
            std::string t = newTmp();
            emit(t + " =l call $simple_chan_new(l " + std::to_string(sizeOf(elemT)) +
                 ", l " + cap + ", l " + dtor + ")");
            pushTemp(t, e.type);
            return t;
        }
        case ExprKind::ListNew: {
            needList_ = true;
            std::string t = newTmp();
            emit(t + " =l call $simple_list_new(l " + std::to_string(sizeOf(*e.type.elem)) +
                 ")");
            pushTemp(t, e.type);
            return t;
        }
        case ExprKind::MapNew: {
            needMap_ = true;
            long stride = 16 + alignUp(sizeOf(*e.type.elem), 8);
            const char* kis = e.type.key->kind == TypeKind::Str ? "1" : "0";
            std::string t = newTmp();
            emit(t + " =l call $simple_map_new(l " + std::to_string(stride) +
                 ", l " + kis + ")");
            pushTemp(t, e.type);
            return t;
        }
        case ExprKind::Unary: {
            std::string v = genExpr(*e.lhs);
            std::string t = newTmp();
            std::string k(1, qbeType(e.type));
            if (e.op == Tok::Minus && isFloat(e.type)) {
                emit(t + " =" + k + " neg " + v);
                return t;
            }
            if (e.op == Tok::Minus) {
                emit(t + " =" + k + " sub 0, " + v);
                return narrow(t, e.type);
            }
            if (e.op == Tok::Tilde) {
                emit(t + " =" + k + " xor " + v + ", -1");
                return narrow(t, e.type);
            }
            emit(t + " =w ceqw " + v + ", 0");
            return t;
        }
        case ExprKind::Binary: return genBinary(e);
        case ExprKind::Call:   return genCall(e);
        }
        return "0";
    }

    std::string genBinary(Expr& e) {
        if (e.op == Tok::AndAnd || e.op == Tok::OrOr) {
            bool isAnd = e.op == Tok::AndAnd;
            std::string slot = hiddenSlot("sc", 4);
            std::string rhsL = newLbl(isAnd ? "and_rhs" : "or_rhs");
            std::string shortL = newLbl(isAnd ? "and_false" : "or_true");
            std::string endL = newLbl(isAnd ? "and_end" : "or_end");
            std::string a = genExpr(*e.lhs);
            if (isAnd) emit("jnz " + a + ", " + rhsL + ", " + shortL);
            else emit("jnz " + a + ", " + shortL + ", " + rhsL);
            terminated_ = true;
            placeLabel(rhsL);
            // short-circuit: the right side runs only when the left side
            // held (for &&) or failed (for ||), so it may assume that
            auto savedRanges = ranges_;
            refine(*e.lhs, isAnd);
            std::string b = genExpr(*e.rhs);
            ranges_ = savedRanges;
            emit("storew " + b + ", " + slot);
            emit("jmp " + endL);
            terminated_ = true;
            placeLabel(shortL);
            emit(std::string("storew ") + (isAnd ? "0" : "1") + ", " + slot);
            emit("jmp " + endL);
            terminated_ = true;
            placeLabel(endL);
            std::string t = newTmp();
            emit(t + " =w loadw " + slot);
            return t;
        }

        std::string a = genExpr(*e.lhs);
        std::string b = genExpr(*e.rhs);

        // pointer arithmetic scales by the pointee size, like C — but only
        // for + and -; comparisons fall through to the normal path
        if (e.lhs->type.kind == TypeKind::Ptr &&
            (e.op == Tok::Plus || e.op == Tok::Minus)) {
            long esz = sizeOf(*e.lhs->type.elem);
            std::string off = b;
            if (esz != 1) {
                off = newTmp();
                emit(off + " =l mul " + b + ", " + std::to_string(esz));
            }
            std::string t = newTmp();
            emit(t + " =l " + (e.op == Tok::Plus ? "add " : "sub ") + a + ", " + off);
            return t;
        }

        if (e.lhs->type.kind == TypeKind::Str) {
            if (e.op == Tok::Plus) {
                needConcat_ = true;
                std::string t = newTmp();
                emit(t + " =l call $simple_concat(l " + a + ", l " + b + ")");
                pushTemp(t, e.type); // owned until consumed or end of statement
                return t;
            }
            // length-prefixed content comparison (NUL-safe), not strcmp
            needStrEq_ = true;
            std::string eq = newTmp();
            emit(eq + " =w call $simple_streq(l " + a + ", l " + b + ")");
            if (e.op == Tok::EqEq) return eq;
            std::string t = newTmp();          // != is the negation
            emit(t + " =w ceqw " + eq + ", 0");
            return t;
        }

        // floating point: QBE's typed add/sub/mul/div and c<cmp><s|d>
        if (isFloat(e.lhs->type)) {
            std::string t = newTmp();
            std::string k(1, qbeType(e.lhs->type));   // 's' or 'd'
            const std::string& fs = k;                 // compare suffix = class
            switch (e.op) {
            case Tok::Plus:  emit(t + " =" + k + " add " + a + ", " + b); break;
            case Tok::Minus: emit(t + " =" + k + " sub " + a + ", " + b); break;
            case Tok::Star:  emit(t + " =" + k + " mul " + a + ", " + b); break;
            case Tok::Slash: emit(t + " =" + k + " div " + a + ", " + b); break;
            case Tok::Lt:  emit(t + " =w clt" + fs + " " + a + ", " + b); break;
            case Tok::Le:  emit(t + " =w cle" + fs + " " + a + ", " + b); break;
            case Tok::Gt:  emit(t + " =w cgt" + fs + " " + a + ", " + b); break;
            case Tok::Ge:  emit(t + " =w cge" + fs + " " + a + ", " + b); break;
            case Tok::EqEq: emit(t + " =w ceq" + fs + " " + a + ", " + b); break;
            case Tok::NotEq: emit(t + " =w cne" + fs + " " + a + ", " + b); break;
            default: break;
            }
            return t;
        }

        std::string t = newTmp();
        // sized ints: k is the QBE class ('w' for <=32 bits, 'l' for 64),
        // and comparisons pick signed/unsigned forms from the type
        const Type& ot = e.lhs->type;
        std::string k(1, intClass(ot));
        std::string cs = cmpSuffix(ot);
        bool u = isInt(ot) && ot.uns;
        switch (e.op) {
        case Tok::Plus:    emit(t + " =" + k + " add " + a + ", " + b); return narrow(t, ot);
        case Tok::Minus:   emit(t + " =" + k + " sub " + a + ", " + b); return narrow(t, ot);
        case Tok::Star:    emit(t + " =" + k + " mul " + a + ", " + b); return narrow(t, ot);
        case Tok::Slash:
            emit(t + " =" + k + (u ? " udiv " : " div ") + a + ", " + b);
            break;
        case Tok::Percent:
            emit(t + " =" + k + (u ? " urem " : " rem ") + a + ", " + b);
            break;
        case Tok::Amp:     emit(t + " =" + k + " and " + a + ", " + b); break;
        case Tok::Pipe:    emit(t + " =" + k + " or " + a + ", " + b); break;
        case Tok::Caret:   emit(t + " =" + k + " xor " + a + ", " + b); break;
        case Tok::Shl:     emit(t + " =" + k + " shl " + a + ", " + b); return narrow(t, ot);
        case Tok::Shr:
            emit(t + " =" + k + (u ? " shr " : " sar ") + a + ", " + b);
            break;
        case Tok::Lt:      emit(t + " =w c" + (u ? "ult" : "slt") + cs + " " + a + ", " + b); break;
        case Tok::Le:      emit(t + " =w c" + (u ? "ule" : "sle") + cs + " " + a + ", " + b); break;
        case Tok::Gt:      emit(t + " =w c" + (u ? "ugt" : "sgt") + cs + " " + a + ", " + b); break;
        case Tok::Ge:      emit(t + " =w c" + (u ? "uge" : "sge") + cs + " " + a + ", " + b); break;
        case Tok::EqEq:
            emit(t + " =w ceq" + cs + " " + a + ", " + b);
            break;
        case Tok::NotEq:
            emit(t + " =w cne" + cs + " " + a + ", " + b);
            break;
        default: break;
        }
        return t;
    }

    std::string genCall(Expr& e) {
        if (e.str == "fail") {
            // an error IS its message string (null would be ok); own a ref
            needErr_ = true;
            Expr& arg = *e.args[0];
            std::string v = genExpr(arg);
            consumeOwned(arg, v);
            pushTemp(v, e.type);
            return v;
        }
        if (e.str == "argc") {
            needIo_ = true;
            std::string t = newTmp();
            emit(t + " =l loadl $simple_argc");
            return t;
        }
        if (e.str == "arg") {
            needIo_ = true;
            needOob_ = true; // $simple_arg bounds-checks
            std::string i = genExpr(*e.args[0]);
            std::string t = newTmp();
            emit(t + " =l call $simple_arg(l " + i + ")");
            pushTemp(t, e.type); // fresh string, +1
            return t;
        }
        if (e.str == "input" || e.str == "read_all") {
            needIo_ = true;
            std::string t = newTmp();
            emit(t + " =l call $simple_read_stream(l " +
                 (e.str == "input" ? "1" : "0") + ")");
            pushTemp(t, e.type); // fresh string, +1
            return t;
        }
        if (e.str == "read_file") {
            // multi-return builtin: fills a (str, error) buffer exactly like
            // a user multi-fn; only reachable from `let (s, e) = ...`
            needIo_ = true;
            std::string buf = hiddenSlot("ret", 16);
            std::string p = genExpr(*e.args[0]);
            emit("call $simple_read_file(l " + buf + ", l " + p + ")");
            return buf;
        }
        if (e.str == "write_file") {
            needIo_ = true;
            std::string p = genExpr(*e.args[0]);
            std::string d = genExpr(*e.args[1]);
            std::string t = newTmp();
            emit(t + " =l call $simple_write_file(l " + p + ", l " + d + ")");
            pushTemp(t, e.type); // owned error (or null = ok)
            return t;
        }
        if (e.str == "exit") {
            // only legal as a bare statement (void), so the pending temps are
            // this call's own — release them and every live local before
            // leaving, keeping the rc books clean even on the exit path
            std::string v = genExpr(*e.args[0]);
            flushTemps();
            emitUnwind(0, "");
            std::string w = newTmp();
            emit(w + " =w copy " + v); // int is 64-bit; exit takes a C int
            emit("call $exit(w " + w + ")");
            emit("hlt");
            terminated_ = true;
            return "";
        }
        if (e.str == "print") {
            Expr& arg = *e.args[0];
            std::string v = genExpr(arg);
            switch (arg.type.kind) {
            case TypeKind::Int: {
                needFmtInt_ = true;
                // printf's %lld wants a full 64-bit argument
                std::string w = v;
                if (arg.type.bits < 64) {
                    w = newTmp();
                    emit(w + " =l ext" + std::string(arg.type.uns ? "u" : "s") +
                         (arg.type.bits == 8 ? "b" : arg.type.bits == 16 ? "h" : "w") +
                         " " + v);
                }
                emit("call $printf(l $fmt_int, ..., l " + w + ")");
                break;
            }
            case TypeKind::Float: {
                needFmtFlt_ = true;
                // printf promotes float args to double
                std::string d = v;
                if (arg.type.bits == 32) {
                    d = newTmp();
                    emit(d + " =d exts " + v);
                }
                emit("call $printf(l $fmt_flt, ..., d " + d + ")");
                break;
            }
            case TypeKind::Str:
                emit("call $puts(l " + v + ")");
                break;
            case TypeKind::Bool: {
                needBoolStrs_ = true;
                std::string tL = newLbl("print_true");
                std::string fL = newLbl("print_false");
                std::string eL = newLbl("print_end");
                emit("jnz " + v + ", " + tL + ", " + fL);
                terminated_ = true;
                placeLabel(tL);
                emit("call $puts(l $str_true)");
                emit("jmp " + eL);
                terminated_ = true;
                placeLabel(fL);
                emit("call $puts(l $str_false)");
                emit("jmp " + eL);
                terminated_ = true;
                placeLabel(eL);
                break;
            }
            default: break;
            }
            return "";
        }
        if (e.str == "send") {
            needChan_ = true;
            std::string ch = genExpr(*e.args[0]);
            Expr& val = *e.args[1];
            std::string v = genExpr(val);
            std::string slot = hiddenSlot("snd", sizeOf(val.type));
            storeSendValue(val, v, val.type, slot);
            emit("call $simple_chan_send(l " + ch + ", l " + slot + ")");
            return "";
        }
        if (e.str == "recv") {
            needChan_ = true;
            std::string ch = genExpr(*e.args[0]);
            const Type& T = e.type;
            std::string slot = hiddenSlot("rcv", sizeOf(T));
            emit("call $simple_chan_recv(l " + ch + ", l " + slot + ")");
            if (isAggregate(T)) {
                pushTemp(slot, T); // we own what we received
                return slot;
            }
            std::string t = newTmp();
            emit(t + " =l loadl " + slot);
            if (isRcScalar(T)) pushTemp(t, T);
            return t;
        }
        if (e.str == "len") {
            Expr& arg = *e.args[0];
            std::string v = genExpr(arg);
            if (arg.type.kind == TypeKind::Array)
                return std::to_string(arg.type.alen);
            if (arg.type.kind == TypeKind::List ||
                arg.type.kind == TypeKind::Map) {  // both keep len at header[8]
                std::string lp = newTmp();
                emit(lp + " =l add " + v + ", 8");
                std::string t = newTmp();
                emit(t + " =l loadl " + lp);
                return t;
            }
            // O(1): length lives in the string header at pointer-8
            std::string lp = newTmp();
            emit(lp + " =l sub " + v + ", 8");
            std::string t = newTmp();
            emit(t + " =l loadl " + lp);
            return t;
        }
        if (e.str == "has") {
            needMap_ = true;
            std::string m = genExpr(*e.args[0]);
            std::string k = genExpr(*e.args[1]);
            std::string t = newTmp();
            emit(t + " =l call $simple_map_has(l " + m + ", l " + k + ")");
            std::string b = newTmp();
            emit(b + " =w copy " + t); // bool result lives in a w register
            return b;
        }
        if (e.str == "del") {
            needMap_ = true;
            const Type& mapT = e.args[0]->type;
            // deletion mutates: COW first, exactly like m[k] = v
            if (e.args[0]->kind == ExprKind::Var) {
                Slot& sl = findSlot(e.args[0]->str);
                std::string vr = typeHasRc(*mapT.elem) ? mapValRetain(mapT) : "0";
                std::string h = newTmp();
                emit(h + " =l loadl " + sl.addr);
                std::string h2 = newTmp();
                emit(h2 + " =l call $simple_map_unique(l " + h + ", l " + vr + ")");
                emit("storel " + h2 + ", " + sl.addr);
            }
            std::string m = genExpr(*e.args[0]);
            std::string k = genExpr(*e.args[1]);
            std::string vd = typeHasRc(*mapT.elem) ? mapValDtor(mapT) : "0";
            emit("call $simple_map_del(l " + m + ", l " + k + ", l " + vd + ")");
            return "";
        }
        if (e.str == "substr") {
            needSubstr_ = true;
            std::string s = genExpr(*e.args[0]);
            std::string a = genExpr(*e.args[1]);
            std::string b = genExpr(*e.args[2]);
            std::string t = newTmp();
            emit(t + " =l call $simple_substr(l " + s + ", l " + a + ", l " + b + ")");
            pushTemp(t, Type{TypeKind::Str});
            return t;
        }
        if (e.str == "push") {
            needList_ = true;
            Expr& lst = *e.args[0];
            Expr& val = *e.args[1];
            const Type& elemT = *lst.type.elem;
            std::string slot = genPlace(lst);           // the list's storage
            // stage the element into a stack buffer so the runtime can memcpy it
            std::string es = std::to_string(sizeOf(elemT));
            std::string buf = hiddenSlot("pushv", sizeOf(elemT) < 8 ? 8 : sizeOf(elemT));
            std::string v = genExpr(val);
            storeInterior(buf, elemT, v);
            std::string rf = typeHasRc(elemT) ? listElemRetain(lst.type) : "0";
            std::string df = typeHasRc(elemT) ? listElemDtor(lst.type) : "0";
            std::string h = newTmp();
            emit(h + " =l loadl " + slot);
            // make unique (COW), reserve room, then append
            std::string h2 = newTmp();
            emit(h2 + " =l call $simple_list_unique(l " + h + ", l " + rf + ", l " + df + ")");
            emit("storel " + h2 + ", " + slot);
            emit("call $simple_list_reserve(l " + h2 + ")");
            std::string lenp = newTmp();
            emit(lenp + " =l add " + h2 + ", 8");
            std::string len = newTmp();
            emit(len + " =l loadl " + lenp);
            std::string dp = newTmp();
            emit(dp + " =l add " + h2 + ", 32");
            std::string data = newTmp();
            emit(data + " =l loadl " + dp);
            std::string off = newTmp();
            emit(off + " =l mul " + len + ", " + es);
            std::string dst = newTmp();
            emit(dst + " =l add " + data + ", " + off);
            emit("call $memcpy(l " + dst + ", l " + buf + ", l " + es + ")");
            std::string len1 = newTmp();
            emit(len1 + " =l add " + len + ", 1");
            emit("storel " + len1 + ", " + lenp);
            // the list now holds a reference to the pushed element
            if (typeHasRc(elemT)) {
                if (producesOwned(val)) removeTemp(v);   // transfer ownership
                else emit("call " + listElemRetain(lst.type) + "(l " + dst + ")");
            }
            return "";
        }
        if (e.str == "pop") {
            needList_ = true;
            Expr& lst = *e.args[0];
            const Type& elemT = *lst.type.elem;
            std::string es = std::to_string(sizeOf(elemT));
            std::string slot = genPlace(lst);
            std::string rf = typeHasRc(elemT) ? listElemRetain(lst.type) : "0";
            std::string df = typeHasRc(elemT) ? listElemDtor(lst.type) : "0";
            std::string h = newTmp();
            emit(h + " =l loadl " + slot);
            std::string h2 = newTmp();
            emit(h2 + " =l call $simple_list_unique(l " + h + ", l " + rf + ", l " + df + ")");
            emit("storel " + h2 + ", " + slot);
            std::string lenp = newTmp();
            emit(lenp + " =l add " + h2 + ", 8");
            std::string len = newTmp();
            emit(len + " =l loadl " + lenp);
            // bounds: pop on empty traps
            needOob_ = true;
            std::string empty = newTmp();
            emit(empty + " =w cslel " + len + ", 0");
            std::string popL = newLbl("pop_ok");
            std::string badL = newLbl("pop_empty");
            emit("jnz " + empty + ", " + badL + ", " + popL);
            terminated_ = true;
            placeLabel(badL);
            emit("call $simple_oob(l 0, l 0)");
            emit("hlt");
            terminated_ = true;
            placeLabel(popL);
            std::string len1 = newTmp();
            emit(len1 + " =l sub " + len + ", 1");
            emit("storel " + len1 + ", " + lenp);
            std::string dp = newTmp();
            emit(dp + " =l add " + h2 + ", 32");
            std::string data = newTmp();
            emit(data + " =l loadl " + dp);
            std::string off = newTmp();
            emit(off + " =l mul " + len1 + ", " + es);
            std::string src = newTmp();
            emit(src + " =l add " + data + ", " + off);
            // ownership of the element transfers to the caller
            if (isAggregate(elemT)) {
                std::string out = hiddenSlot("popv", sizeOf(elemT));
                emitCopy(out, src, sizeOf(elemT));
                if (typeHasRc(elemT)) pushTemp(out, elemT);
                return out;
            }
            std::string r = loadScalar(src, elemT);
            if (isRcScalar(elemT)) pushTemp(r, elemT);
            return r;
        }

        Function* f = fns_[e.str];
        bool multiRet = f->ret.kind == TypeKind::Multi;
        bool aggRet = isAggregate(f->ret) || multiRet;
        std::vector<std::string> vals;
        for (auto& a : e.args) vals.push_back(genExpr(*a));
        std::string argsStr;
        bool first = true;
        std::string retSlot;
        if (aggRet) {
            retSlot = hiddenSlot("ret", multiRet ? multiSize(*f) : sizeOf(f->ret));
            argsStr += "l " + retSlot;
            first = false;
        }
        for (size_t i = 0; i < vals.size(); i++) {
            if (!first) argsStr += ", ";
            first = false;
            if (i >= f->params.size()) { // extern variadic tail
                if (i == f->params.size()) argsStr += "..., ";
                argsStr += std::string(1, qbeType(e.args[i]->type)) + " " + vals[i];
                continue;
            }
            const Type& pt = f->params[i].type;
            if (isAggregate(pt)) argsStr += "l " + vals[i];
            else argsStr += std::string(1, qbeType(pt)) + " " + vals[i];
        }
        if (multiRet) {
            // only reachable from `let (a, b) = ...`, which takes over every
            // reference in the buffer — no temp tracking here
            emit("call $" + e.str + "(" + argsStr + ")");
            return retSlot;
        }
        if (aggRet) {
            emit("call $" + e.str + "(" + argsStr + ")");
            pushTemp(retSlot, f->ret); // result is +1: owned until consumed/flushed
            return retSlot;
        }
        if (f->ret.kind == TypeKind::Void) {
            emit("call $" + e.str + "(" + argsStr + ")");
            return "";
        }
        std::string t = newTmp();
        emit(t + " =" + std::string(1, qbeType(f->ret)) + " call $" + e.str + "(" + argsStr + ")");
        if (isRcScalar(f->ret)) pushTemp(t, f->ret); // result is +1
        return t;
    }
    // ================= v0.5 optimization passes =================

    size_t countInsts(const MFunc& f) {
        size_t n = 0;
        for (auto& b : f.blocks) n += b.ins.size();
        return n;
    }

    // ---- inlining ----

    // Clones `orf`'s body in place of the call at blocks[bi].ins[k].
    // Returns the index of the continuation block.
    size_t doInline(MFunc& caller, size_t bi, size_t k, const MFunc& orf, int n) {
        MInst call = caller.blocks[bi].ins[k];
        std::string pfx = "i" + std::to_string(n) + "_";

        // map callee params -> caller operand strings
        std::unordered_map<std::string, std::string> pmap;
        std::vector<std::string> pnames;
        if (orf.aggRet) pnames.push_back("%out");
        for (size_t i = 0; i < orf.fn->params.size(); i++)
            pnames.push_back("%p" + std::to_string(i));
        for (size_t i = 0; i < pnames.size(); i++) {
            std::string a = call.args[1 + i]; // "T %v" typed pair
            pmap[pnames[i]] = a.substr(a.find(' ') + 1);
        }
        auto mapTok = [&](const std::string& tok) -> std::string {
            if (!tok.empty() && tok[0] == '%') {
                auto it = pmap.find(tok);
                if (it != pmap.end()) return it->second;
                return "%" + pfx + tok.substr(1);
            }
            if (!tok.empty() && tok[0] == '@') return "@" + pfx + tok.substr(1);
            return tok;
        };
        auto mapArg = [&](const std::string& a) -> std::string {
            size_t sp = a.find(' ');
            if (sp != std::string::npos) // typed pair "T %v" inside calls
                return a.substr(0, sp + 1) + mapTok(a.substr(sp + 1));
            return mapTok(a);
        };

        // clone allocs
        for (auto& al : orf.allocs) {
            std::string line = al.substr(1); // strip tab
            size_t sp = line.find(' ');
            caller.allocs.push_back("\t" + mapTok(line.substr(0, sp)) + line.substr(sp));
        }

        std::string contL = "@" + pfx + "cont";
        std::vector<MBlock> cloned;
        for (size_t obi = 0; obi < orf.blocks.size(); obi++) {
            const MBlock& ob = orf.blocks[obi];
            MBlock nb;
            nb.label = obi == 0 ? "@" + pfx + "start" : mapTok(ob.label);
            for (auto& oi : ob.ins) {
                if (oi.op == "ret") {
                    if (!call.dst.empty() && !oi.args.empty())
                        nb.ins.push_back(mkMInst(call.dst, call.ty, "copy",
                                                 {mapTok(oi.args[0])}));
                    nb.ins.push_back(mkMInst("", 0, "jmp", {contL}));
                    continue;
                }
                MInst c = oi;
                c.dst = mapTok(c.dst);
                if (c.op == "call") {
                    for (size_t i = 1; i < c.args.size(); i++) c.args[i] = mapArg(c.args[i]);
                } else {
                    for (auto& a : c.args) a = mapTok(a);
                }
                c.text = renderInst(c);
                nb.ins.push_back(c);
            }
            cloned.push_back(std::move(nb));
        }

        // continuation block gets the rest of the split block
        MBlock cont;
        cont.label = contL;
        cont.ins.assign(caller.blocks[bi].ins.begin() + k + 1, caller.blocks[bi].ins.end());
        // split: current block jumps into the cloned entry
        caller.blocks[bi].ins.resize(k);
        caller.blocks[bi].ins.push_back(mkMInst("", 0, "jmp", {"@" + pfx + "start"}));

        size_t at = bi + 1;
        caller.blocks.insert(caller.blocks.begin() + at, cloned.begin(), cloned.end());
        caller.blocks.insert(caller.blocks.begin() + at + cloned.size(), std::move(cont));
        return at + cloned.size();
    }

    void inlinePass() {
        std::unordered_map<std::string, size_t> byName;
        for (size_t i = 0; i < mfuncs_.size(); i++) byName["$" + mfuncs_[i].name] = i;
        std::vector<MFunc> originals = mfuncs_; // clone bodies from the originals
        int inlN = 0;
        for (int round = 0; round < 3; round++) {
            for (auto& caller : mfuncs_) {
                size_t sizeNow = countInsts(caller);
                size_t bi = 0;
                while (bi < caller.blocks.size()) {
                    bool split = false;
                    for (size_t k = 0; k < caller.blocks[bi].ins.size(); k++) {
                        MInst& I = caller.blocks[bi].ins[k];
                        if (I.op != "call") continue;
                        auto it = byName.find(I.args[0]);
                        if (it == byName.end()) continue;
                        const MFunc& orf = originals[it->second];
                        size_t csize = countInsts(orf);
                        bool self = orf.name == caller.name;
                        if (self) {
                            if (sizeNow + csize > 400) continue;
                        } else {
                            if (csize > 80 || sizeNow + csize > 600) continue;
                        }
                        bi = doInline(caller, bi, k, orf, ++inlN);
                        sizeNow += csize;
                        split = true;
                        break;
                    }
                    if (!split) bi++;
                }
            }
        }
        // drop user functions nothing references anymore
        std::set<std::string> used;
        used.insert("main");
        for (auto& f : mfuncs_)
            for (auto& b : f.blocks)
                for (auto& m : b.ins)
                    if (m.op == "call" && m.args[0][0] == '$') used.insert(m.args[0].substr(1));
        for (auto& raw : rcFuncs_) // spawn trampolines call their target by name
            for (auto& f : mfuncs_)
                if (raw.find("$" + f.name + "(") != std::string::npos) used.insert(f.name);
        std::vector<MFunc> kept;
        for (auto& f : mfuncs_)
            if (used.count(f.name)) kept.push_back(std::move(f));
        mfuncs_ = std::move(kept);
    }

    // ---- strength reduction ----

    int srN_ = 0;

    // Find the definition of a temp within one block.
    static const MInst* defIn(const MBlock& b, const std::string& tmp, size_t before) {
        for (size_t i = before; i-- > 0;)
            if (b.ins[i].dst == tmp) return &b.ins[i];
        return nullptr;
    }

    // simple(v0.55) — "the guard is a proof".
    // If block bi is reachable only through `jnz (x & (2^k-1)) == 0`, then
    // inside it x is a known multiple of 2^k, so x / 2^k needs no signed
    // fixup: it is exactly an arithmetic shift. Cuts a 4-instruction
    // dependency chain to 1. Returns the stack slot x was loaded from
    // ("" if none) and sets k.
    std::string knownDivisibleSlot(const MFunc& f, size_t bi, int& k) {
        k = 0;
        const MBlock& blk = f.blocks[bi];
        const MBlock* pred = nullptr;
        int npreds = 0;
        bool isTrueTarget = false;
        for (auto& p : f.blocks) {
            if (p.ins.empty()) continue;
            const MInst& t = p.ins.back();
            bool hits = false, trueEdge = false;
            if (t.op == "jmp" && t.args[0] == blk.label) hits = true;
            else if (t.op == "jnz" && (t.args[1] == blk.label || t.args[2] == blk.label)) {
                hits = true;
                trueEdge = t.args[1] == blk.label && t.args[2] != blk.label;
            }
            if (hits) {
                npreds++;
                pred = &p;
                isTrueTarget = trueEdge;
            }
        }
        if (npreds != 1 || !pred || !isTrueTarget) return "";
        const MInst& jz = pred->ins.back();
        const MInst* cmp = defIn(*pred, jz.args[0], pred->ins.size() - 1);
        if (!cmp || cmp->op != "ceql" || cmp->args[1] != "0") return "";
        const MInst* andI = defIn(*pred, cmp->args[0], pred->ins.size() - 1);
        if (!andI || andI->op != "and") return "";
        long long mask;
        int kk;
        if (!isIntConst(andI->args[1], mask) || !isPow2(mask + 1, kk)) return "";
        const MInst* ld = defIn(*pred, andI->args[0], pred->ins.size() - 1);
        if (!ld || ld->op != "loadl") return "";
        k = kk;
        return ld->args[0];
    }

    // Is `tmp` (used at index `at`) a load of `slot` with no intervening
    // store to that slot inside this block?
    static bool loadsFromUnwritten(const MBlock& b, size_t at, const std::string& tmp,
                                   const std::string& slot) {
        const MInst* d = defIn(b, tmp, at);
        if (!d || d->op != "loadl" || d->args[0] != slot) return false;
        for (size_t i = 0; i < at; i++) {
            const MInst& m = b.ins[i];
            if ((m.op == "storel" || m.op == "storew" || m.op == "blit") &&
                m.args.size() > 1 && m.args[1] == slot)
                return false;
            if (m.op == "call") return false; // a callee could touch it
        }
        return true;
    }

    void strengthPass() {
        for (auto& f : mfuncs_) {
            // use counts + the single compare-against-zero use of each temp
            std::unordered_map<std::string, int> uses;
            std::unordered_map<std::string, MInst*> cmp0;
            for (auto& b : f.blocks)
                for (auto& m : b.ins) {
                    size_t a0 = m.op == "call" ? 1 : 0;
                    for (size_t i = a0; i < m.args.size(); i++) {
                        std::string tok = m.args[i];
                        size_t sp = tok.find(' ');
                        if (sp != std::string::npos) tok = tok.substr(sp + 1);
                        if (!tok.empty() && tok[0] == '%') {
                            uses[tok]++;
                            if ((m.op == "ceql" || m.op == "cnel") && m.args[1] == "0" &&
                                m.args[0] == tok)
                                cmp0[tok] = &m;
                        }
                    }
                }
            for (size_t bi = 0; bi < f.blocks.size(); bi++) {
                MBlock& b = f.blocks[bi];
                int knownK = 0;
                std::string knownSlot = knownDivisibleSlot(f, bi, knownK);
                for (size_t k = 0; k < b.ins.size(); k++) {
                    // copy fields first: mutations below invalidate references
                    std::string op = b.ins[k].op, dst = b.ins[k].dst;
                    char ty = b.ins[k].ty;
                    std::vector<std::string> args = b.ins[k].args;
                    long long c;
                    int p2;
                    if (ty != 'l' || args.size() != 2) continue;
                    if (op == "mul") {
                        // mul by 2^k -> shift (also feeds array indexing)
                        std::string x = args[0], cs = args[1];
                        if (!isIntConst(cs, c) && isIntConst(x, c)) std::swap(x, cs);
                        if (!isIntConst(cs, c)) continue;
                        if (isPow2(c, p2)) {
                            b.ins[k] = p2 == 0
                                           ? mkMInst(dst, 'l', "copy", {x})
                                           : mkMInst(dst, 'l', "shl",
                                                     {x, std::to_string(p2)});
                            continue;
                        }
                        // x*3, x*5, x*9 -> shift + add (multiply is ~3 cycles,
                        // shift+add is 2 independent 1-cycle ops)
                        if (isPow2(c - 1, p2) && p2 > 0 && p2 <= 3) {
                            std::string t = "%sr" + std::to_string(++srN_);
                            b.ins[k] = mkMInst(t, 'l', "shl", {x, std::to_string(p2)});
                            b.ins.insert(b.ins.begin() + k + 1,
                                         mkMInst(dst, 'l', "add", {t, x}));
                            k++;
                        }
                    } else if (op == "rem" && isIntConst(args[1], c) && isPow2(c, p2)) {
                        // (x % 2^k) == 0  <=>  (x & (2^k-1)) == 0, any sign
                        if (uses[dst] == 1 && cmp0.count(dst)) {
                            b.ins[k] = mkMInst(dst, 'l', "and",
                                               {args[0], std::to_string(c - 1)});
                            continue;
                        }
                        // general signed remainder: r = x - (x /s 2^k) * 2^k
                        std::string x = args[0];
                        std::string p2s = std::to_string(p2);
                        std::string sum = insertDivPrefix(b.ins, k, x, p2);
                        std::string q = "%sr" + std::to_string(++srN_);
                        std::string mt = "%sr" + std::to_string(++srN_);
                        b.ins[k + 3] = mkMInst(q, 'l', "sar", {sum, p2s});
                        std::vector<MInst> tail = {
                            mkMInst(mt, 'l', "shl", {q, p2s}),
                            mkMInst(dst, 'l', "sub", {x, mt}),
                        };
                        b.ins.insert(b.ins.begin() + k + 4, tail.begin(), tail.end());
                        k += 5;
                    } else if (op == "div" && isIntConst(args[1], c) && isPow2(c, p2)) {
                        std::string x = args[0];
                        if (p2 == 0) {
                            b.ins[k] = mkMInst(dst, 'l', "copy", {x});
                            continue;
                        }
                        // the guard proves x is a multiple of 2^p2: no fixup
                        if (!knownSlot.empty() && knownK == p2 &&
                            loadsFromUnwritten(b, k, x, knownSlot)) {
                            b.ins[k] = mkMInst(dst, 'l', "sar", {x, std::to_string(p2)});
                            continue;
                        }
                        std::string sum = insertDivPrefix(b.ins, k, x, p2);
                        b.ins[k + 3] = mkMInst(dst, 'l', "sar", {sum, std::to_string(p2)});
                        k += 3;
                    }
                }
            }
        }
    }

    // Inserts the signed-division fixup before index k (3 instructions) and
    // returns the biased sum; the quotient is `sar sum, k`. Standard fixup:
    // negative dividends get divisor-1 added so the arithmetic shift matches
    // sdiv's truncation toward zero.
    std::string insertDivPrefix(std::vector<MInst>& ins, size_t k, const std::string& x,
                                int p2) {
        std::string s = "%sr" + std::to_string(++srN_);
        std::string bias = "%sr" + std::to_string(++srN_);
        std::string sum = "%sr" + std::to_string(++srN_);
        std::vector<MInst> seq = {
            mkMInst(s, 'l', "sar", {x, "63"}),
            mkMInst(bias, 'l', "shr", {s, std::to_string(64 - p2)}),
            mkMInst(sum, 'l', "add", {x, bias}),
        };
        ins.insert(ins.begin() + k, seq.begin(), seq.end());
        return sum;
    }

    // NOTE (v0.55): loop rotation was implemented and MEASURED, then
    // removed. Restructuring the loop header hid the body's diamond from
    // the backend's if-conversion, turning a csel back into a real branch:
    // collatz regressed 0.72 -> 0.77 s with no gain on any other
    // benchmark. If-conversion is worth more than a saved jump.

    // ---- constant folding + dead code elimination ----

    void foldDcePass() {
        for (auto& f : mfuncs_) {
            for (int iter = 0; iter < 4; iter++) {
                bool changed = false;
                // copy/const propagation — but ONLY for single-definition
                // temps: inlined multi-return functions assign the call's
                // result temp once per return site
                std::unordered_map<std::string, int> defs;
                for (auto& b : f.blocks)
                    for (auto& m : b.ins)
                        if (!m.dst.empty()) defs[m.dst]++;
                std::unordered_map<std::string, std::string> prop;
                for (auto& b : f.blocks)
                    for (auto& m : b.ins)
                        if (m.op == "copy" && !m.dst.empty() && defs[m.dst] == 1) {
                            // and the source must itself be stable: a constant,
                            // a global, or a single-def temp
                            const std::string& src = m.args[0];
                            if (src[0] != '%' || defs[src] == 1) prop[m.dst] = src;
                        }
                // resolve chains
                for (auto& [k2, v] : prop) {
                    std::string v2 = v;
                    int guard = 0;
                    while (prop.count(v2) && guard++ < 8) v2 = prop[v2];
                    prop[k2] = v2;
                }
                if (!prop.empty()) {
                    for (auto& b : f.blocks)
                        for (auto& m : b.ins) {
                            size_t a0 = m.op == "call" ? 1 : 0;
                            for (size_t i = a0; i < m.args.size(); i++) {
                                std::string& a = m.args[i];
                                size_t sp = a.find(' ');
                                std::string tok = sp == std::string::npos ? a : a.substr(sp + 1);
                                auto it = prop.find(tok);
                                if (it == prop.end()) continue;
                                a = sp == std::string::npos
                                        ? it->second
                                        : a.substr(0, sp + 1) + it->second;
                                m.text = renderInst(m);
                                changed = true;
                            }
                        }
                }
                // fold constant arithmetic
                for (auto& b : f.blocks)
                    for (auto& m : b.ins) {
                        if (m.dst.empty() || m.args.size() != 2) continue;
                        long long x, y;
                        if (!isIntConst(m.args[0], x) || !isIntConst(m.args[1], y)) continue;
                        long long r;
                        if (!foldOp(m.op, x, y, r)) continue;
                        b.ins[&m - &b.ins[0]] = mkMInst(m.dst, m.ty, "copy",
                                                        {std::to_string(r)});
                        changed = true;
                    }
                // algebraic identities: cheap on their own, and they expose
                // constants for the folding and DCE above to consume
                for (auto& b : f.blocks)
                    for (size_t k = 0; k < b.ins.size(); k++) {
                        MInst& m = b.ins[k];
                        if (m.dst.empty() || m.args.size() != 2) continue;
                        long long c;
                        const std::string &a0 = m.args[0], &a1 = m.args[1];
                        bool c0 = isIntConst(a0, c);
                        long long v0 = c0 ? c : 0;
                        bool c1 = isIntConst(a1, c);
                        long long v1 = c1 ? c : 0;
                        auto toCopy = [&](const std::string& src) {
                            b.ins[k] = mkMInst(m.dst, m.ty, "copy", {src});
                            changed = true;
                        };
                        const std::string& op = m.op;
                        if ((op == "add" || op == "sub" || op == "or" || op == "xor" ||
                             op == "shl" || op == "shr" || op == "sar") &&
                            c1 && v1 == 0)
                            toCopy(a0);
                        else if (op == "add" && c0 && v0 == 0)
                            toCopy(a1);
                        else if (op == "or" && c0 && v0 == 0)
                            toCopy(a1);
                        else if (op == "xor" && c0 && v0 == 0)
                            toCopy(a1);
                        else if ((op == "mul" || op == "div") && c1 && v1 == 1)
                            toCopy(a0);
                        else if (op == "mul" && c0 && v0 == 1)
                            toCopy(a1);
                        else if (op == "mul" && ((c1 && v1 == 0) || (c0 && v0 == 0)))
                            toCopy("0");
                        else if (op == "and" && ((c1 && v1 == 0) || (c0 && v0 == 0)))
                            toCopy("0");
                        else if (op == "and" && c1 && v1 == -1)
                            toCopy(a0);
                        else if (op == "and" && c0 && v0 == -1)
                            toCopy(a1);
                        else if (op == "or" && ((c1 && v1 == -1) || (c0 && v0 == -1)))
                            toCopy("-1");
                        else if ((op == "sub" || op == "xor") && a0 == a1 && !c0)
                            toCopy("0");
                        else if ((op == "and" || op == "or") && a0 == a1 && !c0)
                            toCopy(a0);
                    }
                // jnz on a constant becomes jmp
                for (auto& b : f.blocks) {
                    if (b.ins.empty()) continue;
                    MInst& t = b.ins.back();
                    long long c;
                    if (t.op == "jnz" && isIntConst(t.args[0], c)) {
                        b.ins.back() = mkMInst("", 0, "jmp", {c != 0 ? t.args[1] : t.args[2]});
                        changed = true;
                    }
                }
                // drop unreachable blocks
                {
                    std::set<std::string> reach;
                    std::vector<std::string> work{f.blocks[0].label};
                    std::unordered_map<std::string, MBlock*> byLabel;
                    for (auto& b : f.blocks) byLabel[b.label] = &b;
                    while (!work.empty()) {
                        std::string l = work.back();
                        work.pop_back();
                        if (!reach.insert(l).second) continue;
                        MBlock* b = byLabel[l];
                        if (!b || b->ins.empty()) continue;
                        MInst& t = b->ins.back();
                        if (t.op == "jmp") work.push_back(t.args[0]);
                        else if (t.op == "jnz") {
                            work.push_back(t.args[1]);
                            work.push_back(t.args[2]);
                        }
                    }
                    std::vector<MBlock> kept;
                    for (auto& b : f.blocks)
                        if (reach.count(b.label)) kept.push_back(std::move(b));
                        else changed = true;
                    f.blocks = std::move(kept);
                }
                // DCE: pure instructions whose results are never used
                {
                    std::unordered_map<std::string, int> uses;
                    for (auto& b : f.blocks)
                        for (auto& m : b.ins) {
                            size_t a0 = m.op == "call" ? 1 : 0;
                            for (size_t i = a0; i < m.args.size(); i++) {
                                std::string tok = m.args[i];
                                size_t sp = tok.find(' ');
                                if (sp != std::string::npos) tok = tok.substr(sp + 1);
                                if (!tok.empty() && tok[0] == '%') uses[tok]++;
                            }
                        }
                    static const std::set<std::string> pure = {
                        "add", "sub", "mul", "div", "rem", "and", "or", "shl",
                        "shr", "sar", "copy", "extuw", "loadl", "loadw",
                        "ceql", "cnel", "ceqw", "cnew", "csltl", "cslel",
                        "csgtl", "csgel"};
                    for (auto& b : f.blocks) {
                        std::vector<MInst> kept;
                        for (auto& m : b.ins) {
                            if (!m.dst.empty() && pure.count(m.op) && uses[m.dst] == 0) {
                                changed = true;
                                continue;
                            }
                            kept.push_back(std::move(m));
                        }
                        b.ins = std::move(kept);
                    }
                }
                if (!changed) break;
            }
        }
    }

    static bool foldOp(const std::string& op, long long x, long long y, long long& r) {
        if (op == "add") r = x + y;
        else if (op == "sub") r = x - y;
        else if (op == "mul") r = x * y;
        else if (op == "div") {
            if (y == 0 || (x == INT64_MIN && y == -1)) return false;
            r = x / y;
        } else if (op == "rem") {
            if (y == 0 || (x == INT64_MIN && y == -1)) return false;
            r = x % y;
        } else if (op == "and") r = x & y;
        else if (op == "or") r = x | y;
        else if (op == "shl") { if (y < 0 || y > 63) return false; r = (long long)((unsigned long long)x << y); }
        else if (op == "sar") { if (y < 0 || y > 63) return false; r = x >> y; }
        else if (op == "shr") { if (y < 0 || y > 63) return false; r = (long long)((unsigned long long)x >> y); }
        else if (op == "ceql" || op == "ceqw") r = x == y;
        else if (op == "cnel" || op == "cnew") r = x != y;
        else if (op == "csltl") r = x < y;
        else if (op == "cslel") r = x <= y;
        else if (op == "csgtl") r = x > y;
        else if (op == "csgel") r = x >= y;
        else return false;
        return true;
    }
};

} // namespace

std::string genQBE(Program& prog, bool optimize) {
    return Codegen().run(prog, optimize);
}
