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

#include "test_support.hpp"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <string>
#include <vector>

#include "frontend/session.hpp"
#include "syntax/ast_dump.hpp"

using namespace phs;

namespace {
struct Frontend {
    FrontendSession session;

    bool gate(std::string_view source, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        const auto id = session.sources().add("gate.phs", source);
        CHECK(id);
        const auto* syntax = session.parse(*id);
        if (session.diagnostics().has_errors(*id)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        const bool ok = session.check_codegen(*id);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        CHECK(ok == wanted.empty());
        // Results are cached per file, and diagnostics are not repeated.
        const auto errors = session.diagnostics().error_count();
        CHECK(session.check_codegen(*id) == ok);
        CHECK(session.diagnostics().error_count() == errors);
        return ok;
    }
};

void accepted_subset() {
    Frontend f;
    f.gate(R"(
fun fib(n: Int): Int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
fun narrow(x: Int64): Int32 {
    var y: Int32 = 0
    while y < 3 && !(x == 0) {
        y += 1
        if y == 2 { continue } else { y = y &+ 0 }
    }
    loop { break }
    return y
}
fun pick(c: Bool, a: UInt16, b: UInt16): UInt16 = if c { a & b } else { a | ~b }
fun wide(x: Int32): Int64 = Int64(x) * 2
fun stop(): Never {
    loop {}
}
fun main(): Int {
    let n: Int = if fib(3) > 1 { 1 } else if true { 2 } else { stop() }
    return n + Int(wide(narrow(4)))
}
)");
    Frontend parens;
    parens.gate("fun pick(c: Bool): Int = ((if c { 1 } else { 2 }))\nfun main(): Int {\n    (if true { pick(false) } else { 0 })\n    return (pick(true))\n}\n");
    Frontend unit;
    unit.gate("fun main() {}\n");
    Frontend never;
    never.gate("fun main(): Never {\n    loop {}\n}\n");
}

void rejected_items() {
    Frontend f;
    f.gate(R"(
struct S {}
enum E { A, B }
let g = 1
type T = Int
fun real(x: Float32): Float32 = x
fun opt(x: Unit?) {}
fun bad() throws {}
fun conv(x: inout Int) {}
fun dflt(x: Int = 0) {}
async fun later() {}
fun main() {}
)", {DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen});
}

void rejected_bodies() {
    Frontend f;
    f.gate(R"(
fun other(): Int = 1
fun main(): Int {
    let c = 'x'
    let r = 1 << 2
    defer {}
    let m: Int = match 1 { _ -> 2 }
    let u: Int = unsafe { 3 }
    let h: Bool = other == other
    let v = if true { 1 } else { 2 }
    return v + v
}
)", {DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen});
}

void stored_values() {
    // Parameters and locals need a value; Unit and Never results are fine.
    Frontend f;
    f.gate("fun noop() {}\nfun stop(): Never {\n    loop {}\n}\nfun takes(u: ()) {}\nfun main() {\n    let u = noop()\n    let n: Int = 1\n    if n == 2 {\n        let s = stop()\n    }\n}\n",
           {DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen});
}

void entry_points() {
    Frontend none;
    none.gate("fun helper() {}\n", {DiagCode::InvalidEntryPoint});
    Frontend params;
    params.gate("fun main(x: Int) {}\n", {DiagCode::InvalidEntryPoint});
    Frontend result;
    result.gate("fun main(): Bool = true\n", {DiagCode::InvalidEntryPoint});
    Frontend overloaded;
    overloaded.gate("fun main() {}\nfun main(x: Int) {}\n", {DiagCode::InvalidEntryPoint});
    Frontend generic;
    generic.gate("fun main<T>(): Int = 0\n", {DiagCode::InvalidEntryPoint});
    // A rejected result type is reported once, as unsupported.
    Frontend real;
    real.gate("fun main(): Float64 = 1.0\n", {DiagCode::UnsupportedCodegen});
}

void gated_by_check_errors() {
    FrontendSession session;
    const auto id = session.sources().add("gate.phs", "fun main(): Int {\n    return true\n}\nlet s = \"x\"\n");
    CHECK(id);
    CHECK(!session.check_codegen(*id));
    CHECK(session.diagnostics().error_count() == 1);
    CHECK(session.diagnostics().all().front().code == DiagCode::ScalarTypeMismatch);
}

void long_trees() {
    std::string chain = "fun main(): Int {\n    let c: Bool = true";
    for (int i = 0; i < 50000; ++i) chain += " || false";
    chain += "\n    if c { return 1 }\n    return 0\n}\n";
    Frontend f;
    f.gate(chain, {}, false);
    std::string nested = "fun main() {\n    var i = 0\n";
    for (int i = 0; i < 40; ++i) nested += "while i < 1 {\n";
    nested += "i += 1\n";
    for (int i = 0; i < 40; ++i) nested += "}\n";
    nested += "}\n";
    Frontend g;
    g.gate(nested);
}

void generic_functions() {
    // A generic function is accepted when every call supplies supported type
    // arguments, and gets one instance per distinct argument list.
    Frontend f;
    f.gate(R"(
fun identity<T>(x: T): T = x
fun pair<A, B>(a: A, b: B): (A, B) = (a, b)
fun count<T>(x: T, n: Int): Int {
    if n == 0 { return 0 }
    return 1 + count(x, n - 1)
}
fun unused<T>(x: T): T = x
fun main(): Int = pair(identity(1), identity<Bool>(true)).0 + count("s", 2)
)");
    // Polymorphic recursion nests instances without end.
    Frontend deep;
    deep.gate("fun grow<T>(x: T, n: Int): Int {\n    if n == 0 { return 0 }\n    return grow((x, x), n - 1)\n}\n"
              "fun main(): Int = grow(1, 3)\n",
              {DiagCode::InstanceLimit});
    // Four levels of twelve calls each need more instances than allowed,
    // although none nests deeply.
    const char* types[] = {"Int",    "Int8",   "Int16",  "Int32", "Int64", "UInt",
                           "UInt8", "UInt16", "UInt32", "UInt64", "Bool", "String"};
    std::string wide;
    for (int level = 0; level < 4; ++level) {
        wide += "fun f" + std::to_string(level) + "<T>() {\n";
        for (const char* type : types)
            wide += "    f" + std::to_string(level + 1) + "<(T, " + type + ")>()\n";
        wide += "}\n";
    }
    wide += "fun f4<T>() {}\nfun main() {\n    f0<Int>()\n}\n";
    Frontend broad;
    broad.gate(wide, {DiagCode::InstanceLimit});
    // Generic functions as values need instances too, also through values.
    Frontend values;
    values.gate("fun identity<T>(x: T): T = x\nfun apply<T>(f: fun(T) -> T, x: T): T = f(x)\n"
                "fun main(): Int = apply(identity, 1) + identity<Int>(1)\n");
    Frontend grown;
    grown.gate("fun grow<T>(x: T, n: Int): Int {\n    if n == 0 { return 0 }\n"
               "    let next: fun((T, T), Int) -> Int = grow\n    return next((x, x), n - 1)\n}\n"
               "fun main(): Int = grow(1, 3)\n",
               {DiagCode::InstanceLimit});
    // A generic function's body is still checked once, against the subset.
    Frontend body;
    body.gate("fun opt<T>(x: T): Int {\n    let y: Float64 = 1.5\n    return 0\n}\nfun main() {}\n",
              {DiagCode::UnsupportedCodegen});
}

void generic_structs() {
    // Generic structs and their methods, generic methods included, are
    // accepted with supported type arguments.
    Frontend f;
    f.gate(R"(
struct Box<T>(var value: T) {
    fun get(): T = value
    mut fun set(x: T) { value = x }
    fun with<U>(u: U): Box<(T, U)> = Box(value: (get(), u))
}
struct Plain(n: Int) {
    fun tag<T>(x: T): (Int, T) = (n, x)
}
fun unbox<T>(b: Box<T>): T = b.get()
fun main(): Int {
    var b = Box(value: 1)
    b.set(2)
    return unbox(b.with(true)).0 + Plain(n: 1).tag("s").0
}
)");
    // Instance types that share their arguments are checked once each.
    std::string shared = "struct Tag<A, B>(value: Int)\nfun twice<T>(x: T): Tag<T, T> = Tag<T, T>(value: 0)\nfun main() {\n    let a0 = 1\n";
    for (int i = 1; i <= 40; ++i) shared += std::format("    let a{} = twice(a{})\n", i, i - 1);
    shared += "}\n";
    Frontend tags;
    tags.gate(shared);
    // A method instance that calls itself on a bigger receiver.
    Frontend deep;
    deep.gate("struct W<T>(x: T) {\n    fun grow(n: Int): Int {\n        if n == 0 { return 0 }\n"
              "        return W(x: (x, x)).grow(n - 1)\n    }\n}\nfun main(): Int = W(x: 1).grow(3)\n",
              {DiagCode::InstanceLimit});
    // A struct that holds itself through another's type argument has no
    // finite layout, and Unit is not a type argument of a value.
    Frontend looped;
    looped.gate("struct B<T>(x: T)\nstruct R<T>(b: B<R<T>>)\nfun main() {}\n", {DiagCode::UnsupportedCodegen});
    Frontend unit;
    unit.gate("struct B<T>(x: T)\nfun main() {\n    let b = B(x: ())\n}\n", {DiagCode::UnsupportedCodegen});
}
void interface_bounds() {
    // Interfaces with method requirements are accepted as bounds, and structs
    // that conform to them, generic ones and defaults included.
    Frontend f;
    f.gate(R"(
interface Shape {
    fun area(): Int
    fun twice(): Int = area() * 2
    fun larger(other: Self): Bool = area() > other.area()
}
interface Counter {
    mut fun bump()
}
struct Square(side: Int): Shape {
    fun area(): Int = side * side
}
struct Pair<T: Shape>(a: T, b: T): Shape {
    fun area(): Int = a.area() + b.area()
}
struct Tally(var n: Int): Counter, Shape {
    mut fun bump() { n += 1 }
    fun area(): Int = n
}
fun measure<T: Shape>(x: T): Int = x.twice()
fun both<T: Counter & Shape>(x: T): Int {
    var y = x
    y.bump()
    return y.area()
}
fun main(): Int {
    let p = Pair(a: Square(side: 1), b: Square(side: 2))
    if p.larger(p) { return 0 }
    return measure(p) + both(Tally(n: 1))
}
)");
    // A witness that calls back through the bound on a bigger receiver
    // nests instances without end.
    Frontend deep;
    deep.gate("interface Grow {\n    fun grow(n: Int): Int\n}\n"
              "struct W<T>(x: T): Grow {\n    fun grow(n: Int): Int {\n        if n == 0 { return 0 }\n"
              "        return deeper(W(x: (x, x)), n - 1)\n    }\n}\n"
              "fun deeper<T: Grow>(g: T, n: Int): Int = g.grow(n)\nfun main(): Int = W(x: 1).grow(3)\n",
              {DiagCode::InstanceLimit});
    // Values of an interface type would need dynamic dispatch.
    Frontend value;
    value.gate("interface Shape {\n    fun area(): Int\n}\nstruct Square(side: Int): Shape {\n"
               "    fun area(): Int = side * side\n}\nfun main() {\n    let s: Shape = Square(side: 1)\n}\n",
               {DiagCode::UnsupportedCodegen});
}
}  // namespace

int main() {
    accepted_subset();
    rejected_items();
    rejected_bodies();
    stored_values();
    entry_points();
    gated_by_check_errors();
    long_trees();
    generic_functions();
    generic_structs();
    interface_bounds();
    std::println("gate tests passed");
}
