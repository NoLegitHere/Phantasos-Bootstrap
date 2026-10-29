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
#include <cstring>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "codegen/x86_64.hpp"
#include "frontend/session.hpp"
#include "mir_interpreter.hpp"

#if defined(__linux__) && defined(__x86_64__)
#include <csetjmp>
#include <sys/mman.h>
#define PHS_JIT 1
#endif

using namespace phs;
using test::Outcome;

namespace {

struct Compiled {
    FrontendSession session;
    const mir::Program* program = nullptr;
    obj::Object object;
    std::string listing;

    explicit Compiled(std::string_view source) {
        const auto id = session.sources().add("x86.phs", source);
        CHECK(id);
        program = session.lower(*id);
        if (!program) {
            session.diagnostics().render(stderr);
            std::println(stderr, "{}", source);
        }
        CHECK(program);
        object = x86_64::emit(*program, &listing);
    }
};

#ifdef PHS_JIT
// Loads the object into executable memory, links its relocations and calls
// main. The trap entry long-jumps back here, and the allocator is the host's.
std::jmp_buf trap_target;
u32 trap_code = 0;
extern "C" [[noreturn]] void on_trap(u32 code) {
    trap_code = code;
    std::longjmp(trap_target, 1);
}

// The runtime's allocator, for closure environments: zeroed, 16-byte aligned
// blocks that live until the tests end.
std::vector<std::unique_ptr<std::byte[]>> blocks;
extern "C" void* on_alloc(u64 size) {
    blocks.push_back(std::make_unique<std::byte[]>(size));
    CHECK(reinterpret_cast<uintptr_t>(blocks.back().get()) % 16 == 0);
    return blocks.back().get();
}

// rax and rdx on return: a String's address and length, or a scalar in rax.
struct Words {
    u64 rax, rdx;
};

struct Native {
    Outcome outcome;
    Words words{};
};

Native run_native(const Compiled& compiled, u32 function) {
    const auto& object = compiled.object;
    const usize trampoline = (object.text.size() + 15) / 16 * 16;
    const usize allocator = trampoline + 16;
    const usize rodata = allocator + 16;
    const usize size = rodata + object.rodata.size();
    // UBSan's function check reads the 8 bytes before a called function, so
    // the code does not start at the mapping's first byte.
    constexpr usize lead = 16;
    void* memory = mmap(nullptr, lead + size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    auto* base = static_cast<u8*>(memory) + lead;
    std::memcpy(base, object.text.data(), object.text.size());
    // movabs rax, target; jmp rax
    const auto jump = [&](usize at, u64 target) {
        base[at] = 0x48;
        base[at + 1] = 0xB8;
        std::memcpy(base + at + 2, &target, 8);
        base[at + 10] = 0xFF;
        base[at + 11] = 0xE0;
    };
    jump(trampoline, reinterpret_cast<u64>(&on_trap));
    jump(allocator, reinterpret_cast<u64>(&on_alloc));
    if (!object.rodata.empty()) std::memcpy(base + rodata, object.rodata.data(), object.rodata.size());
    for (const auto& reloc : object.relocations) {
        const auto& symbol = object.symbols[reloc.symbol];
        const usize at = !symbol.defined ? (symbol.name == x86_64::kAlloc ? allocator : trampoline)
                       : symbol.section == obj::Section::Rodata ? rodata + symbol.offset
                                                                : symbol.offset;
        const auto target = reinterpret_cast<i64>(base) + static_cast<i64>(at);
        const i64 value = target + reloc.addend - (reinterpret_cast<i64>(base) + static_cast<i64>(reloc.offset));
        CHECK(value >= INT32_MIN && value <= INT32_MAX);
        const auto field = static_cast<i32>(value);
        std::memcpy(base + reloc.offset, &field, 4);
    }
    CHECK(mprotect(memory, lead + size, PROT_READ | PROT_EXEC) == 0);
    using Entry = Words (*)();
    Entry entry = nullptr;
    const auto address = reinterpret_cast<uintptr_t>(base + object.symbols[function].offset);
    std::memcpy(&entry, &address, sizeof entry);
    Native native;
    if (setjmp(trap_target) == 0) {
        native.words = entry();
        native.outcome.value = native.words.rax;
        // A String result points into the mapping; keep its bytes past munmap.
        if (compiled.program->functions[function].result == mir::Type::Str) {
            native.words.rax -= reinterpret_cast<u64>(base) + rodata;
            CHECK(native.words.rax + native.words.rdx <= object.rodata.size());
        }
    } else native.outcome = {true, 0, trap_code};
    CHECK(munmap(memory, lead + size) == 0);
    return native;
}
#endif

// Native code must agree with the MIR interpreter: the same value, or the same trap.
void agrees(std::string_view source) {
    Compiled compiled(source);
    const auto expected = test::Interpreter(*compiled.program).run();
#ifdef PHS_JIT
    const auto actual = run_native(compiled, compiled.program->main).outcome;
    const bool value = mir::bit_width(compiled.program->functions[compiled.program->main].result) != 0;
    if (actual.trap != expected.trap || (!expected.trapped && value && actual.value != expected.value)) {
        std::println(stderr, "{}\nexpected trap {} value {}, got trap {} value {}\n{}", source, expected.trap,
                     expected.value, actual.trap, actual.value, compiled.listing);
        CHECK(false);
    }
#else
    (void)expected;
#endif
}

// A String result must hold the same bytes natively as in the interpreter,
// which tracks a String by its literal's index.
void strings_agree(std::string_view source, std::string_view expected) {
    Compiled compiled(source);
    u32 function = 0;
    while (compiled.program->functions[function].name != "f") ++function;
    const auto reference = test::Interpreter(*compiled.program).run(function);
    CHECK(!reference.trapped && compiled.program->strings[reference.value] == expected);
#ifdef PHS_JIT
    const auto native = run_native(compiled, function);
    const auto bytes = std::string_view(reinterpret_cast<const char*>(compiled.object.rodata.data()),
                                        compiled.object.rodata.size());
    if (native.outcome.trapped || bytes.substr(native.words.rax, native.words.rdx) != expected) {
        std::println(stderr, "{}\nexpected \"{}\"\n{}", source, expected, compiled.listing);
        CHECK(false);
    }
#endif
}

void strings() {
    // Literals, copies, branches and results.
    strings_agree(R"(
fun f(): String {
    var text = "first"
    let other: String = "second"
    if 1 < 2 { text = other }
    return text
}
fun main() {}
)", "second");
    // Two-word arguments take two registers, or go on the stack with the
    // registers left for later scalars, and come back in rax:rdx.
    strings_agree(R"(
fun pick(n: Int, a: own String, b: own String): String = if n == 0 { a } else { b }
fun late(a: Int, b: Int, c: Int, d: Int, e: Int, text: own String, g: Int): String = if g == 6 { text } else { "wrong" }
fun stacked(a: String, b: String, c: String, d: own String, e: String): String = d
fun f(): String {
    let x = late(1, 2, 3, 4, 5, pick(1, "no", "\u{1F30D} yes"), 6)
    return stacked("a", "b", "c", x, "e")
}
fun main() {}
)", "\xF0\x9F\x8C\x8D yes");
    // Identical literals share their bytes.
    Compiled shared(R"(
fun f(): String = pick("same", "same")
fun pick(a: String, b: own String): String = b
fun main() {}
)");
    CHECK(shared.program->strings == std::vector<std::string>{"same"});
    CHECK(shared.object.rodata.size() == 4);
}

void object_layout() {
    Compiled c(R"(
fun fib(n: Int): Int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
fun main(): Int = fib(10)
)");
    const auto& object = c.object;
    CHECK(object.symbols.size() == 3);
    CHECK(object.symbols[0].name == "_P3fib" && object.symbols[0].defined);
    CHECK(object.symbols[1].name == "_P4main" && object.symbols[1].defined);
    CHECK(object.symbols[2].name == x86_64::kTrapSymbol && !object.symbols[2].defined);
    for (const auto& symbol : object.symbols)
        if (symbol.defined) {
            CHECK(symbol.offset % 16 == 0);
            CHECK(symbol.size > 0 && symbol.offset + symbol.size <= object.text.size());
        }
    // Three calls to fib, the trap entry from fib's overflow checks, and none from main.
    usize to_fib = 0, to_trap = 0;
    for (const auto& reloc : object.relocations) {
        CHECK(reloc.addend == -4 && reloc.offset + 4 <= object.text.size());
        CHECK(object.text[reloc.offset - 1] == 0xE8);
        to_fib += reloc.symbol == 0;
        to_trap += reloc.symbol == 2;
    }
    CHECK(to_fib == 3 && to_trap == 1);
    CHECK(c.listing.contains("_P3fib:  ; fib\n    0000  55                     push rbp\n"));
    CHECK(c.listing.contains("call _P3fib"));
    CHECK(c.listing.contains("; trap: overflow"));
}

void programs() {
    agrees(R"(
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
)");
    agrees(R"(
fun many(a: Int8, b: Int16, c: Int32, d: Int64, e: UInt8, f: UInt16, g: UInt32, h: UInt64, i: Bool): Int64 {
    if !i { return 0 }
    return Int64(a) * 1000000 + Int64(b) * 10000 + Int64(c) * 100 + d + Int64(e) + Int64(f) + Int64(g) + Int64.truncating(h)
}
fun main(): Int = Int(many(-3, -200, 70000, -5, 250, 65000, 4000000000, 7, true))
)");
    agrees(R"(
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
        while total < i && !(total == 3) { total += 1 }
    }
    return total
}
fun main(): Int {
    let x: Int
    if loops(3) == 3 { x = 100 } else { x = 200 }
    let y: Int = if x > 150 { 1 } else if x > 50 { 2 } else { 3 }
    return x + y + classify(-4) + classify(5) * 10 + loops(9)
}
)");
    agrees("fun main() {}\n");
    agrees("fun main(): Int = -9223372036854775807 - 1 + 9223372036854775807\n");
    agrees("fun main(): Int {\n    var x: Int8 = 127\n    x += 1\n    return 0\n}\n");
    agrees("fun main(): Int {\n    let x: UInt = 0\n    let y = x - 1\n    return 0\n}\n");
    agrees("fun big(): UInt64 = 18446744073709551615\nfun main(): Int {\n    let x = big() * 2\n    return 0\n}\n");
    agrees("fun zero(): Int = 0\nfun main(): Int = 5 / zero()\n");
    agrees("fun zero(): UInt8 = 0\nfun main(): Int = Int(UInt8(5) % zero())\n");
    agrees("fun minus(): Int = -1\nfun main(): Int = (-9223372036854775807 - 1) % minus()\n");
    agrees("fun minus(): Int16 = -1\nfun main(): Int = Int(Int16(-32768) / minus())\n");
    agrees("fun main(): Int {\n    let x: Int32 = -2147483647 - 1\n    return Int(-x)\n}\n");
    // Structs: narrow fields, nesting, Strings, passing and returning every way.
    agrees(R"(
struct Flags(on: Bool, level: Int8, mask: UInt16, wide: Int32)
struct Point(x: Int, y: Int)
struct Box(min: Point, max: Point, flags: Flags)
struct Text(label: String, size: Int)
fun flags(level: Int8): Flags = Flags(wide: -100000, mask: 65535, level: level, on: level < 0)
fun corner(b: Box): Point = b.max
fun swap(b: Box): Box = Box(min: b.max, max: b.min, flags: b.flags)
fun late(a: Int, b: Int, c: Int, d: Int, e: Int, p: Point, f: Int, b2: Box, t: Text): Int =
    a + b + c + d + e + p.x * p.y + f + b2.min.x + t.size + Int(b2.flags.level)
fun deep(n: Int, b: Box): Box {
    if n == 0 { return b }
    return deep(n - 1, Box(min: b.min, max: Point(x: b.max.x + 1, y: b.max.y), flags: b.flags))
}
fun main(): Int {
    let box = swap(Box(min: Point(x: 1, y: 2), max: Point(x: 3, y: 4), flags: flags(-7)))
    let far = deep(20, box)
    var total = corner(far).x * 1000 + Int(far.flags.mask) + Int(far.flags.wide)
    if far.flags.on { total += late(1, 2, 3, 4, 5, corner(box), 6, far, Text(label: "t", size: 9)) }
    return total
}
)");
    // Methods: receivers in registers, on the stack and by address, and inout
    // receivers written back through nested fields.
    agrees(R"(
struct Small(var a: Int8, var b: Bool) {
    fun value(): Int = if b { Int(a) } else { -Int(a) }
    mut fun flip() { b = !b }
}
struct Wide(var x: Int, var y: Int, var z: Int, var small: Small) {
    fun total(k: Int): Int = x + y + z + small.value() + k
    mut fun spin(times: Int): Int {
        var i = 0
        while i < times {
            small.flip()
            x += i
            i += 1
        }
        return total(0)
    }
}
fun late(a: Int, b: Int, c: Int, d: Int, e: Int, f: Int, w: Wide): Int = w.total(a + b + c + d + e + f)
fun main(): Int {
    var w = Wide(x: 1, y: 2, z: 3, small: Small(a: 7, b: true))
    let first = w.spin(times: 3)
    w.small.flip()
    return first * 1000 + late(1, 2, 3, 4, 5, 6, w) + w.small.value()
}
)");
    agrees("struct P(x: Int8, y: Int8)\nfun main(): Int {\n    let p = P(x: 100, y: 27)\n    let q = P(x: p.x + p.y, y: p.x + 28)\n    return Int(q.x)\n}\n");
    agrees("fun boom(): Bool = 1 / 0 == 1\nfun main(): Int {\n    let a = false && boom()\n    let b = true || boom()\n    if a || !b { return 1 }\n    return 0\n}\n");
}

