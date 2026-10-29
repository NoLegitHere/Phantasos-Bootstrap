// Copyright 2026 The Phantasos Authors
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "codegen/x86_64.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <vector>

namespace phs::x86_64 {
namespace {
using mir::Op;
using mir::Term;
using mir::Type;

enum Reg : u8 { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11 };
constexpr std::string_view kRegNames[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11"};
constexpr std::string_view kDword[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d"};
constexpr std::string_view kWord[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w"};
constexpr std::string_view kByte[] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b"};
constexpr Reg kArgs[] = {RDI, RSI, RDX, RCX, R8, R9};

// Where a value is passed: System V gives each word an integer register while
// they last, but a two-word value takes two registers or goes on the stack
// whole. A value over two words is passed by address (abi.md A.6), and so is
// an inout value, which the callee writes back before it returns. A result
// over two words is returned through a hidden pointer in the first argument
// register. Stack words count up from the first stack argument.
struct Place {
    u32 words = 1;
    i32 reg = -1;    // first register in kArgs, or -1 on the stack
    u32 stack = 0;   // first stack word when on the stack
    bool address = false;  // the word is a pointer to the value
};

struct Places {
    std::vector<Place> values;
    u32 stack_words = 0;
};

bool indirect(const mir::Program& program, Type type) { return mir::words(program, type) > 2; }

Places places(const mir::Program& program, const mir::Function& callee, const std::vector<mir::Type>& types) {
    Places out;
    u32 next = indirect(program, callee.result) ? 1 : 0;
    for (u32 i = 0; i < types.size(); ++i) {
        const Type type = types[i];
        Place place{mir::words(program, type)};
        if (indirect(program, type) || mir::is_inout(callee, i)) place = {.words = 1, .address = true};
        if (next + place.words <= std::size(kArgs)) {
            place.reg = static_cast<i32>(next);
            next += place.words;
        } else {
            place.stack = out.stack_words;
            out.stack_words += place.words;
        }
        out.values.push_back(place);
    }
    return out;
}

// Condition codes, as the low nibble of jcc and setcc opcodes.
enum Cond : u8 { O = 0x0, B = 0x2, AE = 0x3, E = 0x4, NE = 0x5, BE = 0x6, A = 0x7, L = 0xC, GE = 0xD, LE = 0xE, G = 0xF };
std::string_view cond_name(Cond cc) {
    switch (cc) {
        case O: return "o";
        case B: return "b";
        case AE: return "ae";
        case E: return "e";
        case NE: return "ne";
        case BE: return "be";
        case A: return "a";
        case L: return "l";
        case GE: return "ge";
        case LE: return "le";
        case G: return "g";
    }
    unreachable();
}

std::string disp(i32 offset) { return offset < 0 ? std::format("[rbp-{}]", -offset) : std::format("[rbp+{}]", offset); }

// Renders data bytes as an assembler db directive.
std::string quoted(std::string_view data) {
    std::string text = "db \"";
    for (const char c : data) {
        if (c == '"' || c == '\\') text += std::format("\\{}", c);
        else if (c == '\n') text += "\\n";
        else if (static_cast<u8>(c) < 0x20 || c == 0x7F) text += std::format("\\x{:02x}", static_cast<u8>(c));
        else text += c;
    }
    return text + '"';
}

// Encodes one function at a time into a shared .text buffer. Jumps use rel32
// fields patched once their labels are placed.
class Assembler {
public:
    Assembler(obj::Object& object, std::string* listing) : object_(object), listing_(listing) {}

    using Label = u32;
    Label label() {
        labels_.push_back(kUnplaced);
        return static_cast<Label>(labels_.size() - 1);
    }
    void place(Label l) { labels_[l] = here(); }
    void reset() {
        labels_.clear();
        fixups_.clear();
    }
    void patch() {
        for (const auto& [field, target] : fixups_) {
            PHS_ASSERT(labels_[target] != kUnplaced);
            put32(field, static_cast<u32>(static_cast<i64>(labels_[target]) - static_cast<i64>(field + 4)));
        }
    }
    u64 here() const { return object_.text.size(); }
    void note(std::string text) {
        if (listing_) lines_.push_back({here(), 0, std::move(text), true});
    }
    // Lists the object's read-only data after its code.
    void rodata() {
        if (object_.rodata.empty()) return;
        note(".rodata");
        const std::string_view bytes(reinterpret_cast<const char*>(object_.rodata.data()), object_.rodata.size());
        for (const auto& symbol : object_.symbols)
            if (symbol.defined && symbol.section == obj::Section::Rodata)
                note(std::format("    {:04x}  {}:  {}", symbol.offset, symbol.name,
                                 quoted(bytes.substr(symbol.offset, symbol.size))));
    }
    std::string render() const {
        std::string out;
        for (usize i = 0; i < lines_.size(); ++i) {
            const auto& line = lines_[i];
            if (line.header) {
                out += line.text + '\n';
                continue;
            }
            std::string bytes;
            for (u64 b = 0; b < line.size; ++b) bytes += std::format("{:02x}", object_.text[line.offset + b]);
            out += std::format("    {:04x}  {:<22} {}\n", line.offset, bytes, line.text);
        }
        return out;
    }

    // ------------------------------------------------------------ moves

    void load(Reg dst, i32 offset) { mem(0x8B, dst, offset, std::format("mov {}, qword {}", name(dst), disp(offset))); }
    void store(i32 offset, Reg src) { mem(0x89, src, offset, std::format("mov qword {}, {}", disp(offset), name(src))); }
    void mov_imm(Reg dst, u64 value) {
        const auto start = begin();
        const auto signed_value = static_cast<i64>(value);
        if (signed_value >= std::numeric_limits<i32>::min() && signed_value <= std::numeric_limits<i32>::max()) {
            rex(true, 0, dst);
            byte(0xC7);
            byte(modrm(0, dst));
            put(static_cast<u32>(value));
        } else {
            rex(true, 0, dst);
            byte(static_cast<u8>(0xB8 + (dst & 7)));
            for (int i = 0; i < 8; ++i) byte(static_cast<u8>(value >> (8 * i)));
        }
        end(start, std::format("mov {}, {}", name(dst), signed_value));
    }
    void mov(Reg dst, Reg src) { rr(0x89, src, dst, std::format("mov {}, {}", name(dst), name(src))); }

    // ------------------------------------------------------------ arithmetic

    // op r/m64, r64 forms: add 01, or 09, and 21, sub 29, xor 31, cmp 39.
    void alu(u8 opcode, std::string_view mnemonic, Reg dst, Reg src) {
        rr(opcode, src, dst, std::format("{} {}, {}", mnemonic, name(dst), name(src)));
    }
    void imul(Reg dst, Reg src) {
        const auto start = begin();
        rex(true, dst, src);
        byte(0x0F);
        byte(0xAF);
        byte(modrm(dst, src));
        end(start, std::format("imul {}, {}", name(dst), name(src)));
    }
    // F7 group: not /2, neg /3, mul /4, div /6, idiv /7.
    void group3(u8 ext, std::string_view mnemonic, Reg r) {
        const auto start = begin();
        rex(true, 0, r);
        byte(0xF7);
        byte(modrm(ext, r));
        end(start, std::format("{} {}", mnemonic, name(r)));
    }
    void cqo() { raw({0x48, 0x99}, "cqo"); }
    void zero_edx() { raw({0x31, 0xD2}, "xor edx, edx"); }
    void flip_bool() { raw({0x83, 0xF0, 0x01}, "xor eax, 1"); }
    void test(Reg r) { rr(0x85, r, r, std::format("test {}, {}", name(r), name(r))); }
    void shr(Reg r, u8 count) {
        const auto start = begin();
        rex(true, 0, r);
        byte(0xC1);
        byte(modrm(5, r));
        byte(count);
        end(start, std::format("shr {}, {}", name(r), count));
    }
    void setcc(Cond cc) {
        raw({0x0F, static_cast<u8>(0x90 + cc), 0xC0}, std::format("set{} al", cond_name(cc)));
        raw({0x0F, 0xB6, 0xC0}, "movzx eax, al");
    }
    // Re-establishes the 64-bit form of a narrow value in r.
    void normalize(Reg r, Type type) {
        const auto start = begin();
        std::string text;
        switch (type) {
            case Type::I8: rex(true, r, r); byte(0x0F); byte(0xBE); text = std::format("movsx {}, {}", name(r), kByte[r]); break;
            case Type::I16: rex(true, r, r); byte(0x0F); byte(0xBF); text = std::format("movsx {}, {}", name(r), kWord[r]); break;
            case Type::I32: rex(true, r, r); byte(0x63); text = std::format("movsxd {}, {}", name(r), kDword[r]); break;
            case Type::U8: rex(false, r, r); byte(0x0F); byte(0xB6); text = std::format("movzx {}, {}", kDword[r], kByte[r]); break;
            case Type::U16: rex(false, r, r); byte(0x0F); byte(0xB7); text = std::format("movzx {}, {}", kDword[r], kWord[r]); break;
            case Type::U32: rex(false, r, r); byte(0x89); text = std::format("mov {}, {}", kDword[r], kDword[r]); break;
            default: return;
        }
        byte(modrm(r, r));
        end(start, std::move(text));
    }

    // ------------------------------------------------------------ control

    void jump(Label target) { branch({0xE9}, target, "jmp"); }
    void jcc(Cond cc, Label target) { branch({0x0F, static_cast<u8>(0x80 + cc)}, target, std::format("j{}", cond_name(cc))); }
    void call(u32 symbol) {
        const auto start = begin();
        byte(0xE8);
        object_.relocations.push_back({here(), symbol, -4});
        put(0u);
        end(start, std::format("call {}", object_.symbols[symbol].name));
    }
    void call(Reg target) {
        const auto start = begin();
        rex(false, 0, target);
        byte(0xFF);
        byte(modrm(2, target));
        end(start, std::format("call {}", name(target)));
    }
    void push(Reg r) {
        const auto start = begin();
        if (r >= R8) byte(0x41);
        byte(static_cast<u8>(0x50 + (r & 7)));
        end(start, std::format("push {}", name(r)));
    }
    void pop(Reg r) {
        const auto start = begin();
        if (r >= R8) byte(0x41);
        byte(static_cast<u8>(0x58 + (r & 7)));
        end(start, std::format("pop {}", name(r)));
    }
    void adjust_rsp(i32 amount) {
        const auto start = begin();
        byte(0x48);
        byte(0x81);
        byte(amount < 0 ? 0xEC : 0xC4);
        put(static_cast<u32>(amount < 0 ? -amount : amount));
        end(start, std::format("{} rsp, {}", amount < 0 ? "sub" : "add", amount < 0 ? -amount : amount));
    }
    void mov_edi(u32 value) {
        const auto start = begin();
        byte(0xBF);
        put(value);
        end(start, std::format("mov edi, {}", value));
    }
    void prologue() {
        raw({0x55}, "push rbp");
        raw({0x48, 0x89, 0xE5}, "mov rbp, rsp");
    }
    void epilogue() {
        raw({0xC9}, "leave");
        raw({0xC3}, "ret");
    }
    void ud2() { raw({0x0F, 0x0B}, "ud2"); }
    void rep_movsb() { raw({0xF3, 0xA4}, "rep movsb"); }

    // ------------------------------------------------------------ runtime

    void mov32_imm(Reg dst, u32 value) {
        const auto start = begin();
        rex(false, 0, dst);
        byte(static_cast<u8>(0xB8 + (dst & 7)));
        put(value);
        end(start, std::format("mov {}, {}", kDword[dst], value));
    }
    void mov32(Reg dst, Reg src) {
        const auto start = begin();
        rex(false, src, dst);
        byte(0x89);
        byte(modrm(src, dst));
        end(start, std::format("mov {}, {}", kDword[dst], kDword[src]));
    }
    void zero32(Reg r) {
        const auto start = begin();
        rex(false, r, r);
        byte(0x31);
        byte(modrm(r, r));
        end(start, std::format("xor {}, {}", kDword[r], kDword[r]));
    }
    void cmp32_imm8(Reg r, u8 value) {
        const auto start = begin();
        rex(false, 0, r);
        byte(0x83);
        byte(modrm(7, r));
        byte(value);
        end(start, std::format("cmp {}, {}", kDword[r], value));
    }
    // lea dst, [rip + symbol]
    void lea(Reg dst, u32 symbol) {
        const auto start = begin();
        rex(true, dst, 0);
        byte(0x8D);
        byte(static_cast<u8>((dst & 7) << 3 | 5));
        object_.relocations.push_back({here(), symbol, -4});
        put(0u);
        end(start, std::format("lea {}, [rip+{}]", name(dst), object_.symbols[symbol].name));
    }
    // mov dst, qword [rip + symbol + extra] and mov qword [rip + symbol + extra], src
    void load(Reg dst, u32 symbol, i32 extra) { rip(0x8B, dst, symbol, extra, "mov {}, qword {}"); }
    void store(u32 symbol, i32 extra, Reg src) { rip(0x89, src, symbol, extra, "mov qword {1}, {0}"); }
    void syscall() { raw({0x0F, 0x05}, "syscall"); }
    void ret() { raw({0xC3}, "ret"); }
    // op r64, imm8 forms of the 83 group: add /0, sub /5, cmp /7.
    void alu_imm8(u8 ext, std::string_view mnemonic, Reg r, std::int8_t value) {
        const auto start = begin();
        rex(true, 0, r);
        byte(0x83);
        byte(modrm(ext, r));
        byte(static_cast<u8>(value));
        end(start, std::format("{} {}, {}", mnemonic, name(r), int{value}));
    }
    // mov byte [base], src for a base other than rsp, rbp, r12 and r13.
    void store_byte(Reg base, Reg src) {
        PHS_ASSERT(base != RSP && base != RBP && src < RSP);
        const auto start = begin();
        rex(false, src, base);
        byte(0x88);
        byte(static_cast<u8>((src & 7) << 3 | (base & 7)));
        end(start, std::format("mov byte [{}], {}", name(base), kByte[src]));
    }


    // ------------------------------------------------------------ aggregates

    // Loads a value of type from memory into dst, sign- or zero-extended to 64 bits.
    void load_field(Reg dst, Reg base, i32 offset, Type type) {
        const auto at = address(base, offset);
        switch (type) {
            case Type::I8: memop(true, {0x0F, 0xBE}, dst, base, offset, std::format("movsx {}, byte {}", name(dst), at)); break;
            case Type::I16: memop(true, {0x0F, 0xBF}, dst, base, offset, std::format("movsx {}, word {}", name(dst), at)); break;
            case Type::I32: memop(true, {0x63}, dst, base, offset, std::format("movsxd {}, dword {}", name(dst), at)); break;
            case Type::Bool: case Type::U8: load_bytes(dst, base, offset, 1); break;
            case Type::U16: load_bytes(dst, base, offset, 2); break;
            case Type::U32: load_bytes(dst, base, offset, 4); break;
            default: load_bytes(dst, base, offset, 8); break;
        }
    }
    // Loads 1, 2, 4 or 8 bytes from memory into dst, zero-extended.
    void load_bytes(Reg dst, Reg base, i32 offset, u32 bytes) {
        const auto at = address(base, offset);
        switch (bytes) {
            case 1: memop(false, {0x0F, 0xB6}, dst, base, offset, std::format("movzx {}, byte {}", kDword[dst], at)); break;
            case 2: memop(false, {0x0F, 0xB7}, dst, base, offset, std::format("movzx {}, word {}", kDword[dst], at)); break;
            case 4: memop(false, {0x8B}, dst, base, offset, std::format("mov {}, dword {}", kDword[dst], at)); break;
            default: memop(true, {0x8B}, dst, base, offset, std::format("mov {}, qword {}", name(dst), at)); break;
        }
    }
    // Stores the low 1, 2, 4 or 8 bytes of src.
    void store_bytes(Reg base, i32 offset, Reg src, u32 bytes) {
        const auto at = address(base, offset);
        switch (bytes) {
            case 1: memop(false, {0x88}, src, base, offset, std::format("mov byte {}, {}", at, kByte[src]), true); break;
            case 2:
                byte(0x66);
                memop(false, {0x89}, src, base, offset, std::format("mov word {}, {}", at, kWord[src]), false, 1);
                break;
            case 4: memop(false, {0x89}, src, base, offset, std::format("mov dword {}, {}", at, kDword[src])); break;
            default: memop(true, {0x89}, src, base, offset, std::format("mov qword {}, {}", at, name(src))); break;
        }
    }
    // lea dst, [base + offset]
    void lea(Reg dst, Reg base, i32 offset) {
        memop(true, {0x8D}, dst, base, offset, std::format("lea {}, {}", name(dst), address(base, offset)));
    }

private:
    static constexpr u64 kUnplaced = std::numeric_limits<u64>::max();
    struct Line {
        u64 offset, size;
        std::string text;
        bool header = false;
    };

    static std::string_view name(Reg r) { return kRegNames[r]; }
    static u8 modrm(u8 reg, u8 rm) { return static_cast<u8>(0xC0 | (reg & 7) << 3 | (rm & 7)); }
    void byte(u8 b) { object_.text.push_back(b); }
    void put(u32 v) {
        for (int i = 0; i < 4; ++i) byte(static_cast<u8>(v >> (8 * i)));
    }
    void put32(u64 at, u32 v) {
        for (int i = 0; i < 4; ++i) object_.text[at + i] = static_cast<u8>(v >> (8 * i));
    }
    void rex(bool wide, u8 reg, u8 rm) {
        const u8 prefix = static_cast<u8>(0x40 | (wide ? 8 : 0) | (reg >= 8 ? 4 : 0) | (rm >= 8 ? 1 : 0));
        if (prefix != 0x40) byte(prefix);
    }
    u64 begin() const { return here(); }
    void end(u64 start, std::string text) {
        if (listing_) lines_.push_back({start, here() - start, std::move(text)});
    }
    void raw(std::initializer_list<u8> bytes, std::string text) {
        const auto start = begin();
        for (const u8 b : bytes) byte(b);
        end(start, std::move(text));
    }
    void rr(u8 opcode, Reg reg, Reg rm, std::string text) {
        const auto start = begin();
        rex(true, reg, rm);
        byte(opcode);
        byte(modrm(reg, rm));
        end(start, std::move(text));
    }
    // [rbp + disp32] operand.
    void mem(u8 opcode, Reg reg, i32 offset, std::string text) {
        const auto start = begin();
        rex(true, reg, RBP);
        byte(opcode);
        byte(static_cast<u8>(0x80 | (reg & 7) << 3 | RBP));
        put(static_cast<u32>(offset));
        end(start, std::move(text));
    }
    static std::string address(Reg base, i32 offset) {
        if (base == RBP) return disp(offset);
        if (offset == 0) return std::format("[{}]", name(base));
        return offset < 0 ? std::format("[{}-{}]", name(base), -offset) : std::format("[{}+{}]", name(base), offset);
    }
    // [base + disp32] operand for a base other than rsp and r12, which would need
    // a SIB byte. A byte register above bl needs a REX prefix to be addressed;
    // prefixed counts bytes already emitted for this instruction.
    void memop(bool wide, std::initializer_list<u8> opcode, Reg reg, Reg base, i32 offset, std::string text,
               bool byte_reg = false, u64 prefixed = 0) {
        PHS_ASSERT((base & 7) != RSP);
        const auto start = begin() - prefixed;
        const u8 prefix = static_cast<u8>(0x40 | (wide ? 8 : 0) | (reg >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
        if (prefix != 0x40 || (byte_reg && reg >= RSP)) byte(prefix);
        for (const u8 b : opcode) byte(b);
        byte(static_cast<u8>(0x80 | (reg & 7) << 3 | (base & 7)));
        put(static_cast<u32>(offset));
        end(start, std::move(text));
    }
    void rip(u8 opcode, Reg reg, u32 symbol, i32 extra, std::string_view text) {
        const auto start = begin();
        rex(true, reg, 0);
        byte(opcode);
        byte(static_cast<u8>((reg & 7) << 3 | 5));
        object_.relocations.push_back({here(), symbol, i64{extra} - 4});
        put(0u);
        const auto at = extra ? std::format("[rip+{}+{}]", object_.symbols[symbol].name, extra)
                              : std::format("[rip+{}]", object_.symbols[symbol].name);
        const auto reg_name = name(reg);
        end(start, std::vformat(text, std::make_format_args(reg_name, at)));
    }
    void branch(std::initializer_list<u8> opcode, Label target, std::string mnemonic) {
        const auto start = begin();
        for (const u8 b : opcode) byte(b);
        fixups_.push_back({here(), target});
        put(0u);
        end(start, std::format("{} .L{}", mnemonic, target));
    }

    obj::Object& object_;
    std::string* listing_;
    std::vector<u64> labels_;
    std::vector<std::pair<u64, Label>> fixups_;
    std::vector<Line> lines_;
};

class Emitter {
public:
    Emitter(const mir::Program& program, obj::Object& object, std::string* listing)
        : program_(program), object_(object), as_(object, listing) {}

    void run() {
        for (const auto& fun : program_.functions) object_.symbols.push_back({symbol_name(fun), true});
        trap_ = static_cast<u32>(object_.symbols.size());
        object_.symbols.push_back({std::string(kTrapSymbol), false});
        // A function used as a value is called through a thunk that takes
        // the environment first and drops it.
        std::vector<u32> targets;
        for (const auto& fun : program_.functions)
            for (const auto& block : fun.blocks)
                for (const auto& inst : block.insts)
                    if (inst.op == Op::FnRef) targets.push_back(inst.callee);
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        std::vector<mir::Function> thunks;
        for (const u32 target : targets) {
            thunks_.emplace(target, static_cast<u32>(object_.symbols.size()));
            object_.symbols.push_back({thunk_name(program_.functions[target]), true});
            thunks.push_back(thunk(target));
        }
        for (usize i = 0; i < program_.functions.size(); ++i)
            function(program_.functions[i], static_cast<u32>(i));
        for (usize i = 0; i < thunks.size(); ++i) function(thunks[i], thunks_.at(targets[i]));
        as_.rodata();
    }

    std::string listing() const { return as_.render(); }

private:
    // A register's lowest address: a String's length and a struct's later
    // bytes lie above it.
    i32 slot(mir::Reg r) const { return slots_[r.id]; }

    // A thunk is MIR too: its parameters are the environment and then the
    // target's, and it returns what the target returns.
    mir::Function thunk(u32 target) const {
        const auto& callee = program_.functions[target];
        mir::Function out;
        out.name = callee.name + "$thunk";
        out.span = callee.span;
        out.regs.push_back(Type::U64);
        mir::Inst call{.op = Op::Call, .callee = target};
        for (u32 p = 0; p < callee.params; ++p) {
            out.regs.push_back(callee.regs[p]);
            call.args.push_back({p + 1});
        }
        out.params = callee.params + 1;
        out.result = callee.result;
        if (mir::size_of(program_, callee.result)) {
            call.dst = {static_cast<u32>(out.regs.size())};
            out.regs.push_back(callee.result);
        }
        auto& block = out.blocks.emplace_back();
        block.term = callee.result == Type::Never ? Term::Unreachable : Term::Ret;
        block.value = call.dst;
        block.insts.push_back(std::move(call));
        return out;
    }

    void function(const mir::Function& fun, u32 index) {
        // Keep functions 16-byte aligned; int3 fills the gap. Literals add
        // symbols as the body is emitted, so the function's is not held by reference.
        while (object_.text.size() % 16) object_.text.push_back(0xCC);
        const u64 offset = as_.here();
        object_.symbols[index].offset = offset;
        as_.note(std::format("{}:  ; {}", object_.symbols[index].name, fun.name));
        as_.reset();
        blocks_.clear();
        for (usize b = 0; b < fun.blocks.size(); ++b) blocks_.push_back(as_.label());
        overflow_ = as_.label();
        divide_ = as_.label();
        used_overflow_ = used_divide_ = false;
        fun_ = &fun;

        slots_.clear();
        i32 used = 0;
        for (const auto type : fun.regs) {
            used += 8 * static_cast<i32>(std::max(words(type), 1u));
            slots_.push_back(-used);
        }
        const bool sret = indirect(program_, fun.result);
        if (sret) {
            used += 8;
            result_ = -used;
        }
        // Where each inout parameter's value goes back to.
        written_.clear();
        for (u32 p = 0; p < fun.params; ++p)
            if (mir::is_inout(fun, p)) {
                used += 8;
                written_.push_back({p, -used});
            }
        as_.prologue();
        const auto frame = (used + 15) / 16 * 16;
        if (frame) as_.adjust_rsp(-frame);
        if (sret) as_.store(result_, RDI);
        const auto params = places(program_, fun, std::vector<Type>(fun.regs.begin(), fun.regs.begin() + fun.params));
        // Registers first, so that copying a value passed by address cannot clobber them.
        for (u32 p = 0; p < fun.params; ++p) {
            const auto& place = params.values[p];
            for (u32 w = 0; place.reg >= 0 && w < place.words; ++w)
                as_.store(slot({p}) + 8 * static_cast<i32>(w), kArgs[place.reg + w]);
        }
        for (u32 p = 0; p < fun.params; ++p) {
            const auto& place = params.values[p];
            // Stack arguments sit above the return address and saved rbp.
            const auto stack = [&](u32 w) { return 16 + 8 * static_cast<i32>(place.stack + w); };
            if (place.address) {
                as_.load(RSI, place.reg >= 0 ? slot({p}) : stack(0));
                for (const auto& [param, at] : written_)
                    if (param == p) as_.store(at, RSI);
                copy(RBP, slot({p}), RSI, 0, size(fun.regs[p]));
            } else if (place.reg < 0) {
                for (u32 w = 0; w < place.words; ++w) {
                    as_.load(RAX, stack(w));
                    as_.store(slot({p}) + 8 * static_cast<i32>(w), RAX);
                }
            }
        }
        for (usize b = 0; b < fun.blocks.size(); ++b) {
            as_.place(blocks_[b]);
            as_.note(std::format("  bb{}:  ; .L{}", b, blocks_[b]));
            const auto& block = fun.blocks[b];
            for (const auto& inst : block.insts) instruction(inst);
            terminator(block, static_cast<u32>(b));
        }
        trap_path(overflow_, used_overflow_, Trap::Overflow, "overflow");
        trap_path(divide_, used_divide_, Trap::DivideByZero, "division by zero");
        as_.patch();
        object_.symbols[index].size = as_.here() - offset;
    }

    void trap_path(Assembler::Label label, bool used, Trap code, std::string_view what) {
        if (!used) return;
        as_.place(label);
        as_.note(std::format("  .L{}:  ; trap: {}", label, what));
        as_.mov_edi(static_cast<u32>(code));
        as_.call(trap_);
        as_.ud2();
    }

    void overflow(Cond cc) {
        used_overflow_ = true;
        as_.jcc(cc, overflow_);
    }

    // Traps unless rax already holds a normalized value of type.
    void range_check(Type type) {
        const u32 width = mir::bit_width(type);
        if (width == 64) return;
        as_.mov(RDX, RAX);
        if (mir::is_signed(type)) {
            as_.normalize(RDX, type);
            as_.alu(0x39, "cmp", RDX, RAX);
        } else as_.shr(RDX, static_cast<u8>(width));
        overflow(NE);
    }

    u32 size(Type type) const { return mir::size_of(program_, type); }
    u32 words(Type type) const { return mir::words(program_, type); }

    // Copies bytes from [src + from] to [dst + to] through rax.
    void copy(Reg dst, i32 to, Reg src, i32 from, u32 bytes) {
        for (u32 done = 0; done < bytes;) {
            const u32 left = bytes - done;
            const u32 chunk = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
            as_.load_bytes(RAX, src, from + static_cast<i32>(done), chunk);
            as_.store_bytes(dst, to + static_cast<i32>(done), RAX, chunk);
            done += chunk;
        }
    }

    // Clears bytes from [rbp + at] through rax.
    void zero(i32 at, u32 bytes) {
        as_.mov_imm(RAX, 0);
        for (u32 done = 0; done < bytes;) {
            const u32 left = bytes - done;
            const u32 chunk = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
            as_.store_bytes(RBP, at + static_cast<i32>(done), RAX, chunk);
            done += chunk;
        }
    }

    // Scalars sit in their slots as normalized 64-bit values; inside an
    // aggregate they take their natural size.
    static bool scalar(Type type) { return type != Type::Str && !mir::is_aggregate(type); }

    void make(const mir::Inst& inst) {
        const auto& layout = program_.layouts[mir::layout_index(fun_->regs[inst.dst.id])];
        for (usize i = 0; i < inst.args.size(); ++i) {
            const i32 at = slot(inst.dst) + static_cast<i32>(layout.offsets[i]);
            const Type type = layout.fields[i];
            if (scalar(type)) {
                as_.load(RAX, slot(inst.args[i]));
                as_.store_bytes(RBP, at, RAX, size(type));
            } else copy(RBP, at, RBP, slot(inst.args[i]), size(type));
        }
    }

    void field(const mir::Inst& inst) {
        const auto& layout = program_.layouts[mir::layout_index(fun_->regs[inst.a.id])];
        const i32 at = slot(inst.a) + static_cast<i32>(layout.offsets[inst.imm]);
        const Type type = layout.fields[inst.imm];
        if (scalar(type)) {
            as_.load_field(RAX, RBP, at, type);
            as_.store(slot(inst.dst), RAX);
        } else copy(RBP, slot(inst.dst), RBP, at, size(type));
    }

    // A copy of the aggregate with one field replaced; in place when dst is a.
    void with(const mir::Inst& inst) {
        const Type type = fun_->regs[inst.a.id];
        const auto& layout = program_.layouts[mir::layout_index(type)];
        if (inst.dst != inst.a) copy(RBP, slot(inst.dst), RBP, slot(inst.a), size(type));
        const i32 at = slot(inst.dst) + static_cast<i32>(layout.offsets[inst.imm]);
        const Type field = layout.fields[inst.imm];
        if (scalar(field)) {
            as_.load(RAX, slot(inst.b));
            as_.store_bytes(RBP, at, RAX, size(field));
        } else copy(RBP, at, RBP, slot(inst.b), size(field));
    }

    void instruction(const mir::Inst& inst) {
        if (inst.op == Op::Call) return call(inst);
        if (inst.op == Op::FnRef) return function_value(inst);
        if (inst.op == Op::CallIndirect) return call_indirect(inst);
        if (inst.op == Op::Closure) return closure(inst);
        if (inst.op == Op::Env) return env(inst);
        if (inst.op == Op::Print) return print(inst);
        if (inst.op == Op::Make) return make(inst);
        if (inst.op == Op::Field) return field(inst);
        if (inst.op == Op::With) return with(inst);
        if (inst.op == Op::None) return zero(slot(inst.dst), size(fun_->regs[inst.dst.id]));
        if (inst.op == Op::Array) return array(inst);
        if (inst.op == Op::Load || inst.op == Op::Store) return element(inst);
        if (inst.op == Op::Reserve) return reserve(inst);
        const Type type = fun_->regs[inst.dst.id];
        if (mir::is_aggregate(type)) {
            PHS_ASSERT(inst.op == Op::Copy);
            return copy(RBP, slot(inst.dst), RBP, slot(inst.a), size(type));
        }
        if (type == Type::Str) return string(inst);
        const Type operand = inst.a.valid() ? fun_->regs[inst.a.id] : type;
        const bool sign = mir::is_signed(operand);
        const bool wide = mir::bit_width(type) == 64;
        if (inst.a.valid()) as_.load(RAX, slot(inst.a));
        if (inst.b.valid()) as_.load(RCX, slot(inst.b));
        switch (inst.op) {
            case Op::Const: as_.mov_imm(RAX, inst.imm); break;
            case Op::Copy: case Op::Widen: break;
            case Op::Truncate: as_.normalize(RAX, type); break;
            case Op::Neg:
                as_.group3(3, "neg", RAX);
                if (wide) overflow(O);
                else range_check(type);
                break;
            case Op::Not: as_.flip_bool(); break;
            case Op::BitNot:
                as_.group3(2, "not", RAX);
                as_.normalize(RAX, type);
                break;
            case Op::Add: case Op::Sub:
                as_.alu(inst.op == Op::Add ? 0x01 : 0x29, inst.op == Op::Add ? "add" : "sub", RAX, RCX);
                if (wide) overflow(sign ? O : B);
                else range_check(type);
                break;
            case Op::Mul:
                if (wide && !sign) {
                    as_.group3(4, "mul", RCX);  // rdx:rax; OF is set when rdx is not zero
                    overflow(O);
                } else {
                    as_.imul(RAX, RCX);
                    if (wide) overflow(O);
                    else range_check(type);
                }
                break;
            case Op::Div: case Op::Rem: divide(inst.op, type, sign); break;
            case Op::WrapAdd: as_.alu(0x01, "add", RAX, RCX); as_.normalize(RAX, type); break;
            case Op::WrapSub: as_.alu(0x29, "sub", RAX, RCX); as_.normalize(RAX, type); break;
            case Op::WrapMul: as_.imul(RAX, RCX); as_.normalize(RAX, type); break;
            case Op::And: as_.alu(0x21, "and", RAX, RCX); break;
            case Op::Or: as_.alu(0x09, "or", RAX, RCX); break;
            case Op::Xor: as_.alu(0x31, "xor", RAX, RCX); break;
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
                as_.alu(0x39, "cmp", RAX, RCX);
                as_.setcc(compare(inst.op, sign));
                break;
            }
            case Op::Call: case Op::Str: case Op::Print: case Op::Make: case Op::Field: case Op::With:
            case Op::FnRef: case Op::CallIndirect: case Op::Closure: case Op::Env: case Op::None:
            case Op::Array: case Op::Load: case Op::Store: case Op::Reserve: break;
        }
        as_.store(slot(inst.dst), RAX);
    }

    // A literal's address and length, or a copy of both words.
    void string(const mir::Inst& inst) {
        if (inst.op == Op::Str) {
            const auto& bytes = program_.strings[inst.imm];
            as_.lea(RAX, object_.literal(bytes));
            as_.store(slot(inst.dst), RAX);
            as_.mov_imm(RAX, bytes.size());
        } else {
            PHS_ASSERT(inst.op == Op::Copy);
            as_.load(RAX, slot(inst.a));
            as_.store(slot(inst.dst), RAX);
            as_.load(RAX, slot(inst.a) + 8);
        }
        as_.store(slot(inst.dst) + 8, RAX);
    }

    static Cond compare(Op op, bool sign) {
        switch (op) {
            case Op::Eq: return E;
            case Op::Ne: return NE;
            case Op::Lt: return sign ? L : B;
            case Op::Le: return sign ? LE : BE;
            case Op::Gt: return sign ? G : A;
            case Op::Ge: return sign ? GE : AE;
            default: unreachable();
        }
    }

    // rax / rcx. A zero divisor traps, and so does the signed minimum divided
    // by -1, for both quotient and remainder.
    void divide(Op op, Type type, bool sign) {
        as_.test(RCX);
        used_divide_ = true;
        as_.jcc(E, divide_);
        if (sign) {
            const auto ok = as_.label();
            const u32 width = mir::bit_width(type);
            as_.mov_imm(RDX, ~u64{0});
            as_.alu(0x39, "cmp", RCX, RDX);
            as_.jcc(NE, ok);
            as_.mov_imm(RDX, ~u64{0} << (width - 1));
            as_.alu(0x39, "cmp", RAX, RDX);
            overflow(E);
            as_.place(ok);
            as_.cqo();
            as_.group3(7, "idiv", RCX);
        } else {
            as_.zero_edx();
            as_.group3(6, "div", RCX);
        }
        if (op == Op::Rem) as_.mov(RAX, RDX);
    }

    // print: the runtime entry for the argument's type (Int, UInt, Bool or String).
    void print(const mir::Inst& inst) {
        const Type type = fun_->regs[inst.a.id];
        const auto entry = type == Type::Str ? kPrintString
                         : type == Type::Bool ? kPrintBool
                         : mir::is_signed(type) ? kPrintInt
                                                : kPrintUInt;
        PHS_ASSERT(type == Type::Str || type == Type::Bool || type == Type::I64 || type == Type::U64);
        as_.load(RDI, slot(inst.a));
        if (type == Type::Str) as_.load(RSI, slot(inst.a) + 8);
        as_.call(runtime(entry));
    }

    // The symbol of a runtime entry, added when it is first used.
    u32 runtime(std::string_view entry) {
        auto [found, added] = runtime_.try_emplace(entry, static_cast<u32>(object_.symbols.size()));
        if (added) object_.symbols.push_back({std::string(entry), false});
        return found->second;
    }

    // A closure's value: its function's code and an environment allocated
    // from the runtime and filled with the captured values, or a null one.
    void closure(const mir::Inst& inst) {
        const auto& callee = program_.functions[inst.callee];
        if (callee.environment) {
            const auto& layout = program_.layouts[*callee.environment];
            as_.mov_imm(RDI, layout.size);
            as_.call(runtime(kAlloc));
            as_.store(slot(inst.dst) + 8, RAX);
            as_.mov(RDX, RAX);
            for (usize i = 0; i < inst.args.size(); ++i) {
                const auto at = static_cast<i32>(layout.offsets[i]);
                const Type type = layout.fields[i];
                if (scalar(type)) {
                    as_.load(RAX, slot(inst.args[i]));
                    as_.store_bytes(RDX, at, RAX, size(type));
                } else copy(RDX, at, RBP, slot(inst.args[i]), size(type));
            }
        } else {
            as_.zero32(RAX);
            as_.store(slot(inst.dst) + 8, RAX);
        }
        as_.lea(RAX, inst.callee);
        as_.store(slot(inst.dst), RAX);
    }

    // A new array: its elements in memory from _P$alloc, each its size
    // apart, and a count and capacity of their number. An empty array is
    // all zero and allocates nothing.
    void array(const mir::Inst& inst) {
        const auto& layout = program_.layouts[mir::layout_index(fun_->regs[inst.dst.id])];
        if (inst.args.empty()) return zero(slot(inst.dst), layout.size);
        const u32 stride = size(layout.element);
        as_.mov_imm(RDI, u64{stride} * inst.args.size());
        as_.call(runtime(kAlloc));
        as_.store(slot(inst.dst) + static_cast<i32>(layout.offsets[0]), RAX);
        as_.mov(RDX, RAX);
        for (usize i = 0; i < inst.args.size(); ++i) {
            const auto at = static_cast<i32>(stride * i);
            if (scalar(layout.element)) {
                as_.load(RAX, slot(inst.args[i]));
                as_.store_bytes(RDX, at, RAX, stride);
            } else copy(RDX, at, RBP, slot(inst.args[i]), stride);
        }
        as_.mov_imm(RAX, inst.args.size());
        as_.store(slot(inst.dst) + static_cast<i32>(layout.offsets[1]), RAX);
        as_.store(slot(inst.dst) + static_cast<i32>(layout.offsets[2]), RAX);
    }

    // An array with room for one more element. A full one gets new memory
    // from _P$alloc with twice its capacity, or 4 when that was 0, and its
    // elements are copied there; the old memory is never freed.
    void reserve(const mir::Inst& inst) {
        const Type type = fun_->regs[inst.a.id];
        const auto& layout = program_.layouts[mir::layout_index(type)];
        if (inst.dst != inst.a) copy(RBP, slot(inst.dst), RBP, slot(inst.a), layout.size);
        const i32 data = slot(inst.dst) + static_cast<i32>(layout.offsets[0]);
        const i32 count = slot(inst.dst) + static_cast<i32>(layout.offsets[1]);
        const i32 capacity = slot(inst.dst) + static_cast<i32>(layout.offsets[2]);
        const u32 stride = size(layout.element);
        const auto done = as_.label(), doubled = as_.label();
        as_.load(RAX, count);
        as_.load(RCX, capacity);
        as_.alu(0x39, "cmp", RAX, RCX);
        as_.jcc(L, done);
        as_.alu(0x01, "add", RCX, RCX);
        as_.jcc(NE, doubled);
        as_.mov_imm(RCX, 4);
        as_.place(doubled);
        as_.store(capacity, RCX);
        as_.mov_imm(RAX, stride);
        as_.imul(RCX, RAX);
        as_.mov(RDI, RCX);
        as_.call(runtime(kAlloc));
        as_.load(RSI, data);
        as_.store(data, RAX);
        as_.mov(RDI, RAX);
        as_.load(RCX, count);
        as_.mov_imm(RAX, stride);
        as_.imul(RCX, RAX);
        as_.rep_movsb();
        as_.place(done);
    }

    // Load and Store: the element's address, data + index * size, in rdx.
    void element(const mir::Inst& inst) {
        const auto& layout = program_.layouts[mir::layout_index(fun_->regs[inst.a.id])];
        const Type type = layout.element;
        as_.load(RAX, slot(inst.b));
        as_.mov_imm(RCX, size(type));
        as_.imul(RAX, RCX);
        as_.load(RDX, slot(inst.a) + static_cast<i32>(layout.offsets[0]));
        as_.alu(0x01, "add", RDX, RAX);
        if (inst.op == Op::Load) {
            if (scalar(type)) {
                as_.load_field(RAX, RDX, 0, type);
                as_.store(slot(inst.dst), RAX);
            } else copy(RBP, slot(inst.dst), RDX, 0, size(type));
        } else if (scalar(type)) {
            as_.load(RAX, slot(inst.args.front()));
            as_.store_bytes(RDX, 0, RAX, size(type));
        } else copy(RDX, 0, RBP, slot(inst.args.front()), size(type));
    }

    // A captured value, read from the environment that a points to.
    void env(const mir::Inst& inst) {
        const auto& layout = program_.layouts[*fun_->environment];
        const auto at = static_cast<i32>(layout.offsets[inst.imm]);
        const Type type = layout.fields[inst.imm];
        as_.load(RDX, slot(inst.a));
        if (scalar(type)) {
            as_.load_field(RAX, RDX, at, type);
            as_.store(slot(inst.dst), RAX);
        } else copy(RBP, slot(inst.dst), RDX, at, size(type));
    }

    // The value of a function used as a value: its thunk and a null environment.
    void function_value(const mir::Inst& inst) {
        as_.lea(RAX, thunks_.at(inst.callee));
        as_.store(slot(inst.dst), RAX);
        as_.zero32(RAX);
        as_.store(slot(inst.dst) + 8, RAX);
    }

    struct Argument {
        Type type;
        i32 at;  // the value's slot
    };

    void call(const mir::Inst& inst) {
        std::vector<Argument> args;
        for (const auto arg : inst.args) args.push_back({fun_->regs[arg.id], slot(arg)});
        call(program_.functions[inst.callee], args, inst.dst, [&] { as_.call(inst.callee); });
    }

    // A function value's code gets its environment as the first argument
    // after ret* (abi.md A.6), and is called through r11, which no argument uses.
    void call_indirect(const mir::Inst& inst) {
        const auto& layout = program_.layouts[mir::layout_index(fun_->regs[inst.a.id])];
        mir::Function signature;
        signature.result = layout.result;
        std::vector<Argument> args{{Type::U64, slot(inst.a) + 8}};
        for (const auto arg : inst.args) args.push_back({fun_->regs[arg.id], slot(arg)});
        call(signature, args, inst.dst, [&] {
            as_.load(R11, slot(inst.a));
            as_.call(R11);
        });
    }

    template <typename Emit>
    void call(const mir::Function& callee, const std::vector<Argument>& values, mir::Reg dst, Emit emit) {
        std::vector<Type> types;
        for (const auto& value : values) types.push_back(value.type);
        const bool sret = indirect(program_, callee.result);
        const auto args = places(program_, callee, types);
        // rsp stays 16-byte aligned at the call.
        const i32 pad = args.stack_words % 2 ? 8 : 0;
        if (pad) as_.adjust_rsp(-pad);
        for (usize i = values.size(); i-- > 0;) {
            const auto& place = args.values[i];
            if (place.reg >= 0) continue;
            for (u32 w = place.words; w-- > 0;) {
                if (place.address) as_.lea(RAX, RBP, values[i].at);
                else as_.load(RAX, values[i].at + 8 * static_cast<i32>(w));
                as_.push(RAX);
            }
        }
        for (usize i = 0; i < values.size(); ++i) {
            const auto& place = args.values[i];
            if (place.reg < 0) continue;
            if (place.address) as_.lea(kArgs[place.reg], RBP, values[i].at);
            else
                for (u32 w = 0; w < place.words; ++w)
                    as_.load(kArgs[place.reg + w], values[i].at + 8 * static_cast<i32>(w));
        }
        // The callee writes a large result straight into its register's slot.
        if (sret) as_.lea(RDI, RBP, slot(dst));
        emit();
        if (const auto cleanup = static_cast<i32>(8 * args.stack_words) + pad) as_.adjust_rsp(cleanup);
        if (!dst.valid() || sret) return;
        as_.store(slot(dst), RAX);
        if (words(fun_->regs[dst.id]) == 2) as_.store(slot(dst) + 8, RDX);
    }

    void terminator(const mir::Block& block, u32 index) {
        switch (block.term) {
            case Term::Br:
                if (block.target != index + 1) as_.jump(blocks_[block.target]);
                break;
            case Term::CondBr:
                as_.load(RAX, slot(block.value));
                as_.test(RAX);
                if (block.other == index + 1) as_.jcc(NE, blocks_[block.target]);
                else if (block.target == index + 1) as_.jcc(E, blocks_[block.other]);
                else {
                    as_.jcc(NE, blocks_[block.target]);
                    as_.jump(blocks_[block.other]);
                }
                break;
            case Term::Ret:
                for (const auto& [param, at] : written_) {
                    as_.load(RSI, at);
                    copy(RSI, 0, RBP, slot({param}), size(fun_->regs[param]));
                }
                if (block.value.valid()) {
                    const Type type = fun_->regs[block.value.id];
                    if (indirect(program_, type)) {
                        // Copy to *ret* and return its address.
                        as_.load(RDX, result_);
                        copy(RDX, 0, RBP, slot(block.value), size(type));
                        as_.mov(RAX, RDX);
                    } else {
                        as_.load(RAX, slot(block.value));
                        if (words(type) == 2) as_.load(RDX, slot(block.value) + 8);
                    }
                }
                as_.epilogue();
                break;
            case Term::Unreachable: as_.ud2(); break;
            case Term::Panic: {
                const auto& message = program_.strings[block.message];
                as_.lea(RSI, object_.literal(message));
                as_.mov32_imm(RDX, static_cast<u32>(message.size()));
                as_.mov_edi(static_cast<u32>(Trap::Panic));
                as_.call(trap_);
                as_.ud2();
                break;
            }
        }
    }

    const mir::Program& program_;
    obj::Object& object_;
    Assembler as_;
    const mir::Function* fun_ = nullptr;
    std::vector<i32> slots_;
    i32 result_ = 0;  // the slot holding a large result's destination address
    std::vector<std::pair<u32, i32>> written_;  // inout parameters and the slots holding their addresses
    std::unordered_map<std::string_view, u32> runtime_;  // print entries by name
    std::unordered_map<u32, u32> thunks_;  // function index to its thunk's symbol
    std::vector<Assembler::Label> blocks_;
    Assembler::Label overflow_ = 0, divide_ = 0;
    bool used_overflow_ = false, used_divide_ = false;
    u32 trap_ = 0;
};

// Linux x86_64 system call numbers.
constexpr u32 kSysWrite = 1;
constexpr u32 kSysMmap = 9;
constexpr u32 kSysExitGroup = 231;
constexpr u32 kChunk = 1 << 20;  // the allocator's smallest mmap request
constexpr std::int8_t kEintr = 4;

class Runtime {
public:
    Runtime(const mir::Program& program, obj::Object& object, std::string* listing)
        : program_(program), object_(object), as_(object, listing) {}

    void run() {
        const auto& main = program_.functions[program_.main];
        const u32 start = symbol(std::string(kEntrySymbol), true);
        const u32 trap = symbol(std::string(kTrapSymbol), true);
        const u32 write = symbol("_P$write", true);
        const u32 decimal = symbol("_P$print_decimal", true);
        const u32 print_string = symbol(std::string(kPrintString), true);
        const u32 print_int = symbol(std::string(kPrintInt), true);
        const u32 print_uint = symbol(std::string(kPrintUInt), true);
        const u32 print_bool = symbol(std::string(kPrintBool), true);
        const u32 main_symbol = symbol(symbol_name(main), false);
        static constexpr std::string_view kOverflow = "panic: arithmetic overflow\n";
        static constexpr std::string_view kDivide = "panic: division by zero\n";
        static constexpr std::string_view kWrite = "panic: cannot write to standard output\n";
        static constexpr std::string_view kMemory = "panic: out of memory\n";
        const u32 overflow = object_.literal(kOverflow);
        const u32 divide = object_.literal(kDivide);
        const u32 write_failed = object_.literal(kWrite);
        const u32 newline = object_.literal("\n");
        const u32 yes = object_.literal("true\n");
        const u32 no = object_.literal("false\n");
        const bool allocates = std::ranges::any_of(program_.functions, [&](const mir::Function& fun) {
            return std::ranges::any_of(fun.blocks, [&](const mir::Block& block) {
                return std::ranges::any_of(block.insts, [&](const mir::Inst& inst) {
                    return (inst.op == Op::Closure && program_.functions[inst.callee].environment.has_value()) ||
                           (inst.op == Op::Array && !inst.args.empty()) || inst.op == Op::Reserve;
                });
            });
        });
        const u32 alloc = allocates ? symbol(std::string(kAlloc), true) : 0;
        const u32 memory = allocates ? object_.literal(kMemory) : 0;

        // The kernel enters with rsp 16-byte aligned, so the call leaves main
        // with the alignment System V expects.
        begin(start, "process entry");
        as_.zero32(RBP);
        as_.call(main_symbol);
        if (mir::bit_width(main.result) != 0) as_.mov32(RDI, RAX);
        else as_.zero32(RDI);
        exit();
        end(start);

        begin(trap, "edi holds the trap code; a panic's message is in rsi and rdx");
        const auto not_divide = as_.label(), report = as_.label();
        as_.cmp32_imm8(RDI, static_cast<u8>(Trap::Panic));
        as_.jcc(E, report);
        as_.lea(RSI, overflow);
        as_.mov32_imm(RDX, static_cast<u32>(kOverflow.size()));
        as_.cmp32_imm8(RDI, static_cast<u8>(Trap::DivideByZero));
        as_.jcc(NE, not_divide);
        as_.lea(RSI, divide);
        as_.mov32_imm(RDX, static_cast<u32>(kDivide.size()));
        as_.place(not_divide);
        as_.cmp32_imm8(RDI, static_cast<u8>(Trap::WriteFailed));
        if (allocates) {
            const auto not_write = as_.label();
            as_.jcc(NE, not_write);
            as_.lea(RSI, write_failed);
            as_.mov32_imm(RDX, static_cast<u32>(kWrite.size()));
            as_.place(not_write);
            as_.cmp32_imm8(RDI, static_cast<u8>(Trap::OutOfMemory));
            as_.jcc(NE, report);
            as_.lea(RSI, memory);
            as_.mov32_imm(RDX, static_cast<u32>(kMemory.size()));
        } else {
            as_.jcc(NE, report);
            as_.lea(RSI, write_failed);
            as_.mov32_imm(RDX, static_cast<u32>(kWrite.size()));
        }
        as_.place(report);
        as_.mov32_imm(RDI, 2);  // stderr
        as_.mov32_imm(RAX, kSysWrite);
        as_.syscall();
        as_.mov32_imm(RDI, kPanicExitStatus);
        exit();
        as_.patch();
        end(trap);

        begin(write, "writes rsi bytes at rdi to stdout");
        const auto next = as_.label(), done = as_.label(), failed = as_.label();
        as_.mov(R8, RDI);
        as_.mov(R9, RSI);
        as_.place(next);
        as_.test(R9);
        as_.jcc(E, done);
        as_.mov32_imm(RDI, 1);  // stdout
        as_.mov(RSI, R8);
        as_.mov(RDX, R9);
        as_.mov32_imm(RAX, kSysWrite);
        as_.syscall();
        as_.alu_imm8(7, "cmp", RAX, -kEintr);
        as_.jcc(E, next);
        as_.test(RAX);
        as_.jcc(LE, failed);  // an error, or no progress
        as_.alu(0x01, "add", R8, RAX);
        as_.alu(0x29, "sub", R9, RAX);
        as_.jump(next);
        as_.place(done);
        as_.ret();
        as_.place(failed);
        as_.mov32_imm(RDI, static_cast<u32>(Trap::WriteFailed));
        as_.call(trap);
        as_.ud2();
        as_.patch();
        end(write);

        // Digits are written backwards into a stack buffer that ends with the
        // newline; 20 digits, a sign and the newline fit in 32 bytes.
        begin(decimal, "rdi: magnitude, rsi: nonzero when negative");
        const auto digit = as_.label(), unsigned_ = as_.label();
        as_.prologue();
        as_.adjust_rsp(-32);
        as_.mov(RAX, RDI);
        as_.mov(R9, RSI);
        as_.mov(R8, RBP);
        as_.alu_imm8(5, "sub", R8, 1);
        as_.mov32_imm(RDX, '\n');
        as_.store_byte(R8, RDX);
        as_.mov32_imm(RCX, 10);
        as_.place(digit);
        as_.zero_edx();
        as_.group3(6, "div", RCX);
        as_.alu_imm8(0, "add", RDX, '0');
        as_.alu_imm8(5, "sub", R8, 1);
        as_.store_byte(R8, RDX);
        as_.test(RAX);
        as_.jcc(NE, digit);
        as_.test(R9);
        as_.jcc(E, unsigned_);
        as_.mov32_imm(RDX, '-');
        as_.alu_imm8(5, "sub", R8, 1);
        as_.store_byte(R8, RDX);
        as_.place(unsigned_);
        as_.mov(RDI, R8);
        as_.mov(RSI, RBP);
        as_.alu(0x29, "sub", RSI, R8);
        as_.call(write);
        as_.epilogue();
        as_.patch();
        end(decimal);

        begin(print_string, "rdi:rsi is the String");
        as_.call(write);
        as_.lea(RDI, newline);
        as_.mov32_imm(RSI, 1);
        as_.call(write);
        as_.ret();
        end(print_string);

        begin(print_int, "rdi is the Int");
        const auto positive = as_.label();
        as_.zero32(RSI);
        as_.test(RDI);
        as_.jcc(GE, positive);
        // The magnitude of the minimum is its own bit pattern, read unsigned.
        as_.group3(3, "neg", RDI);
        as_.mov32_imm(RSI, 1);
        as_.place(positive);
        as_.call(decimal);
        as_.ret();
        as_.patch();
        end(print_int);

        begin(print_uint, "rdi is the UInt");
        as_.zero32(RSI);
        as_.call(decimal);
        as_.ret();
        end(print_uint);

        begin(print_bool, "rdi is the Bool");
        const auto chosen = as_.label();
        as_.test(RDI);
        as_.lea(RDI, yes);
        as_.mov32_imm(RSI, 5);
        as_.jcc(NE, chosen);
        as_.lea(RDI, no);
        as_.mov32_imm(RSI, 6);
        as_.place(chosen);
        as_.call(write);
        as_.ret();
        as_.patch();
        end(print_bool);

        if (allocates) allocator(alloc, trap);

        as_.rodata();
    }

    std::string listing() const { return as_.render(); }

private:
    // Bumps a pointer through the current chunk, and maps a new chunk of the
    // request plus kChunk bytes when it does not fit; the rest of the old one
    // is left unused. mmap returns zeroed memory, and an error as -4095..-1.
    void allocator(u32 alloc, u32 trap) {
        const u32 heap = object_.reserve("_P$heap", 16);  // the next free byte and the chunk's end
        begin(alloc, "rdi: size; returns a zeroed, 16-byte aligned block in rax");
        const auto refill = as_.label(), failed = as_.label();
        as_.alu_imm8(0, "add", RDI, 15);
        as_.alu_imm8(4, "and", RDI, -16);
        as_.load(RAX, heap, 0);
        as_.load(RCX, heap, 8);
        as_.mov(RDX, RAX);
        as_.alu(0x01, "add", RDX, RDI);
        as_.alu(0x39, "cmp", RDX, RCX);
        as_.jcc(A, refill);
        as_.store(heap, 0, RDX);
        as_.ret();
        as_.place(refill);
        as_.push(RDI);
        as_.mov(RSI, RDI);
        as_.mov_imm(RCX, kChunk);
        as_.alu(0x01, "add", RSI, RCX);
        as_.zero32(RDI);
        as_.mov32_imm(RDX, 3);     // PROT_READ | PROT_WRITE
        as_.mov32_imm(R10, 0x22);  // MAP_PRIVATE | MAP_ANONYMOUS
        as_.mov_imm(R8, ~u64{0});  // no file
        as_.zero32(R9);
        as_.mov32_imm(RAX, kSysMmap);
        as_.syscall();
        as_.pop(RDI);
        as_.mov_imm(RCX, ~u64{0} << 12);
        as_.alu(0x39, "cmp", RAX, RCX);
        as_.jcc(A, failed);
        as_.mov(RCX, RAX);
        as_.alu(0x01, "add", RCX, RSI);
        as_.store(heap, 8, RCX);
        as_.mov(RDX, RAX);
        as_.alu(0x01, "add", RDX, RDI);
        as_.store(heap, 0, RDX);
        as_.ret();
        as_.place(failed);
        as_.mov32_imm(RDI, static_cast<u32>(Trap::OutOfMemory));
        as_.call(trap);
        as_.ud2();
        as_.patch();
        end(alloc);
    }

    u32 symbol(std::string name, bool defined) {
        object_.symbols.push_back({std::move(name), defined});
        return static_cast<u32>(object_.symbols.size() - 1);
    }
    void begin(u32 index, std::string_view comment) {
        while (object_.text.size() % 16) object_.text.push_back(0xCC);
        object_.symbols[index].offset = as_.here();
        as_.note(std::format("{}:  ; {}", object_.symbols[index].name, comment));
        as_.reset();
    }
    void end(u32 index) { object_.symbols[index].size = as_.here() - object_.symbols[index].offset; }
    void exit() {
        as_.mov32_imm(RAX, kSysExitGroup);
        as_.syscall();
        as_.ud2();
    }

    const mir::Program& program_;
    obj::Object& object_;
    Assembler as_;
};

}  // namespace

std::string symbol_name(const mir::Function& function) {
    return std::format("_P{}{}", function.name.size(), function.name);
}

std::string thunk_name(const mir::Function& function) {
    return std::format("_P$5thunk{}{}", function.name.size(), function.name);
}

obj::Object emit(const mir::Program& program, std::string* listing) {
    obj::Object object;
    Emitter emitter(program, object, listing);
    emitter.run();
    if (listing) *listing = emitter.listing();
    return object;
}

obj::Object runtime(const mir::Program& program, std::string* listing) {
    obj::Object object;
    Runtime runtime(program, object, listing);
    runtime.run();
    if (listing) *listing = runtime.listing();
    return object;
}

}  // namespace phs::x86_64
