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
#include <initializer_list>
#include <string>
#include <vector>

#include "frontend/session.hpp"
#include "syntax/ast_dump.hpp"

using namespace phs;
using Type = ScalarType;

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};

    void check(std::string_view source, const std::vector<DiagCode>& expected = {}, bool dump = true) {
        const auto id = session.sources().add("flow.phs", source);
        CHECK(id);
        const auto* syntax = session.parse(*id);
        if (session.diagnostics().has_errors(*id)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        result = session.check(*id);
        CHECK(result.scalar_types);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        const auto errors = session.diagnostics().error_count();
        CHECK(session.check(*id).scalar_types == result.scalar_types);
        CHECK(session.diagnostics().error_count() == errors);
    }

    const ast::VarItem& global(std::string_view name) const {
        for (const auto& b : result.resolution->bindings())
            if (b.kind == BindingKind::Variable && session.names().str(b.name.sym) == name) return *ast::cast<ast::VarItem>(b.item);
        CHECK(false);
        unreachable();
    }
    Type tail(const ast::Expr* block) const {
        const auto* stmts = ast::cast<ast::BlockExpr>(block);
        return result.scalar_types->expression(ast::cast<ast::ExprStmt>(stmts->stmts.back())->expr);
    }
};

void accepted_paths() {
    Frontend f;
    f.check(R"(
fun branches(x: Int): Int {
    if x > 0 { return 1 } else if x < 0 { return -1 } else { return 0 }
}
fun forever(): Int {
    loop {}
}
fun exits(x: Int): Int {
    loop {
        if x > 3 { return x }
    }
}
fun aborts(x: Int): Int {
    if x > 0 { return 1 }
    panic("negative")
}
fun deferred(x: Int): Int {
    let y: Int
    if x > 0 { y = 1 } else { y = 2 }
    return y
}
fun counted(limit: Int): Int {
    var total: Int
    total = 0
    var i = 0
    while i < limit {
        total += i
        i += 1
    }
    return total
}
fun matched(x: Int): Int {
    var y: Int
    match x {
        0 -> { y = 1 }
        _ -> { y = 2 }
    }
    return y
}
fun written(x: inout Int) { x = 3 }
fun unit(): Unit {}
fun expression(x: Int): Int = if x > 0 { 1 } else { 2 }
var counter = 0
fun bump() { counter += 1 }
fun main(): Int { return 0 }
)");
}

void missing_returns() {
    Frontend f;
    f.check(R"(
fun partial(a: Int): Int {
    if a > 0 { return 1 }
}
fun chain(a: Int): Int {
    if a > 1 { return 1 } else if a > 0 { return 2 }
}
fun loops(c: Bool): Int {
    while c { return 1 }
}
fun broken(): Int {
    loop { break }
}
fun empty(): Bool {}
)", {DiagCode::MissingReturn, DiagCode::MissingReturn, DiagCode::MissingReturn, DiagCode::MissingReturn,
     DiagCode::MissingReturn});
}

void initialization() {
    Frontend f;
    f.check(R"(
fun partial(c: Bool): Int {
    var y: Int
    if c { y = 1 }
    return y
}
fun looped(c: Bool): Int {
    var y: Int
    while c { y = 1 }
    return y
}
fun compound(): Int {
    var y: Int
    y += 1
    return 0
}
fun short(a: Bool): Int {
    var y: Int
    if a && (if a { y = 1
        true } else { false }) { return y }
    return 0
}
fun dead(): Int {
    var y: Int
    return 1
    return y
}
)", {DiagCode::UninitializedUse, DiagCode::UninitializedUse, DiagCode::UninitializedUse, DiagCode::UninitializedUse});
}

void immutability() {
    Frontend f;
    f.check(R"(
let limit = 1
fun parameter(x: Int) { x = 1 }
fun local() {
    let x = 1
    x = 2
}
fun twice() {
    let y: Int
    y = 1
    y = 2
}
fun repeated(c: Bool) {
    let y: Int
    while c { y = 1 }
}
fun global() { limit = 2 }
fun bound(values: [Int]) {
    for v in values { v = 1 }
}
)", {DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment});
}

