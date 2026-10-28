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
    std::string source;
    const ast::File* file = nullptr;
    Resolution result;

    void check(std::string text, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        source = std::move(text);
        const auto id = sources.add("resolve.phs", source);
        CHECK(id);
        file = session.parse(*id);
        if (!diag.has_errors(*id)) validate_file(*file, names, diag);
        if (diag.has_errors(*id)) diag.render(stderr);
        CHECK(!diag.has_errors(*id));
        const auto before = dump ? dump_ast(*file, names) : std::string{};
        result = resolve_file(*file, names, diag);
        if (dump) CHECK(dump_ast(*file, names) == before);
        if (diag.error_count() != expected.size()) diag.render(stderr);
        CHECK(diag.error_count() == expected.size());
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : diag.all()) actual.push_back(d.code);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        CHECK(actual == wanted);
    }

    Span span(std::string_view fragment, std::string_view name) const {
        const auto start = source.find(fragment);
        CHECK(start != std::string::npos);
        const auto offset = fragment.find(name);
        CHECK(offset != std::string::npos);
        return {file->file, static_cast<u32>(start + offset), static_cast<u32>(start + offset + name.size())};
    }

    const NameReference& ref(std::string_view fragment, std::string_view name) const {
        const auto* r = result.reference(span(fragment, name));
        CHECK(r);
        return *r;
    }

    const ResolvedBinding& binding(std::string_view fragment, std::string_view name) const {
        const auto& r = ref(fragment, name);
        CHECK(r.state == ResolutionState::Resolved);
        const auto candidates = result.candidates(r.target);
        CHECK(candidates.size() == 1);
        return result.binding(candidates.front());
    }

    void bound_to(std::string_view fragment, std::string_view name, std::string_view declaration) const {
        CHECK(binding(fragment, name).name.span.lo == span(declaration, name).lo);
    }
};

void forward_declarations_and_overloads() {
    Frontend f;
    f.check(R"(
fun start(x: Later) = next(x)
let constructed = Later()
fun next(x: Later) = start(x)
extern "C" { fun next(x: Int): Int }
class Later { class Nested {} }
type Alias = Later.Nested
let callback = next
let aliasMember = Alias.member
fun shadowOverload() { let next = 1; next }
)");
    f.bound_to("x: Later", "Later", "class Later");
    f.bound_to("Later()", "Later", "class Later");
    f.bound_to("Later.Nested", "Nested", "class Nested");
    for (const auto fragment : {"next(x)", "= next\n"}) {
        const auto& r = f.ref(fragment, "next");
        const auto candidates = f.result.candidates(r.target);
        CHECK(candidates.size() == 2);
        CHECK(f.result.binding(candidates[0]).item->kind == ast::ItemKind::Fun);
        CHECK(f.result.binding(candidates[1]).name.span.lo > f.result.binding(candidates[0]).name.span.lo);
    }
    CHECK(f.binding("x: Int", "Int").kind == BindingKind::BuiltinType);
    CHECK(f.ref("Alias.member", "member").state == ResolutionState::Deferred);
    CHECK(f.binding("; next }", "next").kind == BindingKind::Local);
}

void lexical_visibility() {
    Frontend f;
    f.check(R"(
let x = 10
fun f(x: Int, y: Int = x) {
    let x = x + y
    if true { let x = x + 1; x }
    x
    defer { x }
}
fun before() { later; let later = 1 }
fun ownInitializer() { let alone = alone }
fun defaults(a: Int = b, b: Int = 1) = a
fun cleanupScope() { defer { unseen }; let unseen = 1 }
)", {DiagCode::UnknownValue, DiagCode::UnknownValue, DiagCode::UnknownValue, DiagCode::UnknownValue});
    f.bound_to("= x)", "x", "f(x: Int");
    f.bound_to("= x + y", "x", "f(x: Int");
    f.bound_to("= x + 1", "x", "let x = x + y");
    f.bound_to("; x }", "x", "let x = x + 1");
    f.bound_to("defer { x }", "x", "let x = x + y");
    CHECK(f.ref("later;", "later").state == ResolutionState::Unresolved);
    CHECK(f.binding("+ y", "y").kind == BindingKind::Parameter);
}

