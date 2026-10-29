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

#pragma once

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sema/types.hpp"

// MIR: the typed, block-structured IR between the checked AST and machine
// code. Registers are typed but not SSA: a local keeps one register that every
// assignment writes. Arithmetic is checked by default; a checked operation
// traps on overflow, and division and remainder also trap on a zero divisor.
// A String is two words, a pointer to its UTF-8 bytes and their length; stage0
// strings are literals, passed around whole. A struct is an aggregate value
// with a Layout (abi.md A.2), and so is a tuple, whose fields are its elements;
// aggregates are built whole, read one field at a time, and updated by copying
// them with one field replaced. A function value is an aggregate of two words:
// the code to call and an environment, passed to that code as a hidden first
// argument; a top-level function's value has a null environment. A closure
// is a function whose first parameter is that environment: a pointer to its
// captured values, laid out like a struct and never freed, or null when it
// captures nothing. An optional is an aggregate of a Bool tag, set when it
// holds a value, and that value; `none` is all zero bytes, so optionals of
// equal values have equal fields. Lowering leaves the lifting of a value into
// an optional implicit: a value stored where its optional is expected is
// wrapped by a final pass, so every operand has its destination's type. An
// array is an aggregate of three words: a pointer to its elements, never
// freed, and its count and capacity. Copying an array copies those words, so
// the copies share the elements; a store through one is seen by all, and
// the checker's moves (3.30) keep only one in use.

namespace phs::mir {

// Values from Aggregate up name Program::layouts[type - Aggregate].
enum class Type : u32 { Unit, Never, Bool, I8, I16, I32, I64, U8, U16, U32, U64, Str, Aggregate = 16 };

constexpr bool is_aggregate(Type type) { return type >= Type::Aggregate; }
constexpr Type aggregate(u32 layout) { return static_cast<Type>(static_cast<u32>(Type::Aggregate) + layout); }
constexpr u32 layout_index(Type type) { return static_cast<u32>(type) - static_cast<u32>(Type::Aggregate); }

bool is_signed(Type type);
u32 bit_width(Type type);  // 0 for Unit, Never and aggregates, 1 for Bool, 128 for Str

struct Reg {
    u32 id = std::numeric_limits<u32>::max();
    bool valid() const { return id != std::numeric_limits<u32>::max(); }
    friend bool operator==(Reg, Reg) = default;
};

enum class Op : u8 {
    Const, Copy, Str,
    Neg, Not, BitNot,
    Add, Sub, Mul, Div, Rem, WrapAdd, WrapSub, WrapMul, And, Or, Xor,
    Eq, Ne, Lt, Le, Gt, Ge,
    Widen, Truncate,
    Call,
    Print,  // writes a's value and a newline to standard output
    Make,   // an aggregate from args, its fields in declaration order
    Field,  // field imm of the aggregate a
    With,   // the aggregate a with field imm replaced by b
    FnRef,  // the function value of function callee, with a null environment
    CallIndirect,  // a call of the function value a with args
    Closure,  // the function value of closure function callee, its environment holding args
    Env,      // captured value imm of the environment that a points to
    None,     // the `none` of optional type dst: every byte zero
    Array,    // a new array of type dst holding args; with no args, every byte zero
    Load,     // element b of the array a; b is a U64 below its count
    Store,    // element b of the array a becomes args[0]; b is a U64 below its count
    Reserve,  // the array a with room for one more element: a full one's elements move to new
              // memory with twice the capacity, or 4 when it was 0
};

struct Inst {
    Op op;
    Reg dst;              // invalid for a call without a value and for print
    Reg a, b;             // operands; comparisons use the operand type
    u64 imm = 0;          // Const: the value, sign-extended for signed types; Str: index into Program::strings;
                          // Field, With, Env: the field's declaration index
    u32 callee = 0;       // Call, FnRef, Closure: function index
    // Call: arguments in parameter order, a method's receiver first;
    // CallIndirect: arguments in parameter order; Make: field values;
    // Closure: the captured values, in environment order; Array: the
    // elements; Store: the value stored
    std::vector<Reg> args;
};

// Panic writes its message to standard error and ends the program with exit
// status 101; `opt!` on `none` and an array index out of bounds panic with
// their source location.
enum class Term : u8 { Br, CondBr, Ret, Unreachable, Panic };

struct Block {
    std::vector<Inst> insts;
    Term term = Term::Unreachable;
    Reg value;                // CondBr: the Bool condition; Ret: the result, if any
    u32 target = 0, other = 0;  // Br: target; CondBr: target if true, other if false
    u32 message = 0;            // Panic: index into Program::strings
};

struct Function {
    std::string name;  // unique within the program
    Span span;
    std::vector<Type> regs;  // register types; parameters come first
    u32 params = 0;
    // Per parameter: an inout parameter's final value is written back to the
    // caller's argument register when the call returns. A `mut fun` receiver
    // is inout. Empty when no parameter is inout.
    std::vector<bool> inout;
    Type result = Type::Unit;
    std::vector<Block> blocks;  // blocks[0] is the entry; every block is reachable
    // A closure's parameter 0 is its environment pointer, a U64; environment
    // is the layout of what it captures, if anything.
    bool closure = false;
    std::optional<u32> environment;
};

inline bool is_inout(const Function& fun, u32 param) { return param < fun.inout.size() && fun.inout[param]; }

// Fields keep declaration order; offsets place them by decreasing alignment,
// stable among equal alignments, and the size is a multiple of the alignment.
// A function type's layout has two U64 fields, code and env, and records the
// signature that indirect calls through it follow. A closure's environment
// has a field for each captured value, named after it. An optional's layout
// has the fields some, a Bool, and value. An array's layout has the fields
// data, a U64 pointer to its elements, each element's size apart, and count
// and capacity, both I64.
struct Layout {
    enum Kind : u8 { Struct, Tuple, Function, Environment, Optional, Array };
    std::string name;  // a struct's or closure's name, or a tuple, function, optional or array type's spelling
    Kind kind = Struct;  // a tuple's fields are named 0, 1, ...
    std::vector<Type> fields;
    std::vector<std::string> names;
    std::vector<u32> offsets;
    u32 size = 0, align = 1;
    std::vector<Type> params;  // Function: parameter types
    Type result = Type::Unit;  // Function: result type
    Type element = Type::Unit;  // Array: element type
};

struct Program {
    std::vector<Function> functions;
    u32 main = 0;
    std::vector<std::string> strings;  // literal bytes, each distinct value once
    std::vector<Layout> layouts;       // each struct, tuple, function, optional and array type once, and each environment
};

std::string type_name(const Program& program, Type type);
u32 size_of(const Program& program, Type type);   // bytes: 0 for Unit and Never, 1 for Bool
u32 align_of(const Program& program, Type type);
u32 words(const Program& program, Type type);     // 8-byte words: 0 for Unit and Never
// Fills in a layout's offsets, size and alignment from its field types.
void place_fields(const Program& program, Layout& layout);

// Lowers a file that passed check_codegen_subset. Evaluation is strictly left
// to right: operands, call arguments in source order, then compound
// assignment's target before its value. Unreachable blocks are removed.
// Expression lowering is iterative; control nesting follows the syntax, which
// the parser bounds (E0112).
// Panic messages name source locations through sources.
[[nodiscard]] Program lower(const ast::File& file, const Resolution& resolution, ScalarTypes& types,
                            const Interner& names, const SourceManager& sources);

[[nodiscard]] std::string dump(const Program& program);

}  // namespace phs::mir