void field_mutability() {
    Frontend f;
    f.check(R"(
struct Inner(var n: Int) {
    mut fun bump() { n += 1 }
}
struct Outer(var inner: Inner, let fixed: Inner) {
    var count: Int = 0
    static var total: Int = 0
    init(n: Int) {
        inner = Inner(n)
        fixed = Inner(n)
    }
    mut fun step() {
        inner.bump()
        self.inner.n = 2
        count += 1
        bump2()
        total += 1
        self = Outer(n: 1)
    }
    mut fun bump2() { inner.n -= 1 }
    fun read(): Int = inner.n + count
}
class Box(var value: Int) {
    let size: Int = 1
    mut fun grow() { value += 1 }
    fun peek(): Int = value
}
fun own_box(b: own Box) { b.value = 1; b.grow() }
fun take(o: inout Outer) { o.inner.n = 3; o.step(); o.inner.bump() }
fun main() {
    var o = Outer(n: 1)
    o.inner.n = 4
    o.count += 2
    o.step()
    o.inner.bump()
    take(o: o)
    let b = Box(1)
    b.value = 2
    b.grow()
    Box(3).grow()
    Box(3).value = 4
    Outer.total = 5
    var s = shared Box(1)
    s.grow()
}
)");
    Frontend g;
    g.check(R"(
struct Inner(var n: Int) {
    mut fun bump() { n += 1 }
}
struct Outer(var inner: Inner, let fixed: Inner) {
    static let limit: Int = 3
    fun look() {
        inner.n = 1
        inner.bump()
        bump2()
        self.inner.bump()
        self = Outer(Inner(1), Inner(2))
    }
    mut fun bump2() {
        fixed.n = 2
        fixed.bump()
    }
}
class Box(var value: Int) {
    let size: Int = 1
    mut fun grow() { value += 1 }
    fun peek(): Int {
        value = 2
        grow()
        return value
    }
}
fun borrowed(b: Box, o: Outer) {
    b.value = 1
    b.grow()
    o.inner.n = 1
    o.inner.bump()
}
fun make(): Inner = Inner(1)
fun main() {
    let o = Outer(Inner(1), Inner(2))
    o.inner.n = 4
    o.inner.bump()
    make().n = 1
    make().bump()
    let b = Box(1)
    b.size = 2
    Outer.limit = 4
    var p = o
    p.fixed.n = 1
    p.fixed.bump()
}
)", {DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment});
    Frontend h;
    h.check(R"(
open class Base(var x: Int, let y: Int) {
    mut fun bump() { x += 1 }
}
class Derived: Base(1, 2) {
    fun look() {
        super.x = 1
        super.bump()
        x = 1
        y = 2
    }
}
class Child(var n: Int) {
    mut fun grow() { n += 1 }
}
class Parent(let child: Child) {}
fun change(p: Parent) {
    p.child.n = 1
    p.child.grow()
}
struct Counter(var n: Int) {
    mut fun bump<T>() { n += 1 }
    fun read() { bump<Int>() }
}
)", {DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment});
    Frontend projections;
    projections.check(R"(
class Cell(var value: Int) {
    mut fun grow() { value += 1 }
}
struct Pair(var a: Int)
fun change(p: Cell?, cells: [Cell], pairs: [Pair]) {
    p!.value = 1
    p!.grow()
    cells[0].value = 1
    cells[0].grow()
    pairs[0].a = 1
    pairs[0] = Pair(2)
}
fun fixed() {
    let numbers = [1, 2]
    numbers[0] = 3
    let pair: Pair? = Pair(1)
    pair!.a = 2
}
fun allowed(cells: inout [Cell], p: own Cell?) {
    var numbers = [1, 2]
    numbers[0] = 3
    cells[0].value = 1
    p!.grow()
    let owned = [Cell(1)]
    owned[0].grow()
}
)", {DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment});
    Frontend roots;
    roots.check(R"(
struct Slot(var a: Int) {
    mut fun bump() { a += 1 }
}
interface Has {
    var value: Int
    fun reset() { value = 0 }
}
fun change<T: Has>(x: T) { x.value = 1 }
class Holder(let value: Int?, var spare: Int?) {}
fun unwrap(h: Holder) {
    h.value! = 2
    h.spare! = 3
}
fun literals() {
    [Slot(1)][0].a = 2
    [Slot(1)][0].bump()
}
)", {DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment});
    Frontend arguments;
    arguments.check(R"(
struct Slot(var a: Int, let b: Int)
fun bump(x: inout Int) { x += 1 }
fun swap(s: inout Slot) {}
fun pass(n: Int, slot: Slot) {
    bump(x: n)
    bump(x: slot.a)
    let s = Slot(1, 2)
    swap(s: s)
    bump(x: s.a)
    var t = Slot(1, 2)
    bump(x: t.b)
    bump(x: 3)
    swap(s: Slot(1, 2))
}
fun fine(x: inout Int) {
    var t = Slot(1, 2)
    bump(x: t.a)
    swap(s: t)
    bump(x: x)
    var n = 0
    bump(x: n)
}
)", {DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment});
}