// Random integer expressions over every type, compared with the interpreter.
struct Generator {
    std::mt19937_64 rng{20261026};

    struct Kind {
        std::string_view name;
        bool sign;
        u32 width;
    };
    static constexpr Kind kKinds[] = {{"Int8", true, 8},   {"Int16", true, 16},  {"Int32", true, 32},  {"Int64", true, 64},
                                      {"UInt8", false, 8}, {"UInt16", false, 16}, {"UInt32", false, 32}, {"UInt64", false, 64}};

    u64 pick(u64 limit) { return std::uniform_int_distribution<u64>(0, limit)(rng); }

    std::string literal(const Kind& kind) {
        const u64 max = kind.sign ? (u64{1} << (kind.width - 1)) - 1 : kind.width == 64 ? ~u64{0} : (u64{1} << kind.width) - 1;
        // Favor small values and the edges of the range.
        u64 value = pick(3) == 0 ? max - pick(2) : pick(4) == 0 ? pick(max) : pick(9);
        if (value > max) value = max;
        if (kind.sign && pick(1)) return std::format("({}(-{}))", kind.name, value);
        return std::format("{}({})", kind.name, value);
    }

    std::string expression(const Kind& kind, int depth, usize params) {
        if (depth == 0 || pick(3) == 0) return params && pick(1) ? std::format("p{}", pick(params - 1)) : literal(kind);
        static constexpr std::string_view kOps[] = {"+", "-", "*", "/", "%", "&+", "&-", "&*", "&", "|", "^"};
        const auto op = kOps[pick(std::size(kOps) - 1)];
        if (pick(5) == 0) return std::format("(~{})", expression(kind, depth - 1, params));
        if (kind.sign && pick(6) == 0) return std::format("(-{})", expression(kind, depth - 1, params));
        return std::format("({} {} {})", expression(kind, depth - 1, params), op, expression(kind, depth - 1, params));
    }

