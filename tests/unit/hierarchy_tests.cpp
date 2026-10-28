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
#include <array>
#include <initializer_list>
#include <string>

#include "frontend/session.hpp"
#include "syntax/ast_dump.hpp"

using namespace phs;
using namespace phs::ast;
using Type = ScalarType;

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};
    void check(std::string_view source, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        const auto file = session.sources().add("hierarchy.phs", source); CHECK(file);
        const auto* syntax = session.parse(*file);
        if (session.diagnostics().has_errors(*file)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*file));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        result = session.check(*file); CHECK(result.scalar_types);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual); std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        const auto errors = session.diagnostics().error_count();
        CHECK(session.check(*file).scalar_types == result.scalar_types && session.diagnostics().error_count() == errors);
    }
    BindingId id(std::string_view name, BindingKind kind = BindingKind::Variable, usize ordinal = 0) const {
        for (usize i = 0; i < result.resolution->bindings().size(); ++i) {
            const auto& b = result.resolution->bindings()[i];
            if (b.kind == kind && session.names().str(b.name.sym) == name && ordinal-- == 0) return BindingId{i};
        }
        CHECK(false); return {};
    }
    BindingId method(std::string_view owner, std::string_view name) const {
        const auto scope = result.resolution->binding(id(owner, BindingKind::Type)).members;
        for (const auto binding : result.resolution->scopes()[scope.value].bindings)
            if (session.names().str(result.resolution->binding(binding).name.sym) == name) return binding;
        CHECK(false); return {};
    }
    Type value(std::string_view name) const { return result.scalar_types->binding(id(name)); }
    const Expr* init(std::string_view name) const { return cast<VarItem>(result.resolution->binding(id(name)).item)->init; }
    const CheckedCall& call(std::string_view name) const {
        const auto* result_call = result.scalar_types->call(cast<CallExpr>(init(name))); CHECK(result_call); return *result_call;
    }
    const CheckedHierarchy& hierarchy(std::string_view name) const {
        const auto* result_hierarchy = result.scalar_types->hierarchy(id(name, BindingKind::Type)); CHECK(result_hierarchy); return *result_hierarchy;
    }
};

void inherited_members_and_assignability() {
    Frontend f;
    f.check(R"(
let value: Bool = true
let global: Int = 7
interface Readable { fun read(): Int }
open class Base: Readable {
    let value: Int = 1
    static let flag: Bool = true
    open fun read(): Int = value
    fun pick(v: Int): Int = v
}
class Derived: Base, Readable {
    override fun read(): Int = super.read()
    fun pick(v: Bool): Bool = v
    fun implicit() = pick(true)
    fun inherited() = value
    fun outer() = global
}
type Alias = Derived
let object = Alias()
let base: Base = object
let lifted: Base? = object
let protocol: Readable = object
let field = object.value
let staticField = Alias.flag
let called = object.read()
let inheritedCall = object.pick(1)
let ownCall = object.pick(true)
let implicit = object.implicit()
let shadowed = object.inherited()
let outer = object.outer()
fun consume(v: Base): Int = v.read()
let argument = consume(object)
fun choose(v: Base): Bool = false
fun choose(v: Derived): Bool = true
let exact = choose(object)
fun specific(v: Base): Int = 1
fun specific(v: Readable): Bool = true
let closest = specific(object)
)");
    for (std::string_view name : {"field", "called", "inheritedCall", "shadowed", "outer", "argument"}) CHECK(f.value(name) == Type::Int);
    CHECK(f.value("staticField") == Type::Bool && f.value("ownCall") == Type::Bool && f.value("implicit") == Type::Bool);
    CHECK(f.value("lifted").optional_depth == 1 && f.value("lifted").scalar() == f.value("base"));
    CHECK(f.call("exact").target == f.id("choose", BindingKind::Function, 1));
    CHECK(f.value("closest") == Type::Int && f.call("closest").target == f.id("specific", BindingKind::Function));
    const auto& hierarchy = f.hierarchy("Derived");
    CHECK(hierarchy.state == ExpansionState::Resolved && hierarchy.conformance == ExpansionState::Resolved);
    CHECK(hierarchy.superclass == f.id("Base", BindingKind::Type) && hierarchy.bases.size() == 2);
    CHECK(hierarchy.witnesses.size() == 1 && hierarchy.witnesses[0].state == ExpansionState::Resolved);
    CHECK(hierarchy.witnesses[0].requirement == f.method("Readable", "read") && hierarchy.witnesses[0].implementation == f.method("Derived", "read"));
    const auto* member = cast<MemberExpr>(f.init("field"));
    CHECK(f.result.resolution->reference(member->name.span)->state == ResolutionState::Deferred);
    CHECK(f.result.scalar_types->member(member)->candidates.front() == f.method("Base", "value"));
    const auto* read = cast<FunItem>(f.result.resolution->binding(f.method("Derived", "read")).item);
    const auto* super_call = f.result.scalar_types->call(cast<CallExpr>(read->body));
    CHECK(super_call && super_call->target == f.method("Base", "read") && super_call->receiver->kind == ExprKind::Super);
}