void generic_and_type_positions() {
    Frontend f;
    f.check(R"(
interface Trait { type Item }
class Box<T: Trait>(let value: T) where T: Trait {
    type Inner<U> = fun(U) -> T throws(Error)
    fun get<U: Trait>(arg: U): [T?] where U: Trait = value
}
type Optional<T> = shared T?
fun castValue<T: Trait>(x: T) = x as T.Item
fun instantiate() = Box<Missing>()
type Bad = *Unknown
fun missing(a: [Lost]): Absent throws(Failure) = a
)", {DiagCode::UnknownType, DiagCode::UnknownType, DiagCode::UnknownType,
      DiagCode::UnknownType, DiagCode::UnknownType});
    f.bound_to("value: T", "T", "Box<T:");
    f.bound_to("arg: U", "U", "get<U:");
    f.bound_to("-> T throws", "T", "Box<T:");
    f.bound_to("fun(U)", "U", "Inner<U>");
    CHECK(f.ref("T.Item", "Item").state == ResolutionState::Deferred);
    CHECK(f.binding("where U: Trait", "Trait").kind == BindingKind::Type);
}

void constructors_and_members() {
    Frontend f;
    f.check(R"(
class Parent(x: Int) {}
class Child(raw: Int, let value: Int = raw): Parent(raw) {
    let stored = raw
    init { raw; value; self.stored; super(1) }
    init(other: Int) { other; self.stored }
    fun read(): Self = value
    fun inheritedOrMissing() = raw
    static fun invalidReceiver() = self
    class Inner {}
}
class Plain(raw: Int) { fun invalid() = raw }
struct Point(x: Int) { fun read() = x }
let nested: Child.Inner = Child.Inner()
let missing: Child.Missing = none
let bad = Point.missing
fun outside() = Self
)", {DiagCode::UnknownValue, DiagCode::InvalidReceiver, DiagCode::UnknownValue, DiagCode::InvalidReceiver});
    f.bound_to("= raw\n", "raw", "Child(raw:");
    f.bound_to("Parent(raw)", "raw", "Child(raw:");
    f.bound_to("init { raw", "raw", "Child(raw:");
    CHECK(f.ref("= raw):", "raw").target == f.ref("init { raw", "raw").target);
    CHECK(f.binding("= value\n", "value").kind == BindingKind::Field);
    CHECK(f.binding("read() = x", "x").kind == BindingKind::Field);
    CHECK(f.ref("self.stored", "stored").state == ResolutionState::Deferred);
    CHECK(f.ref("Child.Missing", "Missing").state == ResolutionState::Deferred); // inherited type may exist
    CHECK(f.ref("inheritedOrMissing() = raw", "raw").state == ResolutionState::Deferred);
    CHECK(f.binding(": Self", "Self").name.sym == f.names.intern("Child"));
}

void inherited_lookup_is_deferred() {
    Frontend f;
    f.check(R"(
let inherited = 1
type Element = Int
class Base { let inherited = 2; type Element = String }
class Derived<T>: Base {
    fun read(x: T): Element = inherited
    fun ownMember() = read
    fun local(inherited: T) = inherited
}
)");
    CHECK(f.ref("= inherited\n", "inherited").state == ResolutionState::Deferred);
    CHECK(f.ref(": Element", "Element").state == ResolutionState::Deferred);
    f.bound_to("read(x: T)", "T", "Derived<T>");
    f.bound_to("= read", "read", "fun read");
    f.bound_to("= inherited\n}", "inherited", "local(inherited: T)");
}

void conditional_and_pattern_scopes() {
    Frontend f;
    f.check(R"(
enum E { A(Int), B(Int) }
fun f(value: E, xs: [Int]) {
    if let x = value { x } else { x }
    while let y = value { y; break }
    y
    for z in xs { z; continue }
    z
    match value {
        E.A(a) | E.B(a) if a > 0 -> a
        is E as typed -> typed
        .A(b) -> b
        .B(c) -> a
    }
    a
}
)", {DiagCode::UnknownValue, DiagCode::UnknownValue, DiagCode::UnknownValue,
      DiagCode::UnknownValue, DiagCode::UnknownValue});
    f.bound_to("{ x }", "x", "let x =");
    f.bound_to("{ y;", "y", "let y =");
    f.bound_to("{ z;", "z", "for z in");
    f.bound_to("if a >", "a", "E.A(a)");
    f.bound_to("-> a\n", "a", "E.A(a)");
    f.bound_to("-> typed", "typed", "as typed");
    CHECK(f.binding("E.B(a)", "B").kind == BindingKind::EnumCase);
    CHECK(f.ref(".A(b)", "A").state == ResolutionState::Deferred);
}

void namespace_and_conflict_rules() {
    Frontend f;
    f.check(R"(
class Same {}
class Int {}
let builtinShadow: Int = Int()
let Same = 1
let value: Same = Same
fun clash() {}
let clash = 1
let another = 0
extern "C" { fun another() }
fun use() = clash
fun onlyValue() {}
type Wrong = onlyValue
)", {DiagCode::ConflictingDeclaration, DiagCode::ConflictingDeclaration, DiagCode::UnknownType});
    CHECK(f.binding(": Same", "Same").kind == BindingKind::Type);
    CHECK(f.binding("= Same\n", "Same").kind == BindingKind::Variable);
    CHECK(f.binding(": Int =", "Int").kind == BindingKind::Type);
    for (const auto& d : f.diag.all()) if (d.code == DiagCode::ConflictingDeclaration) {
        CHECK(d.notes.size() == 1);
        CHECK(d.notes.front().span.lo < d.span.lo);
    }
}