    // An if only gets its type from context, so ifs appear where a type is expected: the initializer and the
    // tails of other ifs.
    std::string typed(const Kind& kind, int depth, usize params) {
        if (depth == 0 || pick(2) != 0) return expression(kind, depth, params);
        return std::format("if {} < {} {{ {} }} else {{ {} }}", expression(kind, depth - 1, params),
                           expression(kind, depth - 1, params), typed(kind, depth - 1, params),
                           typed(kind, depth - 1, params));
    }

    std::string program() {
        const auto& kind = kKinds[pick(std::size(kKinds) - 1)];
        const usize params = pick(8);
        std::string source = std::format("fun f(");
        for (usize i = 0; i < params; ++i) source += std::format("{}p{}: {}", i ? ", " : "", i, kind.name);
        source += std::format("): {} {{\n    let v: {} = {}\n    return v\n}}\n", kind.name, kind.name, typed(kind, 4, params));
        source += std::format("fun main(): Int {{\n    let r = f(");
        for (usize i = 0; i < params; ++i) source += std::format("{}{}", i ? ", " : "", literal(kind));
        source += ")\n    return Int.truncating(r)\n}\n";
        return source;
    }

    // A struct of random fields, built from labels in a random order, passed
    // after a random number of words so that it lands in registers, on the
    // stack or behind a pointer, returned the same ways, and updated one
    // field at a time, sometimes with checked arithmetic that may trap.
    std::string struct_program() {
        const usize fields = 1 + pick(5), pad = pick(6);
        std::vector<const Kind*> kinds;
        for (usize i = 0; i < fields; ++i) kinds.push_back(&kKinds[pick(std::size(kKinds) - 1)]);
        const auto make = [&](const auto& value) {
            std::vector<usize> order(fields);
            for (usize i = 0; i < fields; ++i) order[i] = i;
            std::shuffle(order.begin(), order.end(), rng);
            std::string text = "S(";
            for (usize i = 0; i < fields; ++i) text += std::format("{}f{}: {}", i ? ", " : "", order[i], value(order[i]));
            return text + ')';
        };
        std::string source = "struct S(";
        for (usize i = 0; i < fields; ++i) source += std::format("{}var f{}: {}", i ? ", " : "", i, kinds[i]->name);
        source += ") {\n    mut fun adjust(by: Int) {\n";
        for (usize i = 0; i < fields; ++i)
            if (pick(1)) source += std::format("        f{} = f{} &+ {}\n", i, i, literal(*kinds[i]));
        source += "        if by > 0 { self.adjust(by: by - 1) }\n    }\n    fun sum(): Int = mix(self)\n}\n";
        source += "struct T(var s: S, var n: Int)\nfun step(";
        for (usize i = 0; i < pad; ++i) source += std::format("w{}: Int, ", i);
        source += "s: S, last: Int): S = ";
        source += make([&](usize i) { return std::format("s.f{} &+ {}", i, literal(*kinds[i])); });
        source += "\nfun mix(s: S): Int {\n    var h = 0\n";
        for (usize i = 0; i < fields; ++i) source += std::format("    h = h &* 31 &+ Int.truncating(s.f{})\n", i);
        source += "    return h\n}\nfun main(): Int {\n    var s = ";
        source += make([&](usize i) { return literal(*kinds[i]); });
        for (int round = 0; round < 2; ++round) {
            source += "\n    s = step(";
            for (usize i = 0; i < pad; ++i) source += std::format("{}, ", i);
            source += "s, 7)";
        }
        source += "\n    var t = T(s: s, n: 1)";
        for (usize i = pick(4); i-- > 0;) {
            const usize f = pick(fields - 1);
            const auto* op = pick(2) == 0 ? "+=" : pick(1) ? "&=" : "=";
            source += std::format("\n    {}.f{} {} {}", pick(1) ? "s" : "t.s", f, op, literal(*kinds[f]));
        }
        if (pick(1)) source += std::format("\n    t.s.adjust(by: {})\n    s.adjust(by: {})", pick(3), pick(3));
        return source + "\n    return s.sum() &+ mix(t.s)\n}\n";
    }