void defaults_and_abstract_requirements() {
    Frontend f;
    f.check(R"(
interface I { fun read(): Int = 1 }
interface J: I {}
interface K: I {}
interface Diamond: J, K {}
class Defaulted: Diamond {}
let result = Defaulted().read()
interface Required { fun fetch(v: Int): Int }
abstract class Abstract: Required { abstract fun fetch(v: Int): Int }
class Concrete: Abstract { override fun fetch(v: Int): Int = v }
let value = Concrete().fetch(1)
open class ReturnBase {}
class ReturnChild: ReturnBase {}
open class Producer { open fun make(): ReturnBase = ReturnBase() }
class Specialized: Producer { override fun make(): ReturnChild = ReturnChild() }
let covariant = Specialized().make()
struct Value: I {}
let structural: I = Value()
interface Left { fun resolved(): Int = 1 }
interface Right { fun resolved(): Int = 2 }
interface Refined: Left, Right { fun resolved(): Int = 3 }
class Multiple: Left, Right, Refined {}
let selectedDefault = Multiple().resolved()
open class Parent(let value: Int8 = 1) {}
class Head: Parent(value: 2) {}
let headField = Head().value
class Delegating: Parent { init { super(1) } }
)");
    CHECK(f.value("result") == Type::Int && f.value("value") == Type::Int);
    CHECK(f.hierarchy("Defaulted").witnesses.size() == 1 && f.hierarchy("Defaulted").conformance == ExpansionState::Resolved);
    CHECK(f.call("result").target == f.method("I", "read"));
    CHECK(f.hierarchy("Abstract").conformance == ExpansionState::Deferred && f.hierarchy("Concrete").conformance == ExpansionState::Resolved);
    CHECK(f.hierarchy("Specialized").conformance == ExpansionState::Resolved && f.value("covariant").kind == Type::Nominal);
    CHECK(f.value("selectedDefault") == Type::Int && f.call("selectedDefault").target == f.method("Refined", "resolved"));
    CHECK(f.value("headField") == Type::Int8);
}

void inherited_types_and_forward_base_aliases() {
    Frontend f;
    f.check(R"(
open class Types { type Number = Int8; class Nested {} }
class Derived: Types { fun take(v: Number): Number = v }
type Small = Derived.Number
let value: Small = Derived().take(1)
let converted = Small(1)
let memberConverted = Derived.Number(1)
let nested: Derived.Nested = Derived.Nested()
class Leaf: Selected {}
type Selected = Names.Parent
class Names: Definitions {}
open class Definitions { type Parent = Root }
open class Root {}
let upcast: Root = Leaf()
)");
    CHECK(f.value("value") == Type::Int8 && f.value("converted") == Type::Int8 && f.value("memberConverted") == Type::Int8);
    CHECK(f.value("nested").kind == Type::Nominal && f.hierarchy("Leaf").superclass == f.id("Root", BindingKind::Type));
    Frontend invalid;
    invalid.check("class Leaf: Selected {}\ntype Selected = Names.Number\nclass Names: Definitions {}\nopen class Definitions { type Number = Int }", {DiagCode::InvalidBase});
    CHECK(invalid.hierarchy("Leaf").state == ExpansionState::Error);
    Frontend cycle;
    cycle.check("open class A { type X = B.X }\nclass B: A {}\nlet value: B.X", {DiagCode::AliasCycle});
}

