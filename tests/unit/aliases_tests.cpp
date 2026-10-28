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
#include "syntax/ast_dump.hpp"

using namespace phs;
using Type = ScalarType;

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};
    void check(std::string_view source, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        const auto id = session.sources().add("aliases.phs", source);
        CHECK(id);
        const auto* syntax = session.parse(*id);
        if (session.diagnostics().has_errors(*id)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        result = session.check(*id);
        CHECK(result.scalar_types && result.resolution);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual); std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        const auto count = session.diagnostics().error_count();
        CHECK(session.check(*id).scalar_types == result.scalar_types && session.diagnostics().error_count() == count);
    }
    BindingId id(std::string_view name, BindingKind kind = BindingKind::TypeAlias) const {
        for (usize i = 0; i < result.resolution->bindings().size(); ++i) {
            const auto& b = result.resolution->bindings()[i];
            if (b.kind == kind && session.names().str(b.name.sym) == name) return BindingId{i};
        }
        CHECK(false); return {};
    }
    const ExpandedType& alias(std::string_view name) const {
        const auto* fact = result.scalar_types->alias(id(name)); CHECK(fact); return *fact;
    }
    Type value(std::string_view name) const { return result.scalar_types->binding(id(name, BindingKind::Variable)); }
    const ast::CallExpr* call(std::string_view name) const {
        return ast::cast<ast::CallExpr>(ast::cast<ast::VarItem>(result.resolution->binding(id(name, BindingKind::Variable)).item)->init);
    }
};

void chains_optional_identity_and_conversions() {
    Frontend f;
    f.check(R"(
type First = Second
type Second = Int8
type Maybe = First?
type Deep = Maybe?
let scalar: First = 1
let optional: Deep = scalar
fun accept(value: First): Maybe = value
let result = accept(1)
let converted = First.truncating(500)
let checked = Second.checked(500)
fun capture<Int8>(value: First): First = value
type Void = ()
fun voidReturn(): Void? { return }
)");
    CHECK(f.alias("First").state == ExpansionState::Resolved && f.alias("First").scalar == Type::Int8);
    CHECK(f.alias("First").target == f.id("Int8", BindingKind::BuiltinType));
    CHECK(f.alias("Second").target == f.alias("First").target);
    CHECK(f.alias("Deep").optional_depth == 2 && f.alias("Deep").scalar == Type{Type::Int8}.optional().optional());
    CHECK(f.value("scalar") == Type::Int8 && f.value("optional") == f.alias("Deep").scalar);
    CHECK(f.value("result") == Type{Type::Int8}.optional() && f.value("converted") == Type::Int8);
    CHECK(f.value("checked") == Type{Type::Int8}.optional());
    const auto* call = f.result.scalar_types->call(f.call("converted"));
    CHECK(call && call->kind == CallKind::NumericConversion && call->target == f.alias("First").target);
    const auto* capture = f.result.scalar_types->signature(f.id("capture", BindingKind::Function));
    CHECK(capture && capture->parameters[0] == Type::Int8 && capture->result == Type::Int8);
}

void qualified_types_and_nominal_identity() {
    Frontend f;
    f.check(R"(
class Holder { type Number = Int16; class Nested {}; type NestedAlias = Nested; type This = Self }
type Box = Holder
type NumberAlias = Box.Number
type Nested = Box.NestedAlias
type OptionalNominal = Nested?
let value: NumberAlias = 1
let converted = Box.Number.truncating(60000)
let constructor = Box.Number(1)
)");
    CHECK(f.alias("Box").target == f.id("Holder", BindingKind::Type));
    CHECK(f.alias("Nested").target == f.id("Nested", BindingKind::Type));
    CHECK(f.alias("OptionalNominal").target == f.alias("Nested").target && f.alias("OptionalNominal").optional_depth == 1);
    CHECK(f.alias("Nested").state == ExpansionState::Resolved && f.alias("Nested").scalar.kind == Type::Nominal);
    CHECK(f.alias("This").target == f.id("Holder", BindingKind::Type));
    CHECK(f.value("value") == Type::Int16 && f.value("converted") == Type::Int16 && f.value("constructor") == Type::Int16);
    const auto* alias = ast::cast<ast::TypeAliasItem>(f.result.resolution->binding(f.id("NumberAlias")).item);
    const auto* ref = f.result.resolution->reference(alias->type->path.parts.back().span);
    CHECK(ref && ref->state == ResolutionState::Deferred); // expansion never rewrites lexical results
}