    // A tuple of random elements, passed after a random number of words,
    // returned, held in a struct and another tuple, updated one element at a
    // time and destructured.
    std::string tuple_program() {
        const usize count = 2 + pick(4), pad = pick(6);
        std::vector<const Kind*> kinds;
        for (usize i = 0; i < count; ++i) kinds.push_back(&kKinds[pick(std::size(kKinds) - 1)]);
        const auto make = [&](const auto& value) {
            std::string text = "(";
            for (usize i = 0; i < count; ++i) text += std::format("{}{}", i ? ", " : "", value(i));
            return text + ')';
        };
        const std::string type = make([&](usize i) { return std::string(kinds[i]->name); });
        std::string source = std::format("struct T(var t: {}, var n: Int)\nfun step(", type);
        for (usize i = 0; i < pad; ++i) source += std::format("w{}: Int, ", i);
        source += std::format("t: {}, last: Int): {} = ", type, type);
        source += make([&](usize i) { return std::format("t.{} &+ {}", i, literal(*kinds[i])); });
        source += std::format("\nfun mix(t: {}): Int {{\n    var h = 0\n", type);
        for (usize i = 0; i < count; ++i) source += std::format("    h = h &* 31 &+ Int.truncating(t.{})\n", i);
        source += std::format("    return h\n}}\nfun main(): Int {{\n    var t: {} = ", type);
        source += make([&](usize i) { return literal(*kinds[i]); });
        for (int round = 0; round < 2; ++round) {
            source += "\n    t = step(";
            for (usize i = 0; i < pad; ++i) source += std::format("{}, ", i);
            source += "t, 7)";
        }
        source += "\n    var n = (T(t: t, n: 1), t)";
        static constexpr std::string_view kPlaces[] = {"t", "n.0.t", "n.1"};
        for (usize i = pick(4); i-- > 0;) {
            const usize e = pick(count - 1);
            const auto* op = pick(2) == 0 ? "+=" : pick(1) ? "&=" : "=";
            source += std::format("\n    {}.{} {} {}", kPlaces[pick(2)], e, op, literal(*kinds[e]));
        }
        source += "\n    let " + make([&](usize i) { return std::format("a{}", i); }) + " = n.1";
        source += "\n    return mix(t) &+ mix(n.0.t) &+ mix(n.1)";
        for (usize i = 0; i < count; ++i) source += std::format(" &+ Int.truncating(a{})", i);
        return source + "\n}\n";
    }