void invalid_hierarchies_and_contracts() {
    for (const auto& [source, code] : std::vector<std::pair<std::string_view, DiagCode>>{
        {"class Final {}\nclass D: Final {}", DiagCode::InvalidBase},
        {"class D: Int {}", DiagCode::InvalidBase},
        {"struct Base {}\nstruct D: Base {}", DiagCode::InvalidBase},
        {"open class Base {}\ninterface I: Base {}", DiagCode::InvalidBase},
        {"open class A {}\nopen class B {}\nclass D: A, B {}", DiagCode::InvalidBase},
        {"interface I {}\ntype Alias = I\nclass D: I, Alias {}", DiagCode::InvalidBase},
        {"interface I {}\nclass D: I() {}", DiagCode::InvalidBase},
        {"open class B(x: Int) {}\nclass D: B(true) {}", DiagCode::NoMatchingCall},
        {"open class B(x: Int8) {}\nclass D: B(128) {}", DiagCode::ScalarLiteralRange},
        {"open class A: B {}\nopen class B: A {}", DiagCode::InheritanceCycle},
        {"interface A: B {}\ninterface B: A {}", DiagCode::InheritanceCycle},
        {"open class B { fun f(): Int = 1 }\nclass D: B { override fun f(): Int = 1 }", DiagCode::InvalidOverride},
        {"open class B { open fun f(): Int = 1 }\nclass D: B { fun f(): Int = 1 }", DiagCode::InvalidOverride},
        {"class C { override fun missing(): Int = 1 }", DiagCode::InvalidOverride},
        {"open class B { open fun f(): Int = 1 }\nclass D: B { override fun f(): Bool = true }", DiagCode::InvalidOverride},
        {"interface I { fun f(): Int }\nclass C: I {}", DiagCode::MissingImplementation},
        {"interface I { fun f(): Int }\nclass C: I { fun f(): Bool = true }", DiagCode::MissingImplementation},
        {"interface I { fun f(): Int }\nclass C: I { fun f(): Int throws = 1 }", DiagCode::MissingImplementation},
        {"abstract class A { abstract fun f(): Int }\nclass C: A {}", DiagCode::MissingImplementation},
        {"open class B {}\nclass D: B {}\nlet value: D = B()", DiagCode::ScalarTypeMismatch},
        {"interface I {}\nclass C {}\nlet value: I = C()", DiagCode::ScalarTypeMismatch},
        {"open class B {}\nclass D: B {}\nlet values: [D] = [D()]\nlet invalid: [B] = values", DiagCode::ScalarTypeMismatch},
        {"open class B {}\nclass D: B {}\nlet value = D().missing", DiagCode::UnknownMember},
        {"open class B {}\nclass D: B { fun f() = missing }", DiagCode::UnknownValue}
    }) { Frontend f; f.check(source, {code}); }
    Frontend conflict;
    conflict.check("interface I { fun f(): Int = 1 }\ninterface J { fun f(): Int = 2 }\nclass C: I, J {}",
                   {DiagCode::MissingImplementation, DiagCode::MissingImplementation});
    Frontend fields;
    fields.check("interface I { static let value: Int = 1 }\ninterface J { static let value: Int = 2 }\nclass C: I, J {}\nlet invalid = C.value", {DiagCode::AmbiguousMember});
}