void annotation_coverage_and_non_leaking_scopes() {
    Frontend f;
    f.check(R"(
interface I { type Element; fun get(x: Element): Self }
class Outer<T> {
    fun nested<T>(x: T) = x
    fun other(x: T) = x
}
class Broken<T: MissingBound>(x: MissingParam = missingDefault): MissingBase(missingArg) where T: MissingWhere {}
type Alias<T: MissingAliasBound> = fun(MissingInput) -> MissingResult throws(MissingError)
fun f<T>(x: T) {
    if let item: MissingCondition = item { item }
    while let value = value { value; break }
    for element in element { element }
    match x { is MissingPattern as p -> p }
    x as MissingCast
}
let escaped: T = none
)", {DiagCode::UnknownType, DiagCode::UnknownType, DiagCode::UnknownValue,
      DiagCode::UnknownType, DiagCode::UnknownType,
      DiagCode::UnknownType, DiagCode::UnknownType, DiagCode::UnknownType, DiagCode::UnknownType,
      DiagCode::UnknownType, DiagCode::UnknownValue, DiagCode::UnknownValue, DiagCode::UnknownValue,
      DiagCode::UnknownType, DiagCode::UnknownType, DiagCode::UnknownType});
    f.bound_to("get(x: Element)", "Element", "type Element");
    CHECK(f.ref("MissingBase(missingArg)", "missingArg").state == ResolutionState::Deferred);
    // The generic declaration itself is not a use; inspect the parameter type.
    const auto nested_type = f.source.find("x: T) = x");
    const auto* r = f.result.reference({f.file->file, static_cast<u32>(nested_type + 3), static_cast<u32>(nested_type + 4)});
    CHECK(r && f.result.binding(f.result.candidates(r->target).front()).name.span.lo == f.span("nested<T>", "T").lo);
    f.bound_to("other(x: T)", "T", "Outer<T>");
}

void deferred_members_and_imports() {
    Frontend f;
    f.check(R"(
import geometry.shapes as s
@flag(key = setting)
fun use(x: s.Shape) = x.area + x?.field + .Default
let setting = 1
let external = s.Shape()
let intrinsic = Int8.checked(1)
)", {DiagCode::ImportUnavailable});
    for (const auto name : {"area", "field", "Default", "checked"})
        CHECK(f.ref(name, name).state == ResolutionState::Deferred);
    CHECK(f.ref("s.Shape)", "Shape").state == ResolutionState::Deferred);
    f.bound_to("key = setting", "setting", "let setting");
    CHECK(!f.result.reference(f.span("@flag", "flag")));
    CHECK(!f.result.reference(f.span("key =", "key")));
}

void isolation_and_long_trees() {
    Frontend f;
    std::string text = "enum E { Case(Int) }\nlet x = 1\nlet sum = ";
    for (int i = 0; i < 20000; ++i) text += i ? "+x" : "x";
    text += "\nlet chain = x";
    for (int i = 0; i < 20000; ++i) text += ".member";
    text += "\ntype Wrapped = Int";
    for (int i = 0; i < 10000; ++i) text += " ?";
    text += "\nfun f(e: E) = match e { ";
    for (int i = 0; i < 2000; ++i) text += i ? " | .Case(a)" : ".Case(a)";
    text += " -> a }\n";
    f.check(std::move(text), {}, false);
    CHECK(f.result.references().size() > 42000);
    const auto original = f.result.references().size();
    const auto second_id = f.sources.add("other.phs", "let other = x\n");
    CHECK(second_id);
    const auto* second = f.session.parse(*second_id);
    const auto result = resolve_file(*second, f.names, f.diag);
    CHECK(f.diag.error_count() == 1);
    CHECK(f.diag.all().front().code == DiagCode::UnknownValue);
    CHECK(f.result.references().size() == original);
    CHECK(!result.reference(f.result.references().front().span));
    const auto repeated = resolve_file(*f.file, f.names, f.diag);
    CHECK(repeated.references().size() == original);
    CHECK(f.diag.error_count() == 1);
}
}  // namespace

int main() {
    forward_declarations_and_overloads();
    lexical_visibility();
    generic_and_type_positions();
    constructors_and_members();
    inherited_lookup_is_deferred();
    conditional_and_pattern_scopes();
    namespace_and_conflict_rules();
    annotation_coverage_and_non_leaking_scopes();
    deferred_members_and_imports();
    isolation_and_long_trees();
}