    // A closure over random parameters and captured locals, returned from the
    // function that creates it so that its environment outlives that frame,
    // sometimes nested so that the inner closure captures through the outer.
    std::string closure_program() {
        const auto& kind = kKinds[pick(std::size(kKinds) - 1)];
        const usize params = 1 + pick(3), captures = pick(5);
        const bool nested = params > 1 && pick(1);
        const auto names = [&](usize from, usize to) {
            std::string text;
            for (usize i = from; i < to; ++i) text += std::format("{}p{}", i > from ? ", " : "", i);
            return text;
        };
        const auto types = [&](usize count) {
            std::string text;
            for (usize i = 0; i < count; ++i) text += std::format("{}{}", i ? ", " : "", kind.name);
            return text;
        };
        const std::string inner = std::format("fun({}) -> {}", types(params - (nested ? 1 : 0)), kind.name);
        std::string source = std::format("fun make(): {} {{\n", nested ? std::format("fun({}) -> {}", kind.name, inner) : inner);
        for (usize i = params; i < params + captures; ++i) source += std::format("    let p{}: {} = {}\n", i, kind.name, literal(kind));
        const std::string body = typed(kind, 3, params + captures);
        if (nested) source += std::format("    return {{ p0 -> {{ {} -> {} }} }}\n}}\n", names(1, params), body);
        else source += std::format("    return {{ {} -> {} }}\n}}\n", names(0, params), body);
        source += "fun main(): Int {\n    let f = make()\n    let r = f(";
        for (usize i = 0; i < params; ++i) {
            if (nested && i == 1) source += ")(";
            else if (i) source += ", ";
            source += literal(kind);
        }
        return source + ")\n    return Int.truncating(r)\n}\n";
    }
};

