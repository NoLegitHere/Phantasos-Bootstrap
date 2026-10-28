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
#include "sema/validate.hpp"
#include "syntax/ast_dump.hpp"

using namespace phs;

namespace {
struct Frontend {
    FrontendSession session;
    SourceManager& sources = session.sources();
    Interner& names = session.names();
    DiagEngine& diag = session.diagnostics();

    const ast::File* parse(std::string_view source) {
        const auto file = sources.add("test.phs", source);
        CHECK(file);
        const auto* ast = session.parse(*file);
        if (diag.has_errors(*file)) diag.render(stderr);
        CHECK(!diag.has_errors(*file));
        return ast;
    }

    void check(std::string_view source, std::initializer_list<DiagCode> expected) {
        const auto* file = parse(source);
        const std::string before = dump_ast(*file, names);
        validate_file(*file, names, diag);
        CHECK(dump_ast(*file, names) == before);
        if (diag.error_count() != expected.size()) diag.render(stderr);
        CHECK(diag.error_count() == expected.size());
        std::vector<DiagCode> actual;
        for (const Diagnostic& d : diag.all()) {
            CHECK(d.span.file == file->file);
            actual.push_back(d.code);
        }
        std::vector<DiagCode> wanted(expected);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        CHECK(actual == wanted);
    }
};

void duplicate_bindings() {
    Frontend f;
    f.check(R"(
type T = Int
struct T {}
let global = 1
var global = 2
fun f<A, A>(x: Int, x: Int) {
    let local = 1
    let local = 2
    if true { let local = 3 }
}
struct S(x: Int, x: Int) {}
enum E { Case(x: Int, x: Int), Case }
)", {DiagCode::DuplicateBinding, DiagCode::DuplicateBinding, DiagCode::DuplicateBinding,
      DiagCode::DuplicateBinding, DiagCode::DuplicateBinding, DiagCode::DuplicateBinding,
      DiagCode::DuplicateBinding, DiagCode::DuplicateBinding});
    for (const Diagnostic& d : f.diag.all()) {
        CHECK(d.notes.size() == 1);
        CHECK(d.notes[0].span.lo < d.span.lo);
        const auto& text = f.sources.file(d.span.file).text;
        CHECK(text.substr(d.span.lo, d.span.hi - d.span.lo) ==
              text.substr(d.notes[0].span.lo, d.notes[0].span.hi - d.notes[0].span.lo));
    }
}

void declaration_contexts() {
    Frontend f;
    f.check(R"(
init {}
deinit {}
type Missing
type Bounded: Trait = Int
interface I {
    type Item
    type Index: Trait = Int
    class C { type Missing }
}
class C {
    init {}
    init {}
    init(x: Int) {}
    init(x: String) {}
    deinit {}
    deinit {}
}
)", {DiagCode::InvalidDeclarationContext, DiagCode::InvalidDeclarationContext,
      DiagCode::InvalidTypeAlias, DiagCode::InvalidTypeAlias, DiagCode::InvalidTypeAlias,
      DiagCode::InvalidDeclarationContext, DiagCode::InvalidDeclarationContext});
}

void promoted_fields() {
    for (std::string_view source : {
            "class C(let x: Int) { let x: Int = 1 }",
            "class C(var x: Int) { var x: Int = 1 }",
            "actor A(let x: Int) { let x: Int = 1 }",
            "actor A(var x: Int) { let x: Int = 1 }",
            "struct S(x: Int) { let x: Int = 1 }",
            "struct S(let x: Int) { var x: Int = 1 }",
            "struct S(var x: Int) { let x: Int = 1 }",
        }) {
        Frontend f;
        f.check(source, {DiagCode::DuplicateBinding});
        const auto& d = f.diag.all().front();
        CHECK(d.notes.size() == 1 && d.notes[0].span.lo == source.find("x:"));
        CHECK(d.span.lo == source.rfind("x:"));
    }
    Frontend f;
    f.check(R"(
class C(x: Int) { let x: Int = 1 }
actor A(x: Int) { let x: Int = 1 }
struct S(x: Int) { fun f(x: Int) { let x = 1 } }
class Outer(let x: Int) { class Inner(let x: Int) {} }
)", {});
    Frontend duplicate;
    duplicate.check("class C(let x: Int, let x: Int) { let x: Int = 1 }",
                    {DiagCode::DuplicateBinding, DiagCode::DuplicateBinding});
    for (const Diagnostic& d : duplicate.diag.all()) CHECK(d.notes[0].span.lo == 12);
}

