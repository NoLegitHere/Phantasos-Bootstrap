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

#include "codegen/mir.hpp"

#include <algorithm>
#include <format>
#include <numeric>

namespace phs::mir {

std::string type_name(const Program& program, Type type) {
    if (is_aggregate(type)) return '@' + program.layouts[layout_index(type)].name;
    switch (type) {
        case Type::Unit: return "unit";
        case Type::Never: return "never";
        case Type::Bool: return "bool";
        case Type::I8: return "i8";
        case Type::I16: return "i16";
        case Type::I32: return "i32";
        case Type::I64: return "i64";
        case Type::U8: return "u8";
        case Type::U16: return "u16";
        case Type::U32: return "u32";
        case Type::U64: return "u64";
        case Type::Str: return "string";
        case Type::Aggregate: break;
    }
    unreachable();
}

bool is_signed(Type type) { return type >= Type::I8 && type <= Type::I64; }

u32 bit_width(Type type) {
    switch (type) {
        case Type::Unit: case Type::Never: return 0;
        case Type::Bool: return 1;
        case Type::I8: case Type::U8: return 8;
        case Type::I16: case Type::U16: return 16;
        case Type::I32: case Type::U32: return 32;
        case Type::I64: case Type::U64: return 64;
        case Type::Str: return 128;
        default: return 0;
    }
}

u32 size_of(const Program& program, Type type) {
    if (is_aggregate(type)) return program.layouts[layout_index(type)].size;
    if (type == Type::Bool) return 1;
    return bit_width(type) / 8;
}

u32 align_of(const Program& program, Type type) {
    if (is_aggregate(type)) return program.layouts[layout_index(type)].align;
    if (type == Type::Str) return 8;
    return std::max(size_of(program, type), 1u);
}

u32 words(const Program& program, Type type) { return (size_of(program, type) + 7) / 8; }

void place_fields(const Program& program, Layout& layout) {
    std::vector<usize> order(layout.fields.size());
    std::iota(order.begin(), order.end(), usize{0});
    std::ranges::stable_sort(order, [&](usize a, usize b) {
        return align_of(program, layout.fields[a]) > align_of(program, layout.fields[b]);
    });
    layout.offsets.assign(layout.fields.size(), 0);
    u32 at = 0;
    layout.align = 1;
    for (const usize field : order) {
        const u32 align = align_of(program, layout.fields[field]);
        at = (at + align - 1) / align * align;
        layout.offsets[field] = at;
        at += size_of(program, layout.fields[field]);
        layout.align = std::max(layout.align, align);
    }
    layout.size = (at + layout.align - 1) / layout.align * layout.align;
}

namespace {

std::string_view op_name(Op op) {
    switch (op) {
        case Op::Const: return "const";
        case Op::Copy: return "copy";
        case Op::Str: return "str";
        case Op::Neg: return "neg";
        case Op::Not: return "not";
        case Op::BitNot: return "bitnot";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Rem: return "rem";
        case Op::WrapAdd: return "wadd";
        case Op::WrapSub: return "wsub";
        case Op::WrapMul: return "wmul";
        case Op::And: return "and";
        case Op::Or: return "or";
        case Op::Xor: return "xor";
        case Op::Eq: return "eq";
        case Op::Ne: return "ne";
        case Op::Lt: return "lt";
        case Op::Le: return "le";
        case Op::Gt: return "gt";
        case Op::Ge: return "ge";
        case Op::Widen: return "widen";
        case Op::Truncate: return "truncate";
        case Op::Call: return "call";
        case Op::Print: return "print";
        case Op::Make: return "make";
        case Op::Field: return "field";
        case Op::With: return "with";
        case Op::FnRef: return "fnref";
        case Op::CallIndirect: return "call";
        case Op::Closure: return "closure";
        case Op::Env: return "env";
        case Op::None: return "none";
        case Op::Array: return "array";
        case Op::Load: return "load";
        case Op::Store: return "store";
        case Op::Reserve: return "reserve";
        case Op::Allocate: return "allocate";
    }
    unreachable();
}

// Constants print as the signed or unsigned value of their type.
std::string constant(Type type, u64 bits) {
    if (type == Type::Bool) return bits ? "true" : "false";
    if (is_signed(type)) return std::format("{}", static_cast<i64>(bits));
    return std::format("{}", bits);
}

// String literals print quoted, with escapes for quotes, backslashes and
// control characters; other UTF-8 bytes print as they are.
std::string literal(std::string_view bytes) {
    std::string out = "\"";
    for (const char c : bytes) {
        if (c == '"' || c == '\\') out += std::format("\\{}", c);
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (static_cast<u8>(c) < 0x20 || c == 0x7F) out += std::format("\\u{{{:X}}}", static_cast<u8>(c));
        else out += c;
    }
    return out + '"';
}

}  // namespace

std::string dump(const Program& program) {
    std::string out;
    for (const auto& layout : program.layouts) {
        constexpr std::string_view kinds[] = {"struct", "tuple", "function", "environment", "optional", "array"};
        out += std::format("{} @{} {{", kinds[layout.kind], layout.name);
        for (usize i = 0; i < layout.fields.size(); ++i)
            out += std::format("{} {}: {} +{}", i ? "," : "", layout.names[i], type_name(program, layout.fields[i]),
                               layout.offsets[i]);
        out += std::format(" }} size {} align {}", layout.size, layout.align);
        if (layout.kind == Layout::Function) {
            out += " signature (";
            for (usize i = 0; i < layout.params.size(); ++i)
                out += (i ? ", " : "") + type_name(program, layout.params[i]);
            out += ") -> " + type_name(program, layout.result);
        }
        if (layout.kind == Layout::Array) out += " element " + type_name(program, layout.element);
        out += "\n\n";
    }
    const auto reg = [](Reg r) { return std::format("%{}", r.id); };
    const auto name_of = [&](Type type) { return type_name(program, type); };
    for (usize f = 0; f < program.functions.size(); ++f) {
        const auto& fun = program.functions[f];
        if (f) out += '\n';
        out += std::format("fun @{}(", fun.name);
        for (u32 p = 0; p < fun.params; ++p)
            out += std::format("{}{}%{}: {}", p ? ", " : "", is_inout(fun, p) ? "inout " : fun.closure && !p ? "env " : "",
                               p, name_of(fun.regs[p]));
        out += std::format(") -> {}{} {{\n", name_of(fun.result), f == program.main ? " entry" : "");
        for (usize b = 0; b < fun.blocks.size(); ++b) {
            const auto& block = fun.blocks[b];
            out += std::format("bb{}:\n", b);
            for (const auto& inst : block.insts) {
                out += "    ";
                if (inst.dst.valid()) out += std::format("{}: {} = ", reg(inst.dst), name_of(fun.regs[inst.dst.id]));
                out += op_name(inst.op);
                switch (inst.op) {
                    case Op::Const: out += ' ' + constant(fun.regs[inst.dst.id], inst.imm); break;
                    case Op::Str: out += ' ' + literal(program.strings[inst.imm]); break;
                    case Op::Call:
                        out += std::format(" @{}(", program.functions[inst.callee].name);
                        for (usize a = 0; a < inst.args.size(); ++a) out += (a ? ", " : "") + reg(inst.args[a]);
                        out += ')';
                        break;
                    case Op::FnRef: out += std::format(" @{}", program.functions[inst.callee].name); break;
                    case Op::Closure:
                        out += std::format(" @{}(", program.functions[inst.callee].name);
                        for (usize a = 0; a < inst.args.size(); ++a) out += (a ? ", " : "") + reg(inst.args[a]);
                        out += ')';
                        break;
                    case Op::Env:
                        out += std::format(" {}, {}", reg(inst.a), program.layouts[*fun.environment].names[inst.imm]);
                        break;
                    case Op::CallIndirect:
                        out += ' ' + reg(inst.a) + '(';
                        for (usize a = 0; a < inst.args.size(); ++a) out += (a ? ", " : "") + reg(inst.args[a]);
                        out += ')';
                        break;
                    case Op::None: break;
                    case Op::Make: case Op::Array:
                        for (usize a = 0; a < inst.args.size(); ++a) out += (a ? ", " : " ") + reg(inst.args[a]);
                        break;
                    case Op::Field: {
                        const auto& layout = program.layouts[layout_index(fun.regs[inst.a.id])];
                        out += std::format(" {}, {}", reg(inst.a), layout.names[inst.imm]);
                        break;
                    }
                    case Op::With: {
                        const auto& layout = program.layouts[layout_index(fun.regs[inst.a.id])];
                        out += std::format(" {}, {}, {}", reg(inst.a), layout.names[inst.imm], reg(inst.b));
                        break;
                    }
                    case Op::Store:
                        out += std::format(" {}, {}, {}", reg(inst.a), reg(inst.b), reg(inst.args.front()));
                        break;
                    default:
                        out += ' ' + reg(inst.a);
                        if (inst.b.valid()) out += ", " + reg(inst.b);
                        break;
                }
                out += '\n';
            }
            switch (block.term) {
                case Term::Br: out += std::format("    br bb{}\n", block.target); break;
                case Term::CondBr:
                    out += std::format("    condbr {}, bb{}, bb{}\n", reg(block.value), block.target, block.other);
                    break;
                case Term::Ret: out += block.value.valid() ? std::format("    ret {}\n", reg(block.value)) : "    ret\n"; break;
                case Term::Unreachable: out += "    unreachable\n"; break;
                case Term::Panic: out += std::format("    panic {}\n", literal(program.strings[block.message])); break;
            }
        }
        out += "}\n";
    }
    return out;
}

}  // namespace phs::mir