void cycles_and_phase_gating() {
    for (std::string_view source : {
        "type Loop = Loop", "type A = B?\ntype B = A", "type A = [B]\ntype B = *A",
        "type A = fun(B) -> Int\ntype B = A", "class Box<T> {}\ntype A = Box<A>",
        "class C { type Back = Outer }\ntype Prefix = C\ntype Outer = Prefix.Back"
    }) {
        Frontend f;
        f.check(source, {DiagCode::AliasCycle});
        CHECK(!f.session.diagnostics().all().front().notes.empty());
    }
    Frontend f;
    f.check("type A = B\ntype B = A\ntype Dependent = A?\nlet other: Bool = 1", {DiagCode::AliasCycle});
    CHECK(f.alias("A").state == ExpansionState::Error && f.alias("B").state == ExpansionState::Error);
    CHECK(f.alias("Dependent").state == ExpansionState::Error && f.value("other") == Type::Deferred);
    const auto healthy = f.session.sources().add("healthy", "type Healthy = Int8\nlet value: Healthy = 1");
    CHECK(healthy && f.session.check(*healthy).scalar_types && !f.session.diagnostics().has_errors(*healthy));
    Frontend separate;
    separate.check("type A = A\ntype B = B", {DiagCode::AliasCycle, DiagCode::AliasCycle});
}

void mismatches_and_member_diagnostics() {
    for (std::string_view source : {
        "type A = Int8\nlet bad: A = true",
        "type A = Int8\nfun f(x: A): A = x\nlet bad = f(true)",
        "type A = Int8\nlet bad = A(128)",
        "type A = Int\nfun f(x: A) {}\nfun f(x: Int) {}\nlet bad = f(1)",
        "class C {}\ntype A = C\nfun f(x: A.Missing) {}",
        "class C {}\ntype A = C\nclass D(let value: A.Missing) {}",
        "class C {}\ntype A = C\ntype Bad = A.Missing"
    }) {
        const auto code = source.find("Missing") != std::string_view::npos ? DiagCode::UnknownType :
                          source.find("A(128)") != std::string_view::npos ? DiagCode::ScalarLiteralRange :
                          source.find("f(1)") != std::string_view::npos ? DiagCode::AmbiguousCall :
                          source.find("f(true)") != std::string_view::npos ? DiagCode::NoMatchingCall : DiagCode::ScalarTypeMismatch;
        Frontend f; f.check(source, {code});
    }
}

void qualified_dependencies_inside_composites() {
    for (std::string_view target : {
        "[Prefix.Back]", "*Prefix.Back", "shared Prefix.Back", "weak Prefix.Back",
        "fun(Prefix.Back) -> Int", "fun() -> Prefix.Back", "fun() -> Int throws(Prefix.Back)",
        "Box<Prefix.Back>", "fun([*Prefix.Back?]) -> Int"
    }) {
        for (bool root_first : {false, true}) {
            const auto root = std::format("type Outer = {}\n", target);
            const std::string definitions = "class Box<T> {}\nclass C { type Back = Outer }\ntype Prefix = C\n";
            Frontend cycle;
            cycle.check(root_first ? root + definitions : definitions + root, {DiagCode::AliasCycle});
            CHECK(cycle.alias("Outer").state == ExpansionState::Error && cycle.alias("Back").state == ExpansionState::Error);
            CHECK(cycle.alias("Prefix").state == ExpansionState::Resolved);
        }
        Frontend acyclic;
        acyclic.check(std::format("type Outer = {}\nclass Box<T> {{}}\nclass C {{ type Back = Int }}\ntype Prefix = C", target));
        CHECK(acyclic.alias("Outer").state == ExpansionState::Resolved);
        CHECK(acyclic.alias("Back").state == ExpansionState::Resolved);
    }
    Frontend missing;
    missing.check("class C {}\ntype Prefix = C\ntype Outer = [Prefix.Missing]", {DiagCode::UnknownType});
    CHECK(missing.alias("Outer").state == ExpansionState::Error);
}

