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

#include <string>
#include <string_view>

#include "codegen/mir.hpp"
#include "codegen/object.hpp"

// Naive x86_64 code generation from MIR. Every register lives in a stack slot
// below rbp and holds its value normalized to 64 bits (sign-extended for
// signed types, zero-extended otherwise). Each instruction loads its operands
// into rax and rcx, computes, and stores the result. Calls follow the System V
// AMD64 convention: integer arguments in rdi, rsi, rdx, rcx, r8 and r9, the
// rest on the stack, and the result in rax. A String takes two slots and is
// passed like a 16-byte struct: in two consecutive argument registers if both
// are free, otherwise both words on the stack, and returned in rax:rdx. String
// literals live in .rodata. A struct lies in its slots with the layout of
// abi.md A.2, narrow fields at their own size. Up to two words it travels like
// a String; a larger one is passed by address and copied in by the callee,
// and a larger result is written through a hidden pointer in rdi. An inout
// parameter, such as a `mut fun` receiver, is passed by address and written
// back before the callee returns. A function value travels like a two-word
// struct of code and environment; a call through it passes the environment
// first after ret* and calls the code through r11. A function used as a
// value has a thunk that drops the environment and calls it. A closure's code
// is its function, which takes the environment as its first parameter; its
// environment is allocated from the runtime when it captures anything.
//
// A failed check calls the runtime's trap entry with a Trap code in edi; the
// entry does not return.

namespace phs::x86_64 {

// A Panic trap writes the message that rsi and rdx hold, its address and length.
enum class Trap : u32 { Overflow = 1, DivideByZero = 2, WriteFailed = 3, OutOfMemory = 4, Panic = 5 };

inline constexpr std::string_view kTrapSymbol = "__phantasos_trap";
inline constexpr std::string_view kEntrySymbol = "_start";
inline constexpr u32 kPanicExitStatus = 101;

// Runtime entries behind the prelude print, one per overload (abi.md A.8).
// Each writes its argument and a newline to standard output: a String in
// rdi:rsi, an Int or UInt in rdi, or a Bool (0 or 1) in rdi.
inline constexpr std::string_view kPrintString = "_P$print_string";
inline constexpr std::string_view kPrintInt = "_P$print_int";
inline constexpr std::string_view kPrintUInt = "_P$print_uint";
inline constexpr std::string_view kPrintBool = "_P$print_bool";
// The runtime's allocator for closure environments: takes a size in rdi and
// returns a 16-byte aligned, zeroed block in rax. It never frees.
inline constexpr std::string_view kAlloc = "_P$alloc";

// "_P" followed by the length-prefixed MIR name (abi.md A.7, without a package).
[[nodiscard]] std::string symbol_name(const mir::Function& function);
// "_P$5thunk" followed by the length-prefixed MIR name: the code of a function's
// value, which takes the environment first and drops it.
[[nodiscard]] std::string thunk_name(const mir::Function& function);

// Symbols 0..n-1 are the program's functions in order; the trap entry follows,
// then the thunks of functions used as values, in function order, and, as they
// are first used, print entries, the allocator and string literals (local
// .rodata symbols).
// When listing is non-null it receives a readable assembly listing.
[[nodiscard]] obj::Object emit(const mir::Program& program, std::string* listing = nullptr);

// The stage0 Linux runtime for a program: _start calls the program's main and
// passes its result (0 for a Unit main) to exit_group, and the trap entry
// writes "panic: <reason>" to stderr and exits with kPanicExitStatus. The
// print entries format integers in decimal and Bool as true or false, and
// write with retries after partial writes and EINTR; any other failure panics
// with WriteFailed. When the program creates closures that capture, it also
// has the allocator: a bump allocator over chunks of at least 1 MiB from mmap,
// whose state lives in .bss, panicking with OutOfMemory when mmap fails. It
// uses raw system calls only; main stays an undefined symbol for the linker.
[[nodiscard]] obj::Object runtime(const mir::Program& program, std::string* listing = nullptr);

}  // namespace phs::x86_64