void function_values() {
    // A function used as a value is called through its thunk, which takes
    // the environment first; the value is the thunk's address and a null
    // environment.
    Compiled c("fun twice(x: Int): Int = x * 2\nfun main(): Int {\n    let f = twice\n    return f(21)\n}\n");
    CHECK(c.object.symbols.size() == 4);
    CHECK(c.object.symbols[3].name == "_P$5thunk5twice" && c.object.symbols[3].defined);
    CHECK(c.object.symbols[3].offset % 16 == 0 && c.object.symbols[3].size > 0);
    CHECK(c.listing.contains("lea rax, [rip+_P$5thunk5twice]") && c.listing.contains("call r11"));
    CHECK(c.listing.contains("_P$5thunk5twice:  ; twice$thunk\n"));
    agrees("fun twice(x: Int): Int = x * 2\nfun main(): Int {\n    let f = twice\n    return f(21)\n}\n");
    // Passing, returning and storing function values; overloads; Unit and
    // Never results; one thunk however often a function is named.
    agrees(R"(
fun add(a: Int, b: Int): Int = a + b
fun add(a: Int): Int = a + 1
fun twice(x: Int): Int = x * 2
fun apply(f: fun(Int) -> Int, x: Int): Int = f(x)
fun pick(first: Bool): fun(Int) -> Int {
    return if first { twice } else { add }
}
struct Op(f: fun(Int, Int) -> Int, name: String)
fun nothing() {}
fun stop(): Never {
    loop {}
}
fun main(): Int {
    let op = Op(add, "add")
    let pair = (pick(true), 3)
    let n = nothing
    n()
    let s: fun() -> Never = stop
    var g = twice
    let a = g(if true {
        g = add
        1
    } else { 0 })
    return apply(add, 10) * 1000 + pick(false)(1) * 100 + op.f(2, 3) * 10 + pair.0(pair.1) + a + g(0)
}
)");
    // Stack arguments, a result returned through ret*, and large arguments
    // passed by address all shift by the environment's register.
    agrees(R"(
struct Big(a: Int, b: Int, c: Int)
fun sum(a: Int, b: Int, c: Int, d: Int, e: Int, f: Int, g: Int, h: Int8): Int = a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + Int(h) * 8
fun make(x: Int, y: Int): Big = Big(a: x, b: y, c: x * y)
fun total(b: Big, scale: Int, text: String, last: Int): Int = (b.a + b.b + b.c) * scale + last
fun last(a: String, b: String, c: String, d: Int): Int = d
fun main(): Int {
    let s = sum
    let m = make
    let t = total
    let l = last
    let big = m(3, 4)
    return s(1, 2, 3, 4, 5, 6, 7, Int8(8)) + t(big, 2, "x", big.c) + l("a", "b", "c", 9)
}
)");
    // A trap inside a function called through its value.
    agrees("fun dec(x: UInt8): UInt8 = x - 1\nfun main(): Int {\n    let f = dec\n    return Int(f(0))\n}\n");
    strings_agree(R"(
fun greet(loud: Bool): String = if loud { "HELLO" } else { "hello" }
fun f(): String {
    let g = greet
    return g(true)
}
fun main() {}
)", "HELLO");
}