void deferred_contracts_and_stable_facts() {
    Frontend f;
    f.check(R"(
open class Generic<T> {}
class Dependent<T>: Generic<T> { override fun f(): Int = 1 }
interface Associated { type Element; fun f(v: Element): Element }
abstract class Pending: Associated {}
interface WithSelf { fun identity(v: Self): Self }
class Identity: WithSelf { fun identity(v: Identity): Identity = Identity() }
let pendingCall = Identity().identity(Identity())
struct Copying: Copy {}
interface Callback { fun use(cb: fun() -> Int throws(Self)): Int }
abstract class PendingCallback: Callback {}
interface Borrowed { fun inspect(v: borrow Int): Int }
class PendingBorrow: Borrowed {}
)");
    CHECK(f.hierarchy("Dependent").state == ExpansionState::Deferred);
    CHECK(f.hierarchy("Pending").conformance == ExpansionState::Deferred && f.hierarchy("Identity").conformance == ExpansionState::Resolved);
    CHECK(f.value("pendingCall").kind == Type::Nominal && f.hierarchy("Copying").state == ExpansionState::Deferred);
    CHECK(f.hierarchy("PendingCallback").conformance == ExpansionState::Deferred && f.hierarchy("PendingBorrow").conformance == ExpansionState::Deferred);
    const auto* fact = f.result.scalar_types->hierarchy(f.id("Identity", BindingKind::Type));
    for (int i = 0; i < 128; ++i) {
        const auto file = f.session.sources().add("extra.phs", "class Extra {}"); CHECK(file);
        CHECK(f.session.check(*file).scalar_types);
    }
    CHECK(f.result.scalar_types->hierarchy(f.id("Identity", BindingKind::Type)) == fact);
}

void optional_owner_barriers() {
    Frontend f;
    f.check(R"(
class C { open class Nested {} }
type Maybe = C?
type Pending = Maybe.Nested
let value: Maybe.Nested = true
class Derived: Maybe.Nested {}
class Holder { type Owner = Maybe }
let nested: Holder.Owner.Nested = false
type Direct = C.Nested
let direct: Direct = C.Nested()
class Known: C.Nested {}
)");
    CHECK(f.value("value") == Type::Deferred && f.value("nested") == Type::Deferred);
    CHECK(f.result.scalar_types->alias(f.id("Pending", BindingKind::TypeAlias))->state == ExpansionState::Deferred);
    CHECK(f.hierarchy("Derived").state == ExpansionState::Deferred && f.hierarchy("Derived").bases.empty());
    CHECK(f.hierarchy("Known").superclass == f.id("Nested", BindingKind::Type) && f.value("direct").kind == Type::Nominal);
}

void base_order_independent_reabstraction() {
    const std::string declarations = R"(
interface A { fun f(): Int = 1 }
interface B { fun f(): Int = 2 }
interface C: A { fun f(): Int }
)";
    std::array<std::string_view, 3> bases{"A", "B", "C"};
    do {
        Frontend f;
        f.check(declarations + std::format("class X: {}, {}, {} {{}}\nlet value = X().f()", bases[0], bases[1], bases[2]));
        CHECK(f.value("value") == Type::Int && f.call("value").target == f.method("B", "f"));
        CHECK(f.hierarchy("X").conformance == ExpansionState::Resolved);
        for (const auto& witness : f.hierarchy("X").witnesses) CHECK(witness.state == ExpansionState::Resolved && witness.implementation == f.method("B", "f"));
    } while (std::next_permutation(bases.begin(), bases.end()));
    for (std::string_view order : {"A, Combined", "Combined, A"}) {
        Frontend f;
        f.check(declarations + "interface Combined: B, C {}\nclass X: " + std::string(order) + " {}\nlet value = X().f()");
        CHECK(f.call("value").target == f.method("B", "f") && f.hierarchy("X").conformance == ExpansionState::Resolved);
    }
}