void many_qualified_dependencies() {
    Frontend f;
    std::string source = "type Outer = fun(";
    for (int i = 0; i < 5000; ++i) {
        if (i) source += ", ";
        source += std::format("Prefix.T{}", i);
    }
    source += ") -> Unit\ntype Prefix = C\nclass C {\ntype T0 = Outer\n";
    for (int i = 1; i < 5000; ++i) source += std::format("type T{} = Int\n", i);
    source += "}";
    f.check(source, {DiagCode::AliasCycle}, false);
    CHECK(f.alias("Outer").state == ExpansionState::Error && f.alias("T0").state == ExpansionState::Error);
    CHECK(f.alias("T1").scalar == Type::Int && f.alias("T4999").scalar == Type::Int);
}

void deferred_forms_and_shadowing() {
    Frontend f;
    f.check(R"(
type Generic<T> = T
type Recursive<T> = Recursive<T>
type Applied = Generic<Int>
type Array = [Int]
interface I { type Abstract; type Default = Int; type RecursiveDefault = RecursiveDefault }
type Associated = I.Default
open class Base {}
class Child: Base {}
type Inherited = Child
type UnknownMember = Inherited.Unknown
struct Int8 {}
type Shadow = Int8
let nominal: Shadow = Shadow()
)", {DiagCode::UnknownType});
    for (std::string_view name : {"Generic", "Recursive", "Abstract", "Default", "RecursiveDefault", "Associated"})
        CHECK(f.alias(name).state == ExpansionState::Deferred);
    CHECK(f.alias("Applied").scalar == Type::Int);
    CHECK(f.alias("UnknownMember").state == ExpansionState::Error);
    CHECK(f.alias("Array").scalar.kind == Type::Array);
    CHECK(f.alias("Shadow").state == ExpansionState::Resolved && f.alias("Shadow").scalar.kind == Type::Nominal);
    CHECK(f.alias("Shadow").target == f.id("Int8", BindingKind::Type));
    Frontend value_shadow;
    value_shadow.check("type Small = Int8\nlet Small = true\nlet bad = Small(1)", {DiagCode::NotCallable});
    Frontend renamed_builtin;
    renamed_builtin.check("type Int8 = UInt16\nlet value = Int8(60000)");
    CHECK(renamed_builtin.value("value") == Type::UInt16);
    CHECK(renamed_builtin.alias("Int8").target == renamed_builtin.id("UInt16", BindingKind::BuiltinType));
    Frontend member_namespace;
    member_namespace.check(R"(
class C { type Number = Int8; static fun Number(value: Bool): Bool = value }
type Container = C
type NumberType = Container.Number
let value = Container.Number(true)
)");
    CHECK(member_namespace.alias("NumberType").scalar == Type::Int8 && member_namespace.value("value") == Type::Bool);
    CHECK(!member_namespace.result.scalar_types->conversion(member_namespace.call("value")));
}

void cached_results_and_long_chains() {
    Frontend f;
    f.check("type A = Int\nlet value: A = 1");
    const auto* saved = &f.alias("A");
    for (int i = 0; i < 128; ++i) {
        const auto id = f.session.sources().add("another", "type A = Bool\nlet value: A = true");
        CHECK(id && f.session.check(*id).scalar_types && !f.session.diagnostics().has_errors(*id));
    }
    CHECK(&f.alias("A") == saved && saved->scalar == Type::Int);
    const auto unrelated = f.session.sources().add("isolated", "let value: A = 1");
    CHECK(unrelated && !f.session.check(*unrelated).scalar_types);

    Frontend chain;
    std::string source;
    for (int i = 0; i < 10000; ++i) source += std::format("type A{} = A{}?\n", i, i + 1);
    source += "type A10000 = Int8\nlet value: A0 = 1";
    chain.check(source, {}, false);
    CHECK(chain.alias("A0").scalar == Type{Type::Int8, 10000});
    CHECK(chain.value("value") == chain.alias("A0").scalar);
}
}  // namespace

int main() {
    chains_optional_identity_and_conversions();
    qualified_types_and_nominal_identity();
    cycles_and_phase_gating();
    mismatches_and_member_diagnostics();
    qualified_dependencies_inside_composites();
    many_qualified_dependencies();
    deferred_forms_and_shadowing();
    cached_results_and_long_chains();
}