void closures() {
    // A closure's code is its own function, which takes the environment
    // first; a capturing closure allocates its environment from the runtime.
    Compiled c("fun makeAdder(n: Int): fun(Int) -> Int = { x -> x + n }\nfun main(): Int = makeAdder(40)(2)\n");
    CHECK(c.object.symbols[2].name == "_P18makeAdder$closure0" && c.object.symbols[2].defined);
    CHECK(c.listing.contains("call _P$alloc") && c.listing.contains("lea rax, [rip+_P18makeAdder$closure0]"));
    CHECK(std::ranges::any_of(c.object.symbols, [](const obj::Symbol& s) { return s.name == x86_64::kAlloc && !s.defined; }));
    agrees("fun makeAdder(n: Int): fun(Int) -> Int = { x -> x + n }\nfun main(): Int = makeAdder(40)(2)\n");
    Compiled plain("fun main(): Int {\n    let f = { x: Int -> x * 2 }\n    return f(21)\n}\n");
    CHECK(!plain.listing.contains("_P$alloc"));
    agrees("fun main(): Int {\n    let f = { x: Int -> x * 2 }\n    return f(21)\n}\n");
    // Captures of every size and kind: narrow integers, Bools, Strings, small
    // and large structs, tuples and function values; nested closures and a
    // method's receiver.
    agrees(R"(
struct Small(a: Int8, b: Bool)
struct Big(a: Int, b: Int, c: Int)
struct Counter(start: Int, step: Int) {
    fun next(): fun(Int) -> Int = { n -> start + n * step }
}
fun keep(b: Big): fun() -> Big = { -> Big(a: b.c, b: b.b, c: b.a) }
fun main(): Int {
    let n8: Int8 = -5
    let u16: UInt16 = 65000
    let flag = true
    let text = "abc"
    let small = Small(a: 3, b: true)
    let big = Big(a: 1, b: 2, c: 3)
    let pair = (7, false)
    let twice = { x: Int -> x * 2 }
    let all = { x: Int ->
        var total = x + Int(n8) + Int(u16) + Int(small.a) + twice(big.c) + pair.0
        if flag && small.b && !pair.1 { total += 1 }
        let s = text.clone()
        total
    }
    let nest = { a: Int -> { b: Int -> { -> a * 100 + b * 10 + big.b } } }
    let swapped = keep(big)()
    let c = Counter(start: 10, step: 3)
    return all(1) + nest(1)(2)() + swapped.a * 1000 + c.next()(2)
}
)");
    // Many environments, beyond what fits in one block, stay intact.
    agrees(R"(
struct Big(a: Int, b: Int, c: Int)
fun hold(b: Big): fun() -> Int = { -> b.a + b.b * 2 + b.c * 3 }
fun main(): Int {
    var total = 0
    var i = 0
    while i < 2000 {
        total += hold(Big(a: i, b: i + 1, c: i + 2))()
        i += 1
    }
    return total
}
)");
    // Stack arguments, ret* and a trap inside a closure body.
    agrees(R"(
struct Big(a: Int, b: Int, c: Int)
fun main(): Int {
    let k = 9
    let f = { a: Int, b: Int, c: Int, d: Int, e: Int, g: Int, h: Int -> Big(a: a + b + c, b: d + e + g, c: h + k) }
    let r = f(1, 2, 3, 4, 5, 6, 7)
    return r.a * 10000 + r.b * 100 + r.c
}
)");
    agrees("fun main(): Int {\n    let limit: UInt8 = 255\n    let f = { x: UInt8 -> limit + x }\n    return Int(f(1))\n}\n");
    strings_agree(R"(
fun f(): String {
    let a = "first"
    let b = "second"
    let g = { pick: Bool -> if pick { a.clone() } else { b.clone() } }
    return g(false)
}
fun main() {}
)", "second");
}

