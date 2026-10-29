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

#include <format>
#include <limits>
#include <string>
#include <vector>

#include "codegen/mir.hpp"
#include "mir_interpreter.hpp"
#include "frontend/session.hpp"

using namespace phs;
using mir::Op;
using mir::Term;
using mir::Type;

using test::Interpreter;
using test::Outcome;

namespace {

struct Frontend {
    FrontendSession session;
    const mir::Program* program = nullptr;

    const mir::Program& lower(std::string_view source) {
        const auto id = session.sources().add("mir.phs", source);
        CHECK(id);
        program = session.lower(*id);
        if (!program) session.diagnostics().render(stderr);
        CHECK(program);
        CHECK(session.lower(*id) == program);
        // Every block is reachable and every jump targets a block.
        for (const auto& fun : program->functions)
            for (const auto& b : fun.blocks) {
                if (b.term == Term::Br || b.term == Term::CondBr) CHECK(b.target < fun.blocks.size());
                if (b.term == Term::CondBr) CHECK(b.other < fun.blocks.size());
            }
        return *program;
    }
    Outcome run(std::string_view source) { return Interpreter(lower(source)).run(); }
    std::string dump(std::string_view source) { return mir::dump(lower(source)); }
};

i64 result(std::string_view source) {
    Frontend f;
    const auto outcome = f.run(source);
    CHECK(!outcome.trapped);
    return static_cast<i64>(outcome.value);
}

bool traps(std::string_view source) {
    Frontend f;
    return f.run(source).trapped;
}

void target_program() {
    CHECK(result(R"(
fun fib(n: Int): Int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
fun sumTo(limit: Int): Int {
    var total = 0
    var i = 1
    while i <= limit {
        if i % 3 == 0 || i % 5 == 0 { total += i }
        i += 1
    }
    return total
}
fun main(): Int {
    return fib(10) - sumTo(9) + 10
}
)") == 42);
}

void control_flow() {
    CHECK(result(R"(
fun classify(x: Int): Int {
    if x < 0 { return -1 } else if x == 0 { return 0 } else if x < 10 { return 1 }
    return 2
}
fun loops(n: Int): Int {
    var total = 0
    var i = 0
    loop {
        i += 1
        if i > n { break }
        if i % 2 == 0 { continue }
        var j = 0
        while true {
            j += 1
            if j >= i { break }
            total += 1
        }
    }
    return total
}
fun pick(c: Bool): Int = if c { 7 } else { 9 }
fun main(): Int {
    let x: Int
    if pick(true) == 7 { x = 100 } else { x = 200 }
    let y: Int = if x > 150 { 1 } else if x > 50 { 2 } else { 3 }
    let z = x + y
    return z + classify(-5) + classify(0) * 10 + classify(5) + classify(50) + loops(5) + pick(false)
}
)") == 102 - 1 + 0 + 1 + 2 + 6 + 9);
}

void arithmetic() {
    CHECK(result(R"(
fun main(): Int {
    let a: Int8 = 127
    let b: Int8 = a &+ 1
    let c: UInt8 = 200
    let d: UInt8 = c &* 2
    let e: UInt16 = ~UInt16(c)
    let m: Int64 = -9223372036854775808
    let t: Int8 = Int8.truncating(300)
    var bits: UInt32 = 12
    bits &= 10
    bits |= 1
    bits ^= 3
    let n: Int = -7 / 2 + -7 % 2
    let flag: Int = if m == -9223372036854775807 - 1 { 1000 } else { 0 }
    return Int(b) + Int(d) + Int(e) + flag + Int(t) + Int(bits) + n
}
)") == -128 + 144 + 65335 + 1000 + 44 + 10 - 3 - 1);
    CHECK(traps("fun main(): Int {\n    var x: Int8 = 127\n    x += 1\n    return 0\n}\n"));
    CHECK(traps("fun main(): Int {\n    let x: UInt = 0\n    let y = x - 1\n    return 0\n}\n"));
    CHECK(traps("fun zero(): Int = 0\nfun main(): Int = 5 / zero()\n"));
    CHECK(traps("fun zero(): Int = 0\nfun main(): Int = 5 % zero()\n"));
    CHECK(traps("fun minus(): Int = -1\nfun main(): Int = (-9223372036854775807 - 1) / minus()\n"));
    CHECK(traps("fun main(): Int {\n    let x: Int32 = -2147483647 - 1\n    return Int(-x)\n}\n"));
    CHECK(traps("fun main(): Int {\n    let x: Int16 = 300\n    return Int(x * x)\n}\n"));
}

void short_circuit() {
    // The right operand would trap if it ran.
    CHECK(result(R"(
fun boom(): Bool = 1 / 0 == 1
fun main(): Int {
    let a = false && boom()
    let b = true || boom()
    if a || !b { return 1 }
    return 0
}
)") == 0);
    CHECK(traps("fun boom(): Bool = 1 / 0 == 1\nfun main(): Int {\n    let a = true && boom()\n    return 0\n}\n"));
}

void calls_and_order() {
    Frontend f;
    const auto text = f.dump(R"(
fun sub(a: Int, b: Int): Int = a - b
fun sub(a: Int8): Int8 = a
fun stop(): Never {
    loop {}
}
fun noop() {}
fun main(): Int {
    noop()
    let x: Int = if true { sub(b: 1, a: 5) } else { stop() }
    return x
}
)");
    CHECK(text.contains("fun @sub.1(%0: i8) -> i8"));
    CHECK(text.contains("fun @stop() -> never"));
    CHECK(text.contains("fun @main() -> i64 entry"));
    // Labeled arguments run in source order and pass in parameter order.
    CHECK(text.contains("    %2: i64 = const 1\n    %3: i64 = const 5\n    %4: i64 = call @sub(%3, %2)\n"));
    // A call to a Never function ends its block.
    CHECK(text.contains("    call @stop()\n    unreachable\n"));
    CHECK(text.contains("    call @noop()\n"));
    Frontend g;
    CHECK(g.run("fun sub(a: Int, b: Int): Int = a - b\nfun main(): Int = sub(b: 1, a: 5)\n").value == 4);
    Frontend unit;
    CHECK(unit.run("fun main() {\n    let same = () == ()\n    if !same { return }\n}\n").value == 0);
}

void writes_during_evaluation() {
    // An operand read before a later operand assigns the same local keeps its value.
    CHECK(result(R"(
fun sub(a: Int, b: Int): Int = a - b
fun main(): Int {
    var x = 1
    let y = sub(x, if true {
        x = 2
        3
    } else { 3 })
    let z = sub(x * 10, if true {
        x = 5
        1
    } else { 1 })
    return y * 100 + z + x
}
)") == -200 + 19 + 5);
    // Compound assignment reads its target before its value.
    CHECK(result(R"(
fun main(): Int {
    var x = 1
    x += if true {
        x = 10
        2
    } else { 2 }
    return x
}
)") == 3);
    // A diverging operand ends the block without a value.
    Frontend f;
    const auto text = f.dump("fun stop(): Never {\n    loop {}\n}\nfun main(): Int = -stop()\nfun flag(): Bool = !stop()\n");
    CHECK(text.contains("    call @stop()\n    unreachable\n"));
    CHECK(!text.contains("neg") && !text.contains("not"));
}

void structs() {
    Frontend f;
    const auto& program = f.lower(R"(
struct Flags(on: Bool, level: Int8, mask: UInt32)
struct Point(x: Int, y: Int)
struct Label(text: String, at: Point, tag: Flags)
struct Bits(a: Bool, b: Bool, c: Bool)
fun tagged(l: Label): Int = Int(l.tag.level) + l.at.y
fun main(): Int {
    let l = Label(tag: Flags(mask: 1, level: -2, on: true), text: "x", at: Point(y: 5, x: 3))
    let bits = Bits(a: false, b: true, c: false)
    if bits.b { return tagged(l) }
    return 0
}
)");
    // Layouts follow declaration order, nested structs first, with fields by
    // decreasing alignment (abi.md A.2).
    CHECK(program.layouts.size() == 4);
    const auto& flags = program.layouts[0];
    CHECK(flags.name == "Flags" && flags.offsets == std::vector<u32>{4, 5, 0} && flags.size == 8 && flags.align == 4);
    const auto& label = program.layouts[2];
    CHECK(label.name == "Label" && label.offsets == std::vector<u32>{0, 16, 32} && label.size == 40 && label.align == 8);
    CHECK(mir::size_of(program, label.fields[1]) == 16 && mir::words(program, mir::aggregate(2)) == 5);
    const auto& bits = program.layouts[3];
    CHECK(bits.offsets == std::vector<u32>{0, 1, 2} && bits.size == 3 && bits.align == 1);
    CHECK(Interpreter(program).run().value == 3);
    const auto text = mir::dump(program);
    CHECK(text.starts_with("struct @Flags { on: bool +4, level: i8 +5, mask: u32 +0 } size 8 align 4\n\n"));
    // Labeled arguments run in source order and fill fields in declaration order.
    CHECK(text.contains("    %0: u32 = const 1\n    %1: i8 = const -2\n    %2: bool = const true\n"
                        "    %3: @Flags = make %2, %1, %0\n"));
    CHECK(text.contains("= field %0, tag\n"));
    // A diverging field value ends the block before the struct is made.
    Frontend g;
    const auto stop = g.dump("struct P(x: Int, y: Int)\nfun stop(): Never {\n    loop {}\n}\n"
                             "fun main(): Int {\n    let p = P(x: 1, y: stop())\n    return p.x\n}\n");
    CHECK(stop.contains("    call @stop()\n    unreachable\n") && !stop.contains("make"));
    // Field assignment rebuilds each enclosing struct from the outside in.
    Frontend h;
    const auto with = h.dump("struct P(var x: Int, var y: Int)\nstruct L(var a: P, var b: P)\n"
                             "fun main(): Int {\n    var l = L(a: P(x: 1, y: 2), b: P(x: 3, y: 4))\n"
                             "    l.b.y += 10\n    return l.b.y\n}\n");
    CHECK(with.contains("    %8: @P = field %7, b\n    %9: i64 = field %8, y\n    %10: i64 = const 10\n"
                        "    %11: i64 = add %9, %10\n    %12: @P = field %7, b\n    %12: @P = with %12, y, %11\n"
                        "    %7: @L = with %7, b, %12\n"));
    CHECK(result("struct P(var x: Int8, var y: Int)\nfun main(): Int {\n    var p = P(x: 1, y: 2)\n    let q = p\n"
                 "    p.x = -5\n    p.y *= 3\n    return Int(p.x) * 100 + p.y * 10 + Int(q.x)\n}\n") == -500 + 60 + 1);
    // Parentheses around a whole target, a field or a local.
    CHECK(result("struct P(var x: Int)\nfun main(): Int {\n    var p = P(x: 1)\n    (p.x) = 2\n    (p.x) += 3\n"
                 "    var n = 1\n    (n) += 4\n    ((p)) = P(x: p.x * 10)\n    (p).x += 1\n    return p.x + n\n}\n") == 56);
    CHECK(traps("struct P(var x: UInt8)\nfun main(): Int {\n    var p = P(x: 0)\n    p.x -= 1\n    return 0\n}\n"));
    // Field values trap like any other arithmetic.
    CHECK(traps("struct P(x: Int8)\nfun main(): Int {\n    let p = P(x: 127)\n    return Int(P(x: p.x + 1).x)\n}\n"));
}

void methods() {
    Frontend f;
    const auto text = f.dump(R"(
struct P(var x: Int) {
    fun get(): Int = x
    fun get(plus: Int): Int = x + plus
    mut fun set(to: Int) { x = to }
    mut fun bump(): Int {
        set(to: get() + 1)
        return self.x
    }
}
fun main(): Int {
    var p = P(x: 1)
    return p.bump() + p.get(plus: 10)
}
)");
    // Methods follow their struct, named after it, with the receiver first.
    CHECK(text.contains("fun @P.get(%0: @P) -> i64 {"));
    CHECK(text.contains("fun @P.get.1(%0: @P, %1: i64) -> i64 {"));
    CHECK(text.contains("fun @P.set(inout %0: @P, %1: i64) -> unit {"));
    CHECK(text.contains("fun @P.get(%0: @P) -> i64 {\nbb0:\n    %1: i64 = field %0, x\n"));
    CHECK(text.contains("    call @P.set(%0, %4)\n    %5: i64 = field %0, x\n"));
    CHECK(Interpreter(*f.program).run().value == 2 + 12);
    // A receiver read before a `mut fun` argument changes it keeps its value.
    CHECK(result(R"(
struct P(var x: Int) {
    fun plus(n: Int): Int = x * 100 + n
    mut fun bump(): Int {
        x += 1
        return x
    }
}
fun pair(a: P, n: Int): Int = a.x * 10 + n
fun main(): Int {
    var p = P(x: 1)
    let a = p.plus(p.bump())
    let b = pair(p, p.bump())
    return a * 1000 + b
}
)") == 102 * 1000 + 23);
    // So does an earlier argument when a later receiver holds the `mut fun` call.
    CHECK(result(R"(
struct P(var x: Int) {
    fun get(): Int = x
    mut fun bump(): P {
        x += 1
        return self
    }
}
fun pair(a: P, n: Int): Int = a.x * 10 + n
fun main(): Int {
    var p = P(x: 1)
    return pair(p, p.bump().get())
}
)") == 12);
    // Nested receivers are written back through every enclosing struct.
    CHECK(result(R"(
struct In(var n: Int8) {
    mut fun inc() { n += 1 }
}
struct Mid(var inner: In, var tag: Bool)
struct Out(var mid: Mid, var other: In) {
    mut fun both() {
        mid.inner.inc()
        other.inc()
        self.other.inc()
    }
}
fun main(): Int {
    var o = Out(mid: Mid(inner: In(n: 1), tag: true), other: In(n: 5))
    o.both()
    o.mid.inner.inc()
    return Int(o.mid.inner.n) * 10 + Int(o.other.n)
}
)") == 37);
    CHECK(traps("struct In(var n: Int8) {\n    mut fun inc() { n += 1 }\n}\n"
                "fun main(): Int {\n    var i = In(n: 127)\n    i.inc()\n    return 0\n}\n"));
}

void tuples() {
    Frontend f;
    const auto text = f.dump("fun main(): Int {\n    let t = (true, 2, Int8(3))\n    return t.1\n}\n");
    // Elements are laid out like struct fields, by decreasing alignment.
    CHECK(text.starts_with("tuple @(Bool, Int, Int8) { 0: bool +8, 1: i64 +0, 2: i8 +9 } size 16 align 8\n\n"));
    CHECK(text.contains("make %0, %1, %2\n") && text.contains("= field %4, 1\n"));
    // A diverging element ends the block before the tuple is made.
    Frontend g;
    const auto stop = g.dump("fun stop(): Never {\n    loop {}\n}\n"
                             "fun main(): Int {\n    let t: (Int, Int) = (1, stop())\n    return t.0\n}\n");
    CHECK(stop.contains("    call @stop()\n    unreachable\n") && !stop.contains("make"));
    // Destructuring, nested element assignment, and the same tuple type shared between functions.
    CHECK(result("fun swap(t: (Int, Int)): (Int, Int) = (t.1, t.0)\n"
                 "fun main(): Int {\n    var t = ((1, 2), Int8(3))\n    t.0.1 = 5\n    t.1 -= 1\n"
                 "    let (a, b) = swap(t.0)\n    return a * 100 + b * 10 + Int(t.1)\n}\n") == 500 + 10 + 2);
    CHECK(traps("fun main(): Int {\n    var t = (UInt8(0), 1)\n    t.0 -= 1\n    return 0\n}\n"));
}

void function_values() {
    Frontend f;
    const auto text = f.dump("fun twice(x: Int): Int = x * 2\n"
                             "fun main(): Int {\n    let g = twice\n    return g(4)\n}\n");
    // A function type is two words, code then environment, with its signature.
    CHECK(text.starts_with("function @fun(Int) -> Int { code: u64 +0, env: u64 +8 } size 16 align 8 "
                           "signature (i64) -> i64\n\n"));
    CHECK(text.contains("= fnref @twice\n") && text.contains("%3: i64 = call %1(%2)\n"));
    // Overloads pick the one of the expected type; values travel through
    // parameters, results, struct fields and tuple elements.
    CHECK(result("fun add(a: Int, b: Int): Int = a + b\nfun add(a: Int): Int = a + 1\n"
                 "fun twice(x: Int): Int = x * 2\n"
                 "fun apply(f: fun(Int) -> Int, x: Int): Int = f(x)\n"
                 "fun pick(first: Bool): fun(Int) -> Int {\n    return if first { twice } else { add }\n}\n"
                 "struct Op(f: fun(Int, Int) -> Int)\n"
                 "fun main(): Int {\n    let op = Op(add)\n    let t = (pick(true), 3)\n"
                 "    return apply(add, 10) * 1000 + pick(false)(1) * 100 + op.f(2, 3) * 10 + t.0(t.1) - 6\n}\n") ==
          11000 + 200 + 50);
    // A field holding a function value, called through a bare field name and
    // through self, and a local that shadows a function.
    CHECK(result("fun one(): Int = 1\nfun two(): Int = 2\n"
                 "struct S(f: fun() -> Int) {\n    fun run(): Int = f() + self.f()\n}\n"
                 "fun main(): Int {\n    let s = S(two)\n    let one = two\n    return s.run() * 10 + one()\n}\n") == 42);
    // The function value runs before the arguments, and a later write does
    // not change which function is called.
    Frontend order;
    const auto run = order.run("fun one(): Int {\n    print(\"one\")\n    return 1\n}\n"
                               "fun two(): Int {\n    print(\"two\")\n    return 2\n}\n"
                               "fun id(x: Int): Int = x\nfun neg(x: Int): Int = -x\n"
                               "fun choose(): fun(Int) -> Int {\n    print(\"choose\")\n    return id\n}\n"
                               "fun main(): Int {\n    var g = id\n"
                               "    let a = g(if true {\n        g = neg\n        one()\n    } else { 0 })\n    return choose()(two()) * 10 + a + g(0)\n}\n");
    CHECK(!run.trapped && run.value == 21 && run.output == "one\nchoose\ntwo\n");
    // A diverging callee or argument ends the block before the call.
    Frontend stop;
    const auto text2 = stop.dump("fun stop(): Never {\n    loop {}\n}\nfun id(x: Int): Int = x\n"
                                 "fun main(): Int {\n    let g = id\n    return g(stop())\n}\n");
    CHECK(text2.contains("    call @stop()\n    unreachable\n") && !text2.contains("call %"));
    // Unit and Never results, and traps inside the callee.
    Frontend unit;
    const auto hello = unit.run("fun hello() { print(\"hi\") }\n"
                                "fun main(): Int {\n    let h = hello\n    h()\n    return 0\n}\n");
    CHECK(!hello.trapped && hello.output == "hi\n");
    // Function types that share their parts are checked and lowered once
    // per type, not once per path through them.
    std::string shared = "fun f0(): Int = 7\n";
    for (int i = 1; i < 64; ++i) shared += std::format("fun f{}() = (f{}, f{})\n", i, i - 1, i - 1);
    shared += "struct S(f: fun() -> Int, deep: fun() -> (fun() -> Int, fun() -> Int))\n";
    shared += "fun main(): Int = f2().1().0()\n";
    CHECK(result(shared) == 7);
    CHECK(traps("fun dec(x: UInt8): UInt8 = x - 1\n"
                "fun main(): Int {\n    let f = dec\n    return Int(f(0))\n}\n"));
}

void closures() {
    Frontend f;
    const auto text = f.dump("fun makeAdder(n: Int): fun(Int) -> Int = { x -> x + n }\n"
                             "fun main(): Int = makeAdder(40)(2)\n");
    // A closure is a function taking its environment first; the environment
    // holds a copy of each captured value, read on entry.
    CHECK(text.contains("environment @makeAdder$closure0 { n: i64 +0 } size 8 align 8\n"));
    CHECK(text.contains("= closure @makeAdder$closure0(%0)\n"));
    CHECK(text.contains("fun @makeAdder$closure0(env %0: u64, %1: i64) -> i64 {\nbb0:\n    %2: i64 = env %0, n\n"));
    // Without captures there is no environment.
    Frontend none;
    const auto plain = none.dump("fun main(): Int {\n    let f = { x: Int -> x * 2 }\n    return f(21)\n}\n");
    CHECK(plain.contains("= closure @main$closure0()\n") && !plain.contains("environment @"));
    CHECK(result("fun main(): Int {\n    let f = { x: Int -> x * 2 }\n    return f(21)\n}\n") == 42);
    // Captures are copies taken when the closure is created; nested closures
    // capture through the closures around them, and names number per creator.
    Frontend nested;
    const auto run = nested.run(R"(
fun main(): Int {
    var k = 1
    let label = "seen"
    let add = { x: Int ->
        let copy = label.clone()
        { y: Int -> print(copy); x + y + k }
    }
    k = 100
    let inner = add(10)
    k = 1000
    let t = (2, true)
    let pick = { -> if t.1 { t.0 } else { 0 } }
    return inner(20) + pick()
}
)");
    CHECK(!run.trapped && run.value == 33 && run.output == "seen\n");
    const auto names = nested.dump(R"(
fun main(): Int {
    let k = 1
    let add = { x: Int -> { y: Int -> x + y + k } }
    let other = { -> k }
    return add(1)(2) + other()
}
)");
    CHECK(names.contains("environment @main$closure0$closure0 { x: i64 +0, k: i64 +8 }"));
    CHECK(names.contains("fun @main$closure1(env %0: u64) -> i64"));
    // A method's closures capture the receiver when they use it, through
    // self, a bare field or a method call.
    CHECK(result(R"(
struct Counter(start: Int, step: Int) {
    fun at(n: Int): Int = start + n * step
    fun next(): fun(Int) -> Int = { n -> at(n) }
    fun first(): fun() -> Int = { -> self.start + step }
    fun constant(): fun() -> Int = { -> 7 }
}
fun main(): Int {
    let c = Counter(start: 10, step: 3)
    return c.next()(2) * 100 + c.first()() + c.constant()()
}
)") == 1620);
    Frontend method;
    const auto receiver = method.dump("struct P(x: Int) {\n    fun get(): fun() -> Int = { -> x }\n    fun no(): fun() -> Int = { -> 1 }\n}\n"
                                     "fun main(): Int = P(x: 1).get()() + P(x: 2).no()()\n");
    CHECK(receiver.contains("environment @P.get$closure0 { self: @P +0 }"));
    CHECK(!receiver.contains("environment @P.no$closure0"));
    // Unit and Never results, an explicit return, and a trap inside the body.
    Frontend unit;
    const auto hello = unit.run("fun main(): Int {\n    let s = \"hi\"\n    let h: fun() -> Unit = { -> print(s) }\n    h()\n    h()\n"
                                "    let r = { x: Int -> if x > 0 { return 1 }; 2 }\n    return r(5) + r(-5)\n}\n");
    CHECK(!hello.trapped && hello.value == 3 && hello.output == "hi\nhi\n");
    CHECK(traps("fun main(): Int {\n    let z = 0\n    let f = { x: Int -> x / z }\n    return f(1)\n}\n"));
    CHECK(result("fun stop(): Never {\n    loop {}\n}\nfun main(): Int {\n    let s: fun() -> Never = { -> stop() }\n    return 4\n}\n") == 4);
}

void generic_functions() {
    // One instance per distinct list of type arguments, named after them;
    // generic functions nobody calls are not lowered.
    Frontend f;
    const auto text = f.dump(R"(
fun identity<T>(x: T): T = x
fun first<A, B>(a: A, b: B): A = identity(a)
fun unused<T>(x: T): T = x
fun main(): Int {
    let n = first(1, true) + identity(2) + identity<Int>(3)
    if first(false, (1, 2)) { return 0 }
    return n
}
)");
    CHECK(text.contains("fun @identity<Int>(%0: i64) -> i64 {"));
    CHECK(text.contains("fun @identity<Bool>(%0: bool) -> bool {"));
    CHECK(text.contains("fun @first<Int, Bool>(%0: i64, %1: bool) -> i64 {"));
    CHECK(text.contains("fun @first<Bool, (Int, Int)>(%0: bool, %1: @(Int, Int)) -> bool {"));
    CHECK(text.find("fun @identity<Int>(") == text.rfind("fun @identity<Int>("));
    CHECK(!text.contains("unused"));
    // Instances of overloads keep the overloads' suffixes.
    Frontend overloads;
    const auto suffixed = overloads.dump("fun pick<T>(x: T): T = x\nfun pick<T>(x: T, y: T): T = y\n"
                                         "fun main(): Int = pick(1) + pick(2, 3)\n");
    CHECK(suffixed.contains("fun @pick<Int>(") && suffixed.contains("fun @pick.1<Int>("));
    // Types that share their parts are substituted once per part, and long
    // type arguments are elided in names, with a suffix where two instances
    // would share one.
    std::string chain = "fun f0<T>() = { a: T, b: T -> 0 }\n";
    for (int i = 1; i <= 30; ++i)
        chain += std::format("fun f{}<T>() = f{}<fun(T, T) -> Int>()\n", i, i - 1);
    chain += "fun main() {\n    let x = f30<Int>()\n}\n";
    Frontend shared;
    CHECK(shared.dump(chain).contains("fun @f30<Int>() -> @fun("));
    std::string tags = "fun tag<T>() {}\nfun h0<T>() {\n    tag<fun(T, Int) -> Int>()\n    tag<fun(T, Bool) -> Int>()\n}\n";
    for (int i = 1; i <= 8; ++i) tags += std::format("fun h{}<T>() {{\n    h{}<fun(T, T) -> Int>()\n}}\n", i, i - 1);
    tags += "fun main() {\n    h8<Int>()\n}\n";
    Frontend elided;
    const auto names = elided.dump(tags);
    CHECK(names.contains("...>() -> unit {") && names.contains("...>.1() -> unit {"));
    // Instances run like any other function, recursion and closures included.
    CHECK(result(R"(
fun count<T>(x: T, n: Int): Int {
    if n == 0 { return 0 }
    return 1 + count(x, n - 1)
}
fun apply<T>(f: fun(T) -> T, x: T): T = f(x)
fun keep<T>(x: T): fun() -> T {
    let c = (x, 0)
    return { -> c.0 }
}
fun main(): Int {
    let n = count("s", 4) * 100 + apply({ n: Int -> n + 1 }, 9)
    if keep(true)() { return n + keep(7)() }
    return 0
}
)") == 417);
}

void generic_structs() {
    // A layout per struct instance and a function per method instance, named
    // after the receiver's type; overloads keep their suffixes.
    Frontend f;
    const auto text = f.dump(R"(
struct Box<T>(value: T) {
    fun get(): T = value
    fun get(x: T): T = x
    fun pair<U>(u: U): (T, U) = (get(), u)
}
fun main(): Int = Box(value: 1).get() + Box(value: 2).get(3) + Box(value: true).pair(4).1
)");
    CHECK(text.contains("struct @Box<Int> { value: i64 +0 } size 8 align 8\n"));
    CHECK(text.contains("struct @Box<Bool> { value: bool +0 } size 1 align 1\n"));
    CHECK(text.contains("fun @Box<Int>.get(%0: @Box<Int>) -> i64 {"));
    CHECK(text.contains("fun @Box<Int>.get.1(%0: @Box<Int>, %1: i64) -> i64 {"));
    CHECK(text.contains("fun @Box<Bool>.pair<Int>(%0: @Box<Bool>, %1: i64) -> @(Bool, Int) {"));
    CHECK(text.contains("fun @Box<Bool>.get(%0: @Box<Bool>) -> bool {"));
    // Instances run like any other function, `mut fun` and closures included.
    CHECK(result(R"(
struct Counter<T>(var last: T, var count: Int) {
    mut fun add(x: T) {
        last = x
        count += 1
    }
    fun reader(): fun() -> Int {
        let n = count
        return { -> n * 10 }
    }
}
fun main(): Int {
    var c = Counter(last: "a", count: 0)
    c.add("b")
    c.add("c")
    var d = Counter(last: false, count: 5)
    d.add(true)
    if d.last { return c.reader()() + d.count }
    return 0
}
)") == 26);
}

void rejected_files() {
    FrontendSession session;
    const auto id = session.sources().add("mir.phs", "fun main(): Float64 = 1.0\n");
    CHECK(id);
    CHECK(!session.lower(*id));
    CHECK(session.diagnostics().error_count() == 1);
    CHECK(!session.lower(*id));
    CHECK(session.diagnostics().error_count() == 1);
}

void long_trees() {
    std::string chain = "fun main(): Int {\n    let c: Bool = false";
    for (int i = 0; i < 50000; ++i) chain += " || false";
    chain += " || true\n    if c { return 1 }\n    return 0\n}\n";
    CHECK(result(chain) == 1);
    std::string sum = "fun main(): Int = 0";
    for (int i = 0; i < 50000; ++i) sum += " + 1";
    sum += "\n";
    CHECK(result(sum) == 50000);
    std::string nested = "fun main(): Int {\n    var i = 0\n";
    for (int i = 0; i < 40; ++i) nested += "while i < 1 {\n";
    nested += "i += 1\n";
    for (int i = 0; i < 40; ++i) nested += "}\n";
    nested += "return i\n}\n";
    CHECK(result(nested) == 1);
}
}  // namespace

void optionals() {
    Frontend f;
    const auto text = f.dump("fun find(n: Int): Int? = if n > 0 { n } else { none }\n"
                             "fun main(): Int = find(2) ?? 0\n");
    // A tag and the value; `none` is all zero; a value is lifted where its optional is expected.
    CHECK(text.starts_with("optional @i64? { some: bool +8, value: i64 +0 } size 16 align 8\n\n"));
    CHECK(text.contains("= none\n") && text.contains("= const true\n") && text.contains("= make "));
    CHECK(result("fun find(n: Int): Int? = if n > 0 { n } else { none }\n"
                 "fun main(): Int {\n    var total = 0\n    if let v = find(2) { total += v }\n"
                 "    if let v = find(-2) { total += 100 } else { total += 10 }\n"
                 "    var next: Int? = 3\n    while let n = next {\n        total += n\n"
                 "        next = if n > 1 { n - 1 } else { none }\n    }\n    return total + (find(0) ?? 1000)\n}\n") ==
          2 + 10 + 6 + 1000);
    // The fallback runs only for `none`.
    CHECK(!traps("fun boom(): Int = 1 / 0\nfun main(): Int {\n    let x: Int? = 1\n    return x ?? boom()\n}\n"));
    CHECK(traps("fun boom(): Int = 1 / 0\nfun main(): Int {\n    let x: Int? = none\n    return x ?? boom()\n}\n"));
    // Equality compares tags, then values, at the deeper optional's type.
    CHECK(result("fun b(x: Bool): Int = if x { 1 } else { 0 }\n"
                 "fun main(): Int {\n    let a: Int? = 5\n    let n: Int? = none\n    let deep: Int? ? = a\n"
                 "    return b(a == 5) + b(a != n) * 2 + b(n == none) * 4 + b(deep == a) * 8 + b(deep == n) * 16 +\n"
                 "        b(deep == 6) * 32\n}\n") == 1 + 2 + 4 + 8);
    // Lifting into arguments, struct fields, tuple elements and results.
    CHECK(result("struct S(let a: Int?, let b: Int)\nfun or(x: Int?, y: Int): Int = x ?? y\n"
                 "fun main(): Int {\n    let s = S(4, 5)\n    let t: (Int?, Int) = (6, 7)\n"
                 "    return or(1, 0) + or(s.a, 0) + or(t.0, 0) + or(none, 20)\n}\n") == 1 + 4 + 6 + 20);
    // A used `if` without `else` is `none` when no condition holds.
    CHECK(result("fun main(): Int {\n    let x: Int? = if false { 5 } else if false { 6 }\n"
                 "    let y: Int? ? = if true { if true { x } } else { none }\n    return (x ?? 9) + ((y ?? none) ?? 20)\n}\n") == 9 + 20);
    // Checked conversions are `none` when the value does not fit.
    CHECK(result("fun main(): Int {\n    let big: UInt = 18446744073709551615\n"
                 "    return Int(Int8.checked(1000) ?? 1) + Int(UInt8.checked(-1) ?? 2) + (Int.checked(big) ?? 4) +\n"
                 "        Int.truncating(UInt.checked(8) ?? 0)\n}\n") == 1 + 2 + 4 + 8);
    // Between signednesses, a negative value fits neither way, whatever the widths.
    CHECK(result("fun main(): Int {\n    let minus: Int8 = -1\n    let big: UInt = 18446744073709551615\n"
                 "    return Int.truncating(UInt.checked(minus) ?? 1) + Int(Int8.checked(big) ?? 2) +\n"
                 "        Int(UInt8.checked(minus) ?? 4)\n}\n") == 1 + 2 + 4);
    // An annotation that adds a level lifts before the test, so `none` is bound.
    CHECK(result("fun main(): Int {\n    let absent: Int? = none\n"
                 "    if let x: Int? = absent { return 1 }\n    return 0\n}\n") == 1);
    // `!` reads the value, or panics with the expression's location.
    CHECK(result("fun main(): Int {\n    let x: Int? ? = 4\n    return x!! + 1\n}\n") == 5);
    Frontend p;
    const auto outcome = p.run("fun main(): Int {\n    let x: Int? = none\n    return x!\n}\n");
    CHECK(outcome.trapped && outcome.trap == 5);
    CHECK(mir::dump(*p.program).contains("    panic \"panic: unwrapped 'none' at mir.phs:3:12\\n\"\n"));
}

void optional_chaining() {
    const std::string point = "struct P(let x: Int, let t: Int?) {\n    fun plus(k: Int): Int = x + k\n"
                              "    fun half(): Int? = if x % 2 == 0 { x / 2 } else { none }\n}\n"
                              "fun boom(): Int = 1 / 0\n";
    // Fields and results are optional, an optional one is not wrapped again.
    CHECK(result(point + "fun main(): Int {\n    let p: P? = P(4, 5)\n    let n: P? = none\n"
                         "    return (p?.x ?? 0) + (n?.x ?? 10) + (p?.t ?? 0) * 100 + (n?.t ?? 2) * 1000 +\n"
                         "        (p?.plus(1) ?? 0) * 10000 + (p?.half() ?? 0) * 100000\n}\n") ==
          4 + 10 + 500 + 2000 + 50000 + 200000);
    // On `none`, the arguments are not evaluated and the method is not called.
    CHECK(!traps(point + "fun main(): Int {\n    let n: P? = none\n    return n?.plus(boom()) ?? 0\n}\n"));
    CHECK(traps(point + "fun main(): Int {\n    let p: P? = P(1, none)\n    return p?.plus(boom()) ?? 0\n}\n"));
}

void arrays() {
    Frontend f;
    const auto text = f.dump("fun main(): Int {\n    let a = [1, 2]\n    return a[1]\n}\n");
    // Three words; a literal allocates its elements; a read checks its bounds first.
    CHECK(text.starts_with("array @[Int] { data: u64 +0, count: i64 +8, capacity: i64 +16 } size 24 align 8 "
                           "element i64\n\n"));
    CHECK(text.contains("= array %0, %1\n") && text.contains("= load %3, %5\n") && text.contains("= lt "));
    CHECK(text.contains("    panic \"panic: array index out of bounds at mir.phs:3:12\\n\"\n"));
    // Reads, stores and compound assignment, through fields, tuple elements and nested arrays.
    CHECK(result("struct P(var x: Int, var t: (Int, [Int]))\n"
                 "fun main(): Int {\n    var a = [1, 2, 3]\n    a[0] = 10\n    a[1] += 5\n"
                 "    var ps = [P(1, (2, [3])), P(4, (5, [6, 7]))]\n    ps[1].x = 40\n    ps[1].t.1[1] *= 10\n"
                 "    var grid = [[1], [2, 3]]\n    grid[1][0] = 20\n"
                 "    return a[0] + a[1] + a[2] + ps[1].x + ps[1].t.1[1] + grid[1][0] + grid[0][0]\n}\n") ==
          10 + 7 + 3 + 40 + 70 + 20 + 1);
    // count and isEmpty, also through `?.`, and elements lifted into optionals.
    CHECK(result("fun b(x: Bool): Int = if x { 1 } else { 0 }\n"
                 "fun main(): Int {\n    let e: [Int] = []\n    let o: [Int]? = [7, 8]\n    let n: [Int]? = none\n"
                 "    var m: [Int?] = [1, none]\n    m[1] = 5\n"
                 "    return e.count + b(e.isEmpty) * 10 + (o?.count ?? 0) * 100 + (n?.count ?? 3) * 1000 +\n"
                 "        b(o?.isEmpty ?? true) * 10000 + m[1]! * 100000\n}\n") ==
          0 + 10 + 200 + 3000 + 0 + 500000);
    // Indices of any integer type; a negative one is out of bounds.
    CHECK(result("fun main(): Int {\n    let a = [5, 6, 7]\n    let i: UInt8 = 2\n    let j: Int16 = 1\n"
                 "    return a[i] + a[j] + a[UInt(0)]\n}\n") == 7 + 6 + 5);
    Frontend p;
    const auto outcome = p.run("fun main(): Int {\n    let a = [5]\n    let i: Int8 = -1\n    return a[i]\n}\n");
    CHECK(outcome.trapped && outcome.trap == 5);
    CHECK(traps("fun main(): Int {\n    let a: [Int] = []\n    return a[0]\n}\n"));
    // A store's index runs before its value, and its bounds check after.
    Frontend order;
    const auto ordered = order.run("fun at(): Int {\n    print(\"index\")\n    return 1\n}\n"
                                   "fun value(): Int {\n    print(\"value\")\n    return 7\n}\n"
                                   "fun main(): Int {\n    var a = [0, 0]\n    a[at()] = value()\n    return a[1]\n}\n");
    CHECK(!ordered.trapped && ordered.value == 7 && ordered.output == "index\nvalue\n");
    Frontend q;
    CHECK(q.run("fun boom(): Int = 1 / 0\nfun main() {\n    var a = [1]\n    a[5] = boom()\n}\n").trap == 2);
    // `mut fun` on an element and stores into the receiver's array; arrays as arguments and results.
    CHECK(result("struct C(var n: Int) {\n    mut fun bump() { n += 1 }\n}\n"
                 "struct Bag(var items: [C]) {\n    mut fun set(i: Int, v: Int) { items[i].n = v }\n}\n"
                 "fun make(): [String] = [\"x\", \"yz\"]\nfun total(cs: [C]): Int = cs[0].n + cs[1].n\n"
                 "fun main(): Int {\n    var b = Bag([C(1), C(2)])\n    b.items[1].bump()\n    b.set(0, 10)\n"
                 "    print(make()[1])\n    return total(b.items)\n}\n") == 10 + 3);
}

int main() {
    target_program();
    control_flow();
    arithmetic();
    short_circuit();
    calls_and_order();
    writes_during_evaluation();
    structs();
    methods();
    tuples();
    function_values();
    closures();
    generic_functions();
    generic_structs();
    optionals();
    optional_chaining();
    arrays();
    rejected_files();
    long_trees();
    std::println("mir tests passed");
}