void branch_results() {
    Frontend f;
    f.check(R"(
fun pick(d: Bool): Int = if d { 1 } else if !d { 2 } else { 3 }
fun maybe(d: Bool): Int? = if d { 5 }
let wide: Int64 = if true { 1 } else { 2 }
let some: Int? = if true { 1 } else { none }
)");
    CHECK(f.tail(ast::cast<ast::IfExpr>(f.global("wide").init)->then_block) == Type::Int64);
    CHECK(f.tail(ast::cast<ast::IfExpr>(f.global("wide").init)->else_branch) == Type::Int64);

    Frontend errors;
    errors.check(R"(
fun missing(d: Bool): Int = if d { 5 }
fun mismatch(a: Int): Int = if a > 0 { true } else { 2 }
let unit: Int = if true { 1 } else { }
let range: UInt8 = if true { 300 } else { 2 }
fun exits(): Int = unsafe { loop { break } }
fun elements(flag: Bool): [Int] = if flag { [if flag { true } else { 2 }] } else { [1] }
let narrow: [UInt8] = [if true { 300 } else { 1 }]
)", {DiagCode::IfWithoutElse, DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch, DiagCode::ScalarLiteralRange,
     DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch, DiagCode::ScalarLiteralRange});

    Frontend loops;
    loops.check(R"(
fun forever(): Int = unsafe { loop {} }
fun returns(): Int = unsafe { loop {
    return 1
    break
} }
fun inner(c: Bool): Int = unsafe {
    loop {
        while c { break }
        loop { break }
    }
}
)");
}

void gated_by_type_errors() {
    // Flow analysis runs only after scalar typing succeeds.
    Frontend f;
    f.check(R"(
fun f(): Int {
    let x: Int = true
}
)", {DiagCode::ScalarTypeMismatch});
}

void caught_errors_guards_and_conditions() {
    Frontend f;
    f.check(R"(
fun caught(): Int {
    let x = try? unsafe { throw 1 }
}
fun caughtBeforeInit(c: Bool): Int {
    var y: Int
    let z = try? unsafe {
        if c { throw 1 }
        y = 2
        1
    }
    return y
}
fun guarded(v: Int) {
    let x: Int
    match v {
        _ if unsafe { x = 1
            false } -> {}
        _ -> { x = 2 }
    }
}
fun condition(c: Bool) {
    let x: Int
    while unsafe { x = 1
        c } {}
}
fun defaulted(value: Int = unsafe { var x: Int
    x }) {}
let initializer = unsafe { var y: Int
    y }
fun maybe(): Never? = none
fun optionalNever(): Int {
    maybe()
}
fun cleanupThenWrite() {
    let x: Int
    defer { x = 1 }
    x = 2
}
fun twoCleanups() {
    let x: Int
    defer { x = 1 }
    defer { x = 2 }
}
fun rewritesParam(a: Int, b: Int = unsafe { a = 1
    0 }) {}
open class Base(x: Int) {}
class Derived: Base(unsafe { var x: Int
    x }) {}
class Field(a: Int) {
    let b: Int = unsafe { a = 1
        0 }
}
)", {DiagCode::MissingReturn, DiagCode::UninitializedUse, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::MissingReturn, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::UninitializedUse, DiagCode::UninitializedUse, DiagCode::ImmutableAssignment,
     DiagCode::UninitializedUse, DiagCode::ImmutableAssignment});

    Frontend valid;
    valid.check(R"(
fun arms(v: Int): Int {
    let x: Int
    match v {
        0 -> { x = 1 }
        _ -> { x = 2 }
    }
    return x
}
fun aborts(): Int {
    let x = try? panic("stop")
}
fun cleanupOnly() {
    let x: Int
    defer { x = 1 }
}
fun catchAll(v: Int): Int {
    var x: Int
    match v {
        _ -> { x = 1 }
        0 -> {}
    }
    return x
}
fun guardFirst(v: Int): Int {
    var x: Int
    match v {
        _ if unsafe { x = 1
            false } -> {}
        _ -> {}
    }
    return x
}
fun initialized(): Int {
    var y: Int
    let z = try? unsafe {
        y = 1
        throw 2
    }
    return y
}
)", {DiagCode::UnreachableArm});
}