void random_expressions() {
    Generator generator;
    for (int i = 0; i < 400; ++i) agrees(generator.program());
    for (int i = 0; i < 200; ++i) agrees(generator.struct_program());
    for (int i = 0; i < 200; ++i) agrees(generator.tuple_program());
    for (int i = 0; i < 200; ++i) agrees(generator.closure_program());
}
}  // namespace

void optionals() {
    // Optionals of scalars, of a nested optional and of a struct passed by
    // address agree with the interpreter, through every lifting site.
    agrees("fun find(n: Int): Int? = if n > 0 { n } else { none }\n"
           "fun main(): Int {\n    var total = 0\n    if let v = find(2) { total += v }\n"
           "    var next: Int? = 3\n    while let n = next {\n        total += n\n"
           "        next = if n > 1 { n - 1 } else { none }\n    }\n    return total + (find(0) ?? 1000)\n}\n");
    agrees("fun b(x: Bool): Int = if x { 1 } else { 0 }\n"
           "fun main(): Int {\n    let a: Int8? = 5\n    let n: Int8? = none\n    let deep: Int8? ? = a\n"
           "    return b(a == 5) + b(a != n) * 2 + b(n == none) * 4 + b(deep == a) * 8 + b(deep == n) * 16 +\n"
           "        b(deep == 6) * 32 + Int((deep ?? none) ?? 0) * 64\n}\n");
    agrees("struct Big(let a: Int, let b: Int, let c: Int)\n"
           "fun pick(n: Int): Big? = if n == 0 { none } else { Big(n, n + 1, n + 2) }\n"
           "fun sum(x: Big?): Int {\n    if let v = x { return v.a + v.b + v.c }\n    return -1\n}\n"
           "fun main(): Int = sum(pick(1)) * 10 + sum(pick(0)) + sum(Big(1, 1, 1))\n");
    agrees("struct S(var a: Int?, let b: Bool?)\n"
           "fun main(): Int {\n    var s = S(none, true)\n    s.a = 7\n    let t: (UInt8?, Int) = (UInt8(3), 4)\n"
           "    let f: fun(Int) -> Int? = { x -> x }\n"
           "    let flag: Int = if s.b ?? false { 100 } else { 0 }\n"
           "    return (s.a ?? 0) + Int(t.0 ?? UInt8(0)) + (f(9) ?? 0) + flag\n}\n");
    agrees("fun main(): Int {\n    let x: Int? ? = 4\n    return x!! + 1\n}\n");
    agrees("fun main(): Int {\n    let x: Int8? = none\n    return Int(x!)\n}\n");
}

int main() {
    object_layout();
    programs();
    strings();
    function_values();
    closures();
    optionals();
    random_expressions();
    std::println("x86_64 tests passed");
}