void defer_boundaries() {
    Frontend f;
    f.check(R"(
async fun f() throws(Error) {
    loop {
        defer {
            break
            continue
            return
            throw failure
            await task()
            loop { break; continue }
            let handled = try? unsafe { throw failure }
        }
        break
        continue
    }
    await task()
    throw failure
    return
}
)", {DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow,
      DiagCode::ThrowOutsideThrows, DiagCode::AwaitOutsideAsync});
    Frontend nested;
    nested.check(R"(
fun f() throws(Error) {
    defer {
        loop { defer { break }; break }
        let x = try? unsafe { defer { throw failure }; throw failure }
        loop { return }
    }
    let x = try? unsafe { defer { throw failure } }
}
)", {DiagCode::InvalidControlFlow, DiagCode::ThrowOutsideThrows,
      DiagCode::InvalidControlFlow, DiagCode::ThrowOutsideThrows});
    Frontend valid;
    valid.check(R"(
fun f() {
    defer {
        for x in items { continue; break }
        while true { break; continue }
        loop { break }
        defer { loop { break }; let x = try? cleanup() }
        let x = try? unsafe { throw failure }
    }
}
)", {});
}

void flow_contexts() {
    Frontend f;
    f.check(R"(
let global = if true { return 1 } else { 2 }
fun plain() {
    break
    continue
    throw error
    await task()
    loop { if true { break }; continue }
    break
    while if true { break; true } {}
    for x in if true { continue; items } {}
}
class C { deinit { throw error } }
async fun asynchronous() { await task() }
fun after() { await task() }
)", {DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow,
      DiagCode::ThrowOutsideThrows, DiagCode::AwaitOutsideAsync, DiagCode::InvalidControlFlow,
      DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow, DiagCode::ThrowOutsideThrows,
      DiagCode::AwaitOutsideAsync});
}

void isolated_defaults_and_initializers() {
    Frontend f;
    f.check(R"(
@attr(if true { break })
async fun f(x: Int = await task()) throws(Error) { await task(); throw error }
class C(x: Int = if true { return 1 }) : Base(if true { continue }) {
    let field = if true { return 2 }
    init(x: Int = if true { return 3 }) throws(Error) { throw error }
}
)", {DiagCode::InvalidControlFlow, DiagCode::AwaitOutsideAsync, DiagCode::InvalidControlFlow,
      DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow});
}

void patterns() {
    Frontend f;
    f.check(R"(
fun f(x: E) = match x {
    .A(a, a) -> a
    .A(a) | .B(b) -> 0
    .A(a) | .B(a) -> a
    .A(.B(a) | .C(a), a) -> a
    .A(is T as a, a) -> a
    .A(a) | .B(a, b) -> a
    .A(.B(a, b) | .C(b, a)) -> a
    .A(.B(a) | .C(a)) | .D(a) -> a
}
)", {DiagCode::DuplicateBinding, DiagCode::PatternBindingMismatch, DiagCode::DuplicateBinding,
      DiagCode::DuplicateBinding, DiagCode::PatternBindingMismatch});
}

void alternative_recovery_is_order_independent() {
    for (std::string_view pattern : {
            ".Pair(.A(a) | .B, a)", ".Pair(.A | .B(a), a)",
            ".Pair(a, .A(a) | .B)", ".Pair(a, .A | .B(a))",
        }) {
        Frontend f;
        const std::string source = "fun f(x: E) = match x { " + std::string(pattern) + " -> a }";
        f.check(source, {DiagCode::PatternBindingMismatch, DiagCode::DuplicateBinding});
        for (const Diagnostic& d : f.diag.all()) {
            if (d.code != DiagCode::DuplicateBinding) continue;
            CHECK(d.notes.size() == 1);
            CHECK(d.notes[0].span.lo == source.find('a', source.find(".Pair(") + 6));
            CHECK(d.notes[0].span.lo < d.span.lo);
        }
    }
    Frontend f;
    constexpr std::string_view source = "fun f(x: E) = match x { .Pair(.A(a) | .B(a) | .C, a) -> a }";
    f.check(source, {DiagCode::PatternBindingMismatch, DiagCode::DuplicateBinding});
    CHECK(f.diag.all().back().notes[0].span.lo == source.find(".A(a)") + 3);
}

void source_order_is_explicit() {
    Frontend f;
    f.check(R"(
@attr(if true { break })
class C<T, T>(let x: Int) {
    let x: Int = 1
    fun f() = match await task() {
        .A(a, a) if await task() -> await task()
        .B(b, b) -> 0
    }
}
)", {DiagCode::InvalidControlFlow, DiagCode::DuplicateBinding, DiagCode::DuplicateBinding,
      DiagCode::AwaitOutsideAsync, DiagCode::DuplicateBinding, DiagCode::AwaitOutsideAsync,
      DiagCode::AwaitOutsideAsync, DiagCode::DuplicateBinding});
    const auto ordered = f.diag.in_source_order();
    CHECK(ordered.size() == f.diag.all().size());
    CHECK(std::ranges::is_sorted(ordered, {}, [](const Diagnostic* d) { return d->span.lo; }));
    CHECK(ordered.front()->code == DiagCode::InvalidControlFlow);
    CHECK(f.diag.all().front().code == DiagCode::DuplicateBinding);
}