void long_trees() {
    std::string chain = "fun f(a: Bool): Int {\n    var y: Int\n    let c = a";
    for (int i = 0; i < 50000; ++i) chain += " || a";
    chain += "\n    if c { y = 1 } else { y = 2 }\n    return y\n}\n";
    Frontend f;
    f.check(chain, {}, false);

    std::string nested = "y = 1\n";
    for (int i = 0; i < 40; ++i) nested = "while a {\n" + nested + "}\n";
    Frontend loops;
    loops.check("fun g(a: Bool): Int {\n    let y: Int\n" + nested + "    return y\n}\n",
                {DiagCode::ImmutableAssignment, DiagCode::UninitializedUse});
}
}  // namespace

void moves() {
    constexpr std::string_view prelude = "fun take(s: own String) {}\nfun look(s: String) {}\nstruct Box(s: String, n: Int)\n";
    const auto M = DiagCode::UseAfterMove;
    const auto B = DiagCode::MoveOutOfBorrow;
    const auto C = DiagCode::InvalidCapture;
    for (const auto& [source, expected] : std::vector<std::pair<std::string_view, std::vector<DiagCode>>>{
        // Copy values never move; reads borrow.
        {"fun f() { let n = 1\nlet m = n\nlet p = (n, true)\nlet q = p\nlet r = p\nlet g = look\nlet h = g\nlet i = g }", {}},
        {"fun f() { let a = \"a\"\nlook(a)\nlook(a)\nprint(a)\nlet n = Box(a, 1).n }", {}},
        // Moves through results of blocks, `match`, `??`, `!`, tuples and constructors.
        {"fun f(c: Int) { let a = \"a\"\nlet b = match c { 0 -> a, _ -> \"z\" }\nlook(a) }", {M}},
        {"fun f(o: String?) { let a: String? = \"a\"\nlet b = a ?? \"z\"\nlook(a!) }", {M}},
        {"fun f() { let a: String? = \"a\"\nlet b = a!\nlet c = a }", {M}},
        {"fun f() { let a = \"a\"\nlet b = Box(s: a, n: 1)\nlet c = Box(a, 2) }", {M}},
        {"fun f() { let a = \"a\"\nlet t = (1, a)\nlet u = (a, 2) }", {M}},
        {"fun f(): String { let a = \"a\"\nlook(a)\nreturn a }", {}},
        {"fun f(): String { let a = \"a\"\ntake(a)\nreturn a }", {M}},
        {"fun f(c: Bool, a: own String): String = if c { a } else { take(a)\n\"b\" }", {}},
        {"fun f(a: own String): Box = Box(a, if a == \"x\" { 1 } else { 2 })", {M}},
        {"fun f(): String { let a = \"a\"\nlet b = a\nreturn a }", {M}},
        // Arguments move only into `own` parameters.
        {"fun g(a: own String, b: String) {}\nfun f() { let a = \"a\"\ng(\"x\", a)\ng(a, a) }", {M}},
        {"fun f(s: own String) { let t = s\ntake(s) }", {M}},
        {"fun f(s: own String) { let g = { -> look(s) }\nlook(s) }", {M}},
        // Paths, loops and reassignment.
        {"fun f(c: Bool) { var a = \"a\"\nif c { take(a)\na = \"b\" }\nlook(a) }", {}},
        {"fun f(c: Bool) { let a = \"a\"\nif c { take(a)\nreturn }\nlook(a) }", {}},
        {"fun f(c: Bool) { let a = \"a\"\nwhile c { while c { take(a) } } }", {M}},
        {"fun f(c: Bool) { let a = \"a\"\nfor i in 0..<3 { let b = \"b\"\ntake(b) } }", {}},
        {"fun f(c: Bool) { let a = \"a\"\nloop { if c { take(a)\nbreak } } }", {}},
        {"fun f(c: Bool) { let a = \"a\"\nloop { take(a)\nif c { break } }\n}", {M}},
        {"fun f(c: Bool) { let a = \"a\"\nwhile c { let f = { -> look(a) } } }", {M}},
        // An inner loop that gives the value back on every exit leaves it usable.
        {"fun f(c: Bool) { var a = \"a\"\nwhile c { look(a)\nloop { a = \"new\"\ntake(a)\nif c { a = \"exit\"\nbreak } } } }", {}},
        {"fun f(c: Bool) { var a = \"a\"\nwhile c { look(a)\nwhile c { take(a)\nif c { a = \"exit\"\nbreak } } } }", {M, M}},
        {"fun f(c: Bool) { var a = \"a\"\nwhile c { look(a)\nwhile c { while c { take(a) }\na = \"b\" } } }", {M}},
        {"fun f(c: Bool) { var a = \"a\"\nwhile c { look(a)\nwhile c { a = \"b\"\nwhile c { take(a) } } } }", {M, M}},
        // Cleanups move at the exits of their scope: the end, `break`, `continue` and `return`.
        {"fun f(c: Bool) { let a = \"a\"\nif c { defer { take(a) } }\nlook(a) }", {M}},
        {"fun f() { let a = \"a\"\ndefer { look(a) }\ntake(a) }", {M}},
        {"fun f() { let a = \"a\"\ndefer { take(a) }\nlook(a) }", {}},
        {"fun f(c: Bool) { let a = \"a\"\nwhile c { defer { take(a) }\nif c { break } } }", {M}},
        {"fun f(c: Bool) { let a = \"a\"\nwhile c { if c { continue }\ndefer { look(a) } } }", {}},
        {"fun f(c: Bool) { let a = \"a\"\ndefer { look(a) }\nif c { take(a)\nreturn }\nlook(a) }", {M}},
        {"fun f() { let a = \"a\"\ndefer { let g = { -> look(a) } } }", {}},
        {"fun f() { var a = \"a\"\ndefer { defer { look(a) }\na = \"restored\" }\ntake(a) }", {}},
        {"fun f() { var a = \"a\"\ndefer { defer { look(a) }\nlook(a) }\ntake(a) }", {M}},
        {"fun f() { let a = \"a\"\nlet x = try? unsafe { defer { take(a) }\nthrow 1 }\nlook(a) }", {M}},
        {"fun g() throws {}\nfun f() { let a = \"a\"\nlet x = try? unsafe { defer { take(a) }\ng() }\nlook(a) }", {M}},
        {"fun f() { defer { let x = try? unsafe { throw 1 } } }", {}},
        {"fun f() { let a = \"a\"\ndefer { look(a) }\ndefer { panic(\"stop\") }\ntake(a) }", {}},
        {"fun f() { let a = \"a\"\ndefer { panic(\"stop\") }\ndefer { look(a) }\ntake(a) }", {M}},
        {"fun f() { var a = \"a\"\ntake(a)\ndefer { defer { look(a) } }\na = \"restored\" }", {}},
        {"fun f() { var a = \"a\"\ndefer { defer { look(a) } }\ntake(a) }", {M}},
        {"fun g() throws {}\nfun f() throws { var a = \"a\"\ndefer { look(a) }\ntake(a)\ntry g()\na = \"restored\" }", {M}},
        {"fun g() throws {}\nfun f() throws { var a = \"a\"\ndefer { look(a) }\ntry g()\ntake(a)\na = \"restored\" }", {}},
        {"fun f() { let g = { -> let a = \"a\"\ndefer { look(a) }\na } }", {M}},
        {"fun f() { let g = { -> let a = \"a\"\nlook(a)\na } }", {}},
        // Inferred expression-body results move too.
        {"fun f() = unsafe { let a = \"a\"\ndefer { look(a) }\na }", {M}},
        {"fun f() = unsafe { let a = \"a\"\ndefer { look(a) }\nlook(a) }", {}},
        // Global and field initializers move their value.
        {"let g = unsafe { let a = \"a\"\ndefer { look(a) }\na }", {M}},
        {"struct S(n: Int) { let s: String = unsafe { let a = \"a\"\ndefer { look(a) }\na } }", {M}},
        {"let g = unsafe { let a = \"a\"\ndefer { look(a) }\nlook(a)\n1 }", {}},
        // `try` and `try?` pass their operand's result on.
        {"fun f() { let a = \"a\"\nlet b = try? unsafe { a }\nlook(a) }", {M}},
        {"fun f() throws { let a = \"a\"\nlet b = try unsafe { a }\nlook(a) }", {M}},
        // A propagated error leaves from the call that throws it.
        {"fun g() throws {}\nfun f() throws { var a = \"a\"\ndefer { look(a) }\ntake(a)\ntry unsafe { g()\na = \"restored\" } }", {M}},
        {"fun g() throws {}\nfun f() throws { var a = \"a\"\ndefer { look(a) }\ntry unsafe { g()\ntake(a) }\na = \"x\" }", {}},
        {"fun g() throws {}\nfun f() { var a = \"a\"\nlet x = try? unsafe { defer { look(a) }\ng()\ntake(a)\n1 }\na = \"x\" }", {M}},
        {"fun g() throws {}\nfun f() { var a = \"a\"\nlet x = try? unsafe { g()\ntake(a)\na = \"b\"\n1 }\nlook(a) }", {}},
        // A default runs before the later defaults and the body.
        {"fun f(a: own String, b: Int = unsafe { take(a)\n0 }) { look(a) }", {M}},
        {"fun f(a: own String, b: Int = unsafe { take(a)\n0 }, c: Int = unsafe { look(a)\n0 }) {}", {M}},
        {"fun f(a: own String, b: Int = unsafe { look(a)\n0 }) { look(a) }", {}},
        {"struct S(a: own String, b: Int = unsafe { take(a)\n0 }) { init(a: own String, b: Int = unsafe { take(a)\n0 }) { look(a) } }", {M}},
        {"fun f(a: own String, b: own String = a) { look(a) }", {M}},
        {"fun f(a: own String, b: String = a) { look(a) }", {}},
        {"fun f(b: own String = unsafe { let a = \"a\"\ndefer { look(a) }\na }) {}", {M}},
        {"struct S(a: own String, b: String = a) { init { look(a) } }", {M}},
        // Construction runs field initializers in order, then the primary `init`.
        {"open class Base(s: own String)\nclass D(a: own String): Base(a) { init { look(a) } }", {M}},
        {"open class Base(s: own String, n: Int)\nclass D(a: own String): Base(n: 1, s: a) { let x: String = a }", {M}},
        {"open class Base(s: String)\nclass D(a: own String): Base(a) { init { look(a) } }", {}},
        {"class C(a: own String) { let x: String = a\nlet y: String = a }", {M}},
        {"class C(a: own String) { let x: String = a\ninit { look(a) } }", {M}},
        {"class C(a: own String) { let x: Int = unsafe { look(a)\n1 }\nlet y: String = a\ninit {} }", {}},
        // A clone borrows its receiver and moves a new value.
        {"fun f() { let a = \"a\"\ntake(a.clone())\nlet b = a.clone()\nlook(a)\ntake(b) }", {}},
        {"fun f() { let a = \"a\"\ntake(a)\nlet b = a.clone() }", {M}},
        {"fun f() { let p = Box(\"a\", 1)\nlet q = p.clone()\nlet r = p\nlook(q.s) }", {}},
        {"fun f() throws { let a = \"a\"\ndefer { look(a) }\nlet b = try unsafe { a.clone() } }", {}},
        // Only what the callable owns can move (E0505): not a borrowed or
        // `inout` parameter, a field, an element, `self` or a capture.
        {"fun f(s: String) { take(s) }", {B}},
        {"fun f(s: String): String = s", {B}},
        {"fun f(s: borrow String) { let t = s }", {B}},
        {"fun f(s: inout String) { let t = s }", {B}},
        {"fun f(c: Bool, s: String): String = if c { s } else { \"x\" }", {B}},
        {"fun f(o: String?) { let s = o! }", {B}},
        {"fun f(s: String) { take(s.clone())\nlook(s)\nlet n = 1\nlet m = n }", {}},
        {"fun f(b: Box) { let n = b.n\nlet s = b.s }", {B}},
        {"fun f() { let b = Box(\"a\", 1)\nlet s = b.s\nlet t = b.s.clone() }", {B}},
        {"fun f(t: (Int, String)) { let n = t.0\nlet s = t.1 }", {B}},
        {"fun f(a: [String]) { let s = a[0] }", {B}},
        {"struct S(name: String) { fun get(): String = name\nfun me(): S = self\nfun other(): String = self.name }", {B, B, B}},
        {"struct P(x: Int) { fun me(): P = self }\nfun f(p: P): P = p", {}},
        {"fun f<T>(x: T): T = x", {}},
        {"fun f(s: own String) { let g = { -> s }\nlet h = { -> s.clone() } }", {B, M}},
        {"fun f(s: own String) { let g = { -> look(s) } }", {}},
        {"fun f() { let g = { x: String -> take(x) } }", {B}},
        {"fun f(s: String) { let g = { -> s } }", {C}},
        {"fun f(s: String) { while true { take(s) } }", {B}},
        {"fun f(s: String) { defer { take(s) } }", {B}},
        // `super` is the borrowed receiver too, and no callable owns a static field.
        {"open class Base {}\nclass D: Base { fun leak(): Base = super }", {B}},
        {"class S { static let text: String = \"x\"\nstatic fun a(): String = S.text\nstatic fun b(): String = text }", {B, B}},
        {"class S { static let n: Int = 1\nstatic fun a(): Int = S.n }", {}},
        // A nested closure takes its own copy of what it captures.
        {"fun f(s: own String) { let g = { -> { -> s.clone() } } }", {B}},
        {"fun f(s: own String) { let g = { -> let c = s.clone()\n{ -> look(c) } } }", {}},
        {"fun f(n: Int) { let g = { -> { -> n } } }", {}},
        // Only reachable moves are reported.
        {"fun f(s: String) { return\nlet t = s }", {}},
        {"fun f(c: Bool, s: String): String = if c { return \"x\" } else { s }", {B}},
        {"fun f(s: String): String { return \"x\"\nreturn s }", {}},
        // A `let` condition takes the optional's value and binds it anew on
        // each run; its binding is immutable.
        {"fun f() { let a: String? = \"a\"\nif let s = a { take(s)\ntake(s) }\nlook(a!) }", {M, M}},
        {"fun f() { let a: String? = \"a\"\nif let s = a { take(s) } else { look(a!) } }", {M}},
        {"fun f(g: fun() -> String?) { while let s = g() { take(s) } }", {}},
        {"fun f(g: fun() -> String?) { let a: String? = \"a\"\nwhile let s = a { take(s) } }", {M}},
        {"fun f(o: Int?) { if let n = o { look(\"x\") }\nif let m = o { m = 1 } }", {DiagCode::ImmutableAssignment}},
        // A moved value is reported once until it moves again.
        {"fun f() { let a = \"a\"\ntake(a)\nlook(a)\nlook(a)\ntake(a)\nlook(a) }", {M, M}},
        // Unreachable code reports nothing.
        {"fun f() { let a = \"a\"\ntake(a)\nreturn\nlook(a) }", {}},
    }) {
        Frontend f;
        const std::string program = std::string(prelude) + std::string(source);
        f.check(program, expected, false);
    }
}