void inapplicable_generic_member_overloads() {
    for (std::string_view declarations : {
        "class C { fun f(x: Int): Int = x; fun f<T>(x: T, y: T): T = x }\n",
        "open class Base { fun f(x: Int): Int = x; fun f<T>(x: T, y: T): T = x }\nclass C: Base {}\n"}) {
        Frontend f;
        f.check(std::string(declarations) + "let value = C().f(1)");
        CHECK(f.value("value") == Type::Int && f.call("value").state == CallState::Resolved);
        const auto* call = cast<CallExpr>(f.init("value"));
        const auto* members = f.result.scalar_types->member(call->callee); CHECK(members);
        CHECK(members->state == ResolutionState::Resolved && members->candidates.size() == 2);
        Frontend mismatch;
        mismatch.check(std::string(declarations) + "let bad: Bool = C().f(1)", {DiagCode::ScalarTypeMismatch});
        Frontend rejected;
        rejected.check(std::string(declarations) + "let bad = C().f(true)", {DiagCode::NoMatchingCall});
    }
    Frontend pending;
    pending.check("class C { fun f(x: Int): Int = x; fun f<T: Copy>(x: T): T = x }\nlet value = C().f(1)");
    CHECK(pending.value("value") == Type::Deferred && pending.call("value").state == CallState::Deferred);
    Frontend missing;
    missing.check("interface I { fun f(x: Int): Int }\nclass C: I { fun f<T>(x: T, y: T): T = x }", {DiagCode::MissingImplementation});
}

void long_hierarchies_and_diamonds() {
    Frontend f;
    std::string source = "open class N0 { let value: Int = 1; fun read(): Int = value }\n";
    for (int i = 1; i <= 10000; ++i) source += std::format("open class N{}: N{} {{}}\n", i, i - 1);
    source += "let field = N10000().value\nlet called = N10000().read()\nlet base: N0 = N10000()";
    f.check(source, {}, false);
    CHECK(f.value("field") == Type::Int && f.value("called") == Type::Int && f.hierarchy("N10000").state == ExpansionState::Resolved);
    Frontend diamond;
    source = "interface I0 { fun read(): Int = 1 }\n";
    for (int i = 1; i <= 64; ++i) source += std::format("interface L{}: I{} {{}}\ninterface R{}: I{} {{}}\ninterface I{}: L{}, R{} {{}}\n", i, i - 1, i, i - 1, i, i, i);
    source += "class C: I64 {}\nlet value = C().read()";
    diamond.check(source, {}, false);
    CHECK(diamond.value("value") == Type::Int && diamond.hierarchy("C").witnesses.size() == 1);
    // Upcasts across branches of a class tree, declared out of order, and to
    // an interface reached through a superclass.
    const std::string tree = "class A2: A {}\nopen class A: R, I {}\nopen class R {}\nopen class B: R {}\ninterface I {}\n"
                             "let root: R = A2()\nlet parent: A = A2()\nlet viaSuper: I = A2()\nlet same: A2 = A2()\n";
    Frontend branches;
    branches.check(tree, {}, false);
    CHECK(branches.value("root").kind == Type::Nominal && branches.value("viaSuper").kind == Type::Nominal);
    // Interface walks cache failures for every class they pass, so later
    // queries from deeper classes stop early and must still find the answer.
    std::string interfaces = "interface I {}\ninterface J {}\ninterface K: J {}\nopen class L0: I {}\n";
    for (int i = 1; i <= 40; ++i) interfaces += std::format("open class L{}: L{}{} {{}}\n", i, i - 1, i == 20 ? ", K" : "");
    interfaces += "let low: I = L40()\nlet mid: J = L25()\nlet top: I = L0()\n";
    Frontend walks;
    walks.check(interfaces, {}, false);
    for (const auto* invalid : {"let above: J = L19()", "let rootward: K = L3()"}) {
        Frontend f3;
        f3.check(interfaces + "let warm: J = L40()\nlet unrelated: J = L10()\n" + invalid, {DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch});
    }
    for (const auto* invalid : {"let sibling: B = A2()", "let down: A2 = A()", "let unrelated: I = B()"}) {
        Frontend f2;
        f2.check(tree + invalid, {DiagCode::ScalarTypeMismatch});
    }
}
} // namespace

int main() {
    optional_owner_barriers();
    base_order_independent_reabstraction();
    inapplicable_generic_member_overloads();
    inherited_members_and_assignability();
    defaults_and_abstract_requirements();
    inherited_types_and_forward_base_aliases();
    invalid_hierarchies_and_contracts();
    deferred_contracts_and_stable_facts();
    long_hierarchies_and_diamonds();
}