void valid_contexts_and_shadowing() {
    Frontend f;
    f.check(R"(
type Integer = Int
let x = 1
class C<T>(let x: T) {
    type Integer = Int
    init { return }
    init(x: T) throws(Error) { throw error }
    deinit { cleanup() }
    fun f<T>(x: T) {
        let x = 2
        if let x = maybe { let x = 3 }
        while true { for x in xs { continue }; break }
        for x in xs { let x = 4; continue }
        loop { break }
    }
}
fun f(x: Int): Int = x
fun f(x: String): String = x
async fun f() throws(Error) {
    await task()
    throw error
}
fun catches() {
    let x = try? unsafe { throw error }
    let y = try? task()
}
enum E { A(Int), B(value: Int) }
interface I { type Element: Trait; type Index = Int }
)", {});
}

void expression_children() {
    // Each container must visit its children, including both sides of binary
    // expressions, arguments, interpolation segments and match guards.
    for (std::string_view source : {
            "fun f() = (await task())", "fun f() = [await task()]",
            "fun f() = 1 + await task()", "fun f() = (await task()) + 1",
            "fun f() = call(await task())", "fun f() = (await task())()",
            "fun f() = xs[await task()]", "fun f() = (await task())[0]",
            "fun f() = (await task()).x", "fun f() = (await task())?.x",
            "fun f() = (await task())!", "fun f() = (await task()) as Int",
            "fun f() = (await task()).x<Int>()", "fun f() = unsafe { await task() }",
            "fun f() = \"x=\\(await task())\"", "fun f() = if await task() {}",
            "fun f() = match await task() { _ -> 0 }",
            "fun f() = match x { _ if await task() -> 0 }",
            "fun f() = match x { _ -> await task() }",
            "fun f() { defer { await task() } }",
            "fun f() { xs[await task()] = 1 }", "fun f() { x = await task() }",
        }) {
        Frontend f;
        f.check(source, {DiagCode::AwaitOutsideAsync});
    }
    Frontend f;
    f.check("fun f() { let x = try? unsafe { throw error }; throw error }", {DiagCode::ThrowOutsideThrows});
}

void flat_trees_and_multiple_files() {
    Frontend f;
    std::string source = "let sum = 1";
    for (int i = 0; i < 20000; ++i) source += " + 1";
    source += "\nlet chain = value";
    for (int i = 0; i < 20000; ++i) source += ".member";
    source += "\nfun alternatives(x: E) = match x { .A(a)";
    for (int i = 0; i < 2000; ++i) source += " | .B(a)";
    source += " -> a }";
    const auto* first = f.parse(source);
    validate_file(*first, f.names, f.diag);
    CHECK(f.diag.error_count() == 0);
    const auto* invalid = f.parse("fun f() { break }");
    validate_file(*invalid, f.names, f.diag);
    CHECK(f.diag.error_count() == 1);
    const auto* second = f.parse("let sum = 1\nlet chain = 2");
    validate_file(*second, f.names, f.diag);
    CHECK(f.diag.error_count() == 1);
    CHECK(f.diag.all()[0].span.file == invalid->file);
}

void parameter_binding_contexts() {
    Frontend valid;
    valid.check(R"(
class C(let x: Int, var y: Int) { init(z: Int) {} }
struct S(let x: Int, var y: Int, z: Int) {}
actor A(let x: Int, var y: Int) {}
fun regular(x: borrow Int, y: inout Int, z: own Int = 1) {}
)", {});

    Frontend invalid;
    invalid.check(R"(
fun top(let x: Int, var y: Int) {}
extern "C" { fun foreign(let x: Int) }
class C {
    fun method(var x: Int) {}
    init(let x: Int, var y: Int) {}
}
interface I { fun required(let x: Int) }
)", {DiagCode::InvalidParameterBinding, DiagCode::InvalidParameterBinding,
      DiagCode::InvalidParameterBinding, DiagCode::InvalidParameterBinding,
      DiagCode::InvalidParameterBinding, DiagCode::InvalidParameterBinding, DiagCode::InvalidParameterBinding});
    for (const auto& d : invalid.diag.all()) {
        const auto& text = invalid.sources.file(d.span.file).text;
        const auto keyword = text.substr(d.span.lo, d.span.hi - d.span.lo);
        CHECK(keyword == "let" || keyword == "var");
    }
}
}  // namespace

int main() {
    duplicate_bindings();
    declaration_contexts();
    promoted_fields();
    defer_boundaries();
    flow_contexts();
    isolated_defaults_and_initializers();
    patterns();
    alternative_recovery_is_order_independent();
    source_order_is_explicit();
    valid_contexts_and_shadowing();
    expression_children();
    flat_trees_and_multiple_files();
    parameter_binding_contexts();
}