void exclusive_access() {
    constexpr std::string_view prelude =
        "fun swap(a: inout Int, b: inout Int) {}\nfun set(a: inout Int, b: Int) {}\n"
        "struct P(var x: Int, var y: Int) {\nmut fun bump(by: Int) { x += by }\nfun read(n: inout Int) { n = x }\n"
        "mut fun apart(n: inout Int) { bump(n) } }\n";
    const auto E = DiagCode::ExclusiveAccess;
    for (const auto& [source, expected] : std::vector<std::pair<std::string_view, std::vector<DiagCode>>>{
        // Separate places, and reads that are not places, are fine.
        {"fun f() { var a = 1\nvar b = 2\nswap(a, b)\nset(a, b)\nset(a, a + 1)\nvar p = P(x: 1, y: 2)\nswap(p.x, p.y)\np.bump(a)\np.read(a) }", {}},
        {"fun f() { var t = (1, 2)\nswap(t.0, t.1)\nvar p = P(x: 1, y: 2)\nvar q = p\nswap(p.x, q.x) }", {}},
        // An `inout` argument overlaps any other argument naming the same place.
        {"fun f() { var a = 1\nswap(a, a)\nset(a, a) }", {E, E}},
        {"fun f() { var p = P(x: 1, y: 2)\nswap(p.x, p.x) }", {E}},
        {"fun g(p: inout P, n: Int) {}\nfun f() { var p = P(x: 1, y: 2)\ng(p, p.x) }", {E}},
        {"fun g(t: inout (Int, Int), n: Int) {}\nfun f() { var t = (1, 2)\ng(t, t.0) }", {E}},
        {"fun f(a: inout [Int]) { swap(a[0], a[1]) }", {E}},
        // So does the receiver of a `mut fun`, and any receiver of a call with an `inout` argument.
        {"fun f() { var p = P(x: 1, y: 2)\np.bump(p.x) }", {E}},
        {"fun f() { var p = P(x: 1, y: 2)\np.read(p.y) }", {E}},
        // Inside a method, `self`, `super` and bare fields are the same receiver.
        {"struct R(var x: Int) { mut fun bump(by: Int) {}\nmut fun twice() { bump(x) } }", {E}},
        {"struct Q(var n: Int) { mut fun g(other: Q) {}\nmut fun h() { g(self)\nself.g(self) } }", {E, E}},
        // A static field is its own place, bare or through its type.
        {"class S { static var n: Int = 1\nstatic fun g() { swap(n, S.n) } }", {E}},
        {"class O { class I { static var n: Int = 0 } }\nstruct G<T>(v: T) { static var n: Int = 0 }\n"
         "fun f() { swap(O.I.n, O.I.n)\nswap(G<Int>.n, G<Int>.n)\nswap(O.I.n, G<Int>.n) }", {E, E}},
        // Omitted defaults are part of the call, with earlier parameters standing for their arguments.
        {"var g: Int = 0\nfun d(a: inout Int, b: inout Int = g) {}\nfun f() { var a = 1\nd(g)\nd(a) }", {E}},
        {"fun d(a: inout Int, b: Int = a) {}\nfun e(p: inout P, n: Int = p.x) {}\n"
         "fun f() { var a = 1\nd(a)\nd(a, 2)\nvar p = P(x: 1, y: 2)\ne(p)\ne(p, 1) }", {E, E}},
        // Construction and destruction have a receiver too.
        {"class K(var x: Int) { mut fun bump(by: Int) {}\ndeinit { bump(x) } }", {E}},
        {"struct L(var x: Int) { mut fun bump(by: Int) {}\ninit(y: Int) { x = y\nbump(x) } }", {E}},
        // A loop or cleanup reports each argument once.
        {"fun f(c: Bool) { var a = 1\nwhile c { swap(a, a) }\ndefer { swap(a, a) } }", {E, E}},
    }) {
        Frontend f;
        const std::string program = std::string(prelude) + std::string(source);
        f.check(program, expected, false);
    }
}

int main() {
    accepted_paths();
    missing_returns();
    initialization();
    immutability();
    field_mutability();
    branch_results();
    gated_by_type_errors();
    caught_errors_guards_and_conditions();
    long_trees();
    moves();
    exclusive_access();
}
