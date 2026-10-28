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
using Type = ScalarType;

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};
    void check(std::string_view source, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        const auto id = session.sources().add("inference.phs", source); CHECK(id);
        const auto* syntax = session.parse(*id);
        if (session.diagnostics().has_errors(*id)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        result = session.check(*id); CHECK(result.scalar_types);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual); std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        const auto errors = session.diagnostics().error_count();
        CHECK(session.check(*id).scalar_types == result.scalar_types && session.diagnostics().error_count() == errors);
    }
    BindingId id(std::string_view name, BindingKind kind = BindingKind::Variable) const {
        for (usize i = 0; i < result.resolution->bindings().size(); ++i) {
            const auto& b = result.resolution->bindings()[i];
            if (b.kind == kind && session.names().str(b.name.sym) == name) return BindingId{i};
        }
        CHECK(false); return {};
    }
    Type value(std::string_view name) const { return result.scalar_types->binding(id(name)); }
    const ast::CallExpr* invocation(std::string_view name) const {
        return ast::cast<ast::CallExpr>(ast::cast<ast::VarItem>(result.resolution->binding(id(name)).item)->init);
    }
    const CheckedCall& call(std::string_view name) const {
        const auto* record = result.scalar_types->call(invocation(name)); CHECK(record); return *record;
    }
    std::string name(Type type) const { return result.scalar_types->type_name(type, *result.resolution, session.names()); }
};

void inferred_arguments_and_literals() {
    Frontend f;
    f.check(R"(
fun identity<T>(value: T): T = value
fun same<T>(a: T, b: T): T = a
type Values<T> = [T]
fun head<T>(values: Values<T>): T = values[0]
class Box<T>(let value: T) { fun echo<U>(value: U): U = value }
let byte: Int8 = 1
let integer = identity(1)
let boolean = identity(true)
let first = same(1, byte)
let last = same(byte, 1)
let array = identity([1, 2])
let element = head([byte, 2])
let empty = same([], [byte])
let box = Box(byte)
let method = box.echo(true)
let rows = same([[1]], [[byte]])
let lifted: Int8? = 1
fun optional<T>(value: T?): T? = value
let optionalValue = optional(lifted)
let promoted = optional(byte)
let optionalFirst = same(lifted, byte)
let optionalLast = same(byte, lifted)
open class Parent {}
class Child: Parent {}
let parent: Parent = Child()
let joined = same(Child(), parent)
let reversed = same(parent, Child())
fun callback<T>(cb: fun(T) -> T, value: T): T = value
let cb: fun(Int8) -> Int8
let callbackResult = callback(cb, 1)
fun address<T>(value: *T): *T = value
let ptr: *Int8
let pointer = address(ptr)
fun labels<T, U>(first: T, second: U): T = first
let reordered = labels(second: 1, first: true)
)");
    CHECK(f.value("integer") == Type::Int && f.value("boolean") == Type::Bool);
    for (std::string_view name : {"first", "last", "element", "callbackResult"}) CHECK(f.value(name) == Type::Int8);
    CHECK(f.name(f.value("array")) == "[Int]" && f.name(f.value("empty")) == "[Int8]");
    CHECK(f.name(f.value("rows")) == "[[Int8]]" && f.name(f.value("box")) == "Box<Int8>");
    CHECK(f.value("method") == Type::Bool && f.name(f.value("pointer")) == "*Int8");
    CHECK(f.value("optionalValue") == Type{Type::Int8}.optional() && f.value("promoted") == f.value("optionalValue"));
    CHECK(f.value("optionalFirst") == f.value("optionalValue") && f.value("optionalLast") == f.value("optionalValue"));
    CHECK(f.name(f.value("joined")) == "Parent" && f.value("joined") == f.value("reversed"));
    CHECK(f.value("reordered") == Type::Bool && f.call("reordered").parameters == (std::vector<usize>{1, 0}));
    CHECK(f.call("first").type_arguments == std::vector<Type>{Type::Int8} && f.call("first").inferred_type_arguments);
    CHECK(f.call("box").type_arguments == std::vector<Type>{Type::Int8} && f.call("box").inferred_type_arguments);
    const auto* signature = f.result.scalar_types->signature(f.id("identity", BindingKind::Function));
    CHECK(signature && signature->parameters.size() == 1 && signature->parameters.front().kind == Type::TypeParameter);
}

void speculative_constraint_uses() {
    const std::string declarations = "interface I {}\nclass Bad {}\nclass Box<T: I> {}\ntype Wrapped<T> = Box<T>\n";
    for (std::string_view pattern : {"Box<T>", "Wrapped<T>"}) {
        const auto specific = std::format("fun choose<T>(value: {}, extra: Int = 0): Bool = true\n", pattern);
        const std::string general = "fun choose<T>(value: T): Int = 1\n";
        for (bool reverse : {false, true}) {
            const auto overloads = reverse ? general + specific : specific + general;
            Frontend f;
            f.check(declarations + overloads + "let first = choose<Bad>(Bad())\nlet again = choose<Bad>(Bad())");
            CHECK(f.value("first") == Type::Int && f.value("again") == Type::Int);
            CHECK(f.call("first").state == CallState::Resolved && std::ranges::none_of(f.result.scalar_types->constraints(), [](const auto& use) { return use.state == ExpansionState::Error; }));
            // Reusing a previously probed signature must still validate it if
            // a later call selects it. Never needs no invalid Box<Bad> argument.
            Frontend selected;
            selected.check(declarations + overloads +
                "extern \"C\" { fun stop(): Never }\nlet first = choose<Bad>(Bad())\nlet selected = choose<Bad>(stop(), extra: 1)",
                {DiagCode::GenericConstraint});
            CHECK(selected.value("first") == Type::Int);
            const auto contracts = selected.result.scalar_types->constraints();
            CHECK(std::ranges::count_if(contracts, [](const auto& use) { return use.state == ExpansionState::Error; }) == 1);
            const auto& contract = *std::ranges::find_if(contracts, [](const auto& use) { return use.state == ExpansionState::Error; });
            CHECK(contract.declaration == selected.id("Box", BindingKind::Type) && contract.state == ExpansionState::Error);
        }
    }
    Frontend method;
    method.check(declarations + "class Owner { fun choose<T>(value: Box<T>): Bool = true; fun choose<T>(value: T): Int = 1 }\nlet value = Owner().choose<Bad>(Bad())");
    CHECK(method.value("value") == Type::Int && std::ranges::none_of(method.result.scalar_types->constraints(), [](const auto& use) { return use.state == ExpansionState::Error; }));
    Frontend closed;
    closed.check(declarations + "fun choose(value: Box<Bad>): Bool = true\nfun choose(value: Bad): Int = 1\nlet value = choose(Bad())", {DiagCode::GenericConstraint});
    CHECK(closed.result.scalar_types->constraints().size() == 1); // an unconditional declaration error still counts
}

void optional_nominal_anchors() {
    Frontend f;
    f.check(R"(
interface Marker {}
open class Parent: Marker {}
class Child: Parent {}
fun same<T>(a: T, b: T): T = a
let child: Child?
let parent: Parent
let forward = same(child, parent)
let reverse = same(parent, child)
let explicitValue = same<Parent?>(child, parent)
let deeper: Child? ?
let optionalParent: Parent?
let deep = same(deeper, optionalParent)
let deepReverse = same(optionalParent, deeper)
let marker: Marker
let interfaceValue = same(child, marker)
class Generic<T> {}
let genericChild: Generic<Int>?
let genericParent: Generic<Bool>
let pending = same(genericChild, genericParent)
)");
    CHECK(f.name(f.value("forward")) == "Parent?" && f.value("reverse") == f.value("forward"));
    CHECK(f.value("forward") == f.value("explicitValue") && f.call("forward").state == CallState::Resolved);
    CHECK(f.name(f.value("deep")) == "Parent??" && f.value("deepReverse") == f.value("deep"));
    CHECK(f.name(f.value("interfaceValue")) == "Marker?");
    CHECK(f.call("pending").state == CallState::Deferred && f.value("pending") == Type::Deferred);
    Frontend invariant;
    invariant.check("open class Parent {}\nclass Child: Parent {}\nfun f<T>(a: [T], b: [T]): T { loop {} }\nlet children: [Child?]\nlet parents: [Parent]\nlet bad = f(children, parents)", {DiagCode::NoMatchingCall});
}

void nondependent_protocol_positions() {
    for (std::string_view protocol : {"Copy", "Sendable", "Error"}) {
        Frontend f;
        f.check(std::format("fun f<T>(x: T, marker: {}): T = x\nlet n: Int = 1\nlet inferred = f(n, n)\nlet explicitValue = f<Int>(n, n)", protocol));
        CHECK(f.call("inferred").state == CallState::Deferred && f.value("inferred") == Type::Deferred);
        CHECK(f.call("explicitValue").state == CallState::Deferred && f.value("explicitValue") == Type::Deferred);
        Frontend alias;
        alias.check(std::format("type Trait = {}\nfun f<T>(x: T, marker: Trait): T = x\nlet n: Int = 1\nlet inferred = f(n, n)\nlet explicitValue = f<Int>(n, n)", protocol));
        CHECK(alias.call("inferred").state == CallState::Deferred && alias.call("explicitValue").state == CallState::Deferred);
    }
    Frontend nested;
    nested.check("fun f<T>(x: T, markers: [Copy]): T = x\nlet n: Int = 1\nlet inferred = f(n, [n])\nlet explicitValue = f<Int>(n, [n])");
    CHECK(nested.call("inferred").state == CallState::Deferred && nested.call("explicitValue").state == CallState::Deferred);
    Frontend dependent;
    dependent.check("interface I<U> {}\nfun f<T>(x: T, marker: I<T>): T = x\nlet n: Int = 1\nlet inferred = f(n, n)\nlet explicitValue = f<Int>(n, n)");
    CHECK(dependent.call("inferred").state == CallState::Deferred && dependent.call("explicitValue").state == CallState::Deferred);
    for (std::string_view source : {
        "class Marker {}\nfun f<T>(x: T, marker: Marker): T = x\nlet bad = f(1, true)",
        "fun f<T>(x: T, values: [T]): T = x\nlet bad = f(1, true)",
        "type Trait = Copy\nfun f<T>(x: T, values: [Trait]): T = x\nlet values: [Int]\nlet bad = f(1, values)"}) {
        Frontend bad; bad.check(source, {DiagCode::NoMatchingCall});
    }
}

void late_nominal_anchors() {
    const std::string declarations = R"(
interface Marker {}
open class Parent: Marker {}
class A: Parent {}
class B: Parent {}
fun same<T>(a: T, b: T, c: T): T = a
let a: A?
let b: B
let deepB: B? ?
let parent: Parent
let marker: Marker
)";
    for (const auto& [arguments, expected] : std::vector<std::pair<std::array<std::string_view, 3>, std::string_view>>{
        {{"A()", "B()", "Parent()"}, "Parent"}, {{"a", "b", "parent"}, "Parent?"},
        {{"a", "deepB", "parent"}, "Parent??"}, {{"a", "b", "marker"}, "Marker?"}
    }) {
        std::array<int, 3> order{0, 1, 2};
        do {
            Frontend f;
            f.check(declarations + std::format("let result = same({}, {}, {})", arguments[order[0]], arguments[order[1]], arguments[order[2]]));
            CHECK(f.name(f.value("result")) == expected && f.call("result").state == CallState::Resolved);
            CHECK(f.call("result").type_arguments == std::vector<Type>{f.value("result")});
        } while (std::next_permutation(order.begin(), order.end()));
    }
    Frontend unresolved;
    unresolved.check(declarations + "let pending = same(A(), B(), A())");
    CHECK(unresolved.call("pending").state == CallState::Deferred && unresolved.value("pending") == Type::Deferred);
    Frontend positions;
    positions.check(declarations + "fun choose<U, T>(prefix: U, a: T, b: T, c: T): T = a\nclass Holder<T>(a: T, b: T, c: T) {}\nlet result = choose(true, A(), B(), Parent())\nlet constructed = Holder(A(), B(), Parent())");
    CHECK(positions.name(positions.value("result")) == "Parent" && positions.call("result").type_arguments.front() == Type::Bool);
    CHECK(positions.name(positions.value("constructed")) == "Holder<Parent>" && positions.call("constructed").state == CallState::Resolved);
}

void checked_bounds_and_where() {
    const std::string declarations = R"(
interface Read { fun read(): Int }
interface Flag { fun flag(): Bool }
interface Refined: Read {}
class Good: Refined, Flag { fun read() = 1; fun flag(): Bool = true }
fun bounded<T: Read & Flag>(value: T): T = value
fun constrained<T>(value: T): T where T: Read & Flag = value
fun closed(value: own Good): Good where Good: Read = value
class Box<T: Read>(let value: T) where T: Flag { fun take<U: Read>(value: U): U = value }
class Closed where Good: Read { fun read(): Int = 1 }
)", calls = R"(
let inferred = bounded(Good())
let explicitValue = bounded<Good>(Good())
let whereValue = constrained(Good())
let closedValue = closed(Good())
let box = Box(Good())
let explicitBox = Box<Good>(Good())
let method = box.take(Good())
let annotation: Box<Good>
let closedMember = Closed().read()
)";
    for (bool forward : {false, true}) {
        Frontend f;
        f.check(forward ? calls + declarations : declarations + calls);
        for (std::string_view name : {"inferred", "explicitValue", "whereValue", "closedValue", "method"})
            CHECK(f.name(f.value(name)) == "Good" && f.call(name).state == CallState::Resolved);
        CHECK(f.value("box") == f.value("explicitBox") && f.value("annotation") == f.value("box"));
        CHECK(f.value("closedMember") == Type::Int);
        CHECK(!f.result.scalar_types->constraints().empty());
        for (const auto& contract : f.result.scalar_types->constraints()) CHECK(contract.state == ExpansionState::Resolved);
        const auto* retained = f.result.scalar_types->constraints().data();
        for (int i = 0; i < 32; ++i) {
            const auto file = f.session.sources().add("extra.phs", "let x = 1"); CHECK(file);
            CHECK(f.session.check(*file).scalar_types);
        }
        CHECK(f.result.scalar_types->constraints().data() == retained);
    }
    Frontend abstract;
    abstract.check("interface I { fun read(): Int }\nfun f<T: I>(value: T): T = value\nlet view: I\nlet result = f(view)");
    CHECK(abstract.call("result").state == CallState::Resolved);
}

void overloads_and_rejected_constraints() {
    Frontend f;
    f.check(R"(
interface I {}
class Good: I {}
fun choose(value: Int): Bool = true
fun choose<T>(value: T): T = value
let concrete = choose(1)
fun exact(value: I): Bool = true
fun exact<T>(value: T): T = value
let generic = exact(Good())
fun filtered<T: I>(value: T): Bool = true
fun filtered(value: Int): Int = value
let filteredValue = filtered(1)
)");
    CHECK(f.value("concrete") == Type::Bool && !f.call("concrete").inferred_type_arguments);
    CHECK(f.name(f.value("generic")) == "Good" && f.call("generic").inferred_type_arguments);
    CHECK(f.value("filteredValue") == Type::Int);
    for (const auto& [source, code] : std::vector<std::pair<std::string_view, DiagCode>>{
        {"fun same<T>(a: T, b: T): T = a\nlet byte: Int8 = 1\nlet bad = same(byte, true)", DiagCode::NoMatchingCall},
        {"fun same<T>(a: T, b: T): T = a\nlet byte: Int8 = 1\nlet bad = same(128, byte)", DiagCode::ScalarLiteralRange},
        {"fun identity<T>(x: T): T = x\nlet bad: Int8 = identity(1)", DiagCode::ScalarTypeMismatch},
        {"interface I {}\nclass Bad {}\nfun f<T: I>(x: T): T = x\nlet bad = f(Bad())", DiagCode::GenericConstraint},
        {"interface I {}\nclass Bad {}\nfun f<T>(x: T): T where T: I = x\nlet bad = f<Bad>(Bad())", DiagCode::GenericConstraint},
        {"interface I {}\nclass Bad {}\nclass Box<T: I>(let value: T) {}\nlet bad = Box(Bad())", DiagCode::GenericConstraint},
        {"interface I {}\nclass Bad {}\nclass Box<T: I> {}\nlet bad: Box<Bad>", DiagCode::GenericConstraint},
        {"interface I {}\ntype Alias<T: I> = T\nlet bad: Alias<Int>", DiagCode::GenericConstraint},
        {"fun f<T>(x: T): T where T: Int = x\nlet bad = f(1)", DiagCode::GenericConstraint}
    }) { Frontend bad; bad.check(source, {code}); }
    Frontend wrong;
    wrong.check("interface I { fun f(): Int }\nclass Bad: I { fun f(): Bool = true }\nfun use<T: I>(x: T): T = x\nlet value = use(Bad())",
                {DiagCode::MissingImplementation, DiagCode::GenericConstraint});
    Frontend annotation;
    annotation.check("interface I {}\nclass Bad {}\nclass Box<T: I> {}\nlet value: Box<Bad>", {DiagCode::GenericConstraint});
    CHECK(annotation.result.scalar_types->constraints().size() == 1 &&
          annotation.result.scalar_types->constraints().front().state == ExpansionState::Error);
}

void dependent_inference_and_constraints() {
    Frontend f;
    f.check(R"(
fun make<T>(): T { loop {} }
let missing = make()
fun first<T>(values: [T]): T = values[0]
let empty = first([])
fun optional<T>(value: T?): T? = value
let absent = optional(none)
fun identity<T>(value: T): T = value
extern "C" { fun stop(): Never }
let bottom = identity(stop())
fun copying<T: Copy>(value: T): T = value
let builtin = copying(1)
interface Associated { type Item }
abstract class Pending: Associated {}
fun associated<T: Associated>(value: T): T = value
let abstractValue: Pending
let property = associated(abstractValue)
class Wrapper<T>(let value: T) {}
interface Plain<U> {}
fun generic<T: Plain<Int>>(value: T): T = value
let hierarchy = generic(Wrapper(1))
)");
    for (std::string_view name : {"missing", "empty", "absent", "bottom", "builtin", "property", "hierarchy"})
        CHECK(f.value(name) == Type::Deferred && f.call(name).state == CallState::Deferred);
    for (const auto& contract : f.result.scalar_types->constraints()) CHECK(contract.state == ExpansionState::Deferred);
}

void nongeneric_nominal_constraints() {
    Frontend bad;
    bad.check("interface I {}\nclass Bad {}\nclass Closed where Bad: I {}\nlet value: Closed", {DiagCode::GenericConstraint});
    CHECK(bad.result.scalar_types->constraints().size() == 1);
    const auto& failure = bad.result.scalar_types->constraints().front();
    CHECK(failure.declaration == bad.id("Closed", BindingKind::Type) && failure.arguments.empty() && failure.state == ExpansionState::Error);
    Frontend good;
    good.check("interface I {}\nclass Good: I {}\nclass Closed where Good: I {}\nlet value: Closed");
    CHECK(good.result.scalar_types->constraints().size() == 1 && good.result.scalar_types->constraints().front().state == ExpansionState::Resolved);
    Frontend qualified;
    qualified.check("interface I {}\nclass Bad {}\nclass Outer { class Closed where Bad: I {} }\ntype Alias = Outer\nlet value: Alias.Closed", {DiagCode::GenericConstraint});
    CHECK(qualified.result.scalar_types->constraints().size() == 1);
    Frontend wrapped;
    wrapped.check("interface I {}\nclass Bad {}\nclass Closed where Bad: I {}\nlet value: [Closed?]", {DiagCode::GenericConstraint});
    Frontend barrier;
    barrier.check("interface I {}\nclass Bad {}\nclass Outer { class Closed where Bad: I {} }\ntype Maybe = Outer?\nlet value: Maybe.Closed");
    CHECK(barrier.value("value") == Type::Deferred && barrier.result.scalar_types->constraints().empty());
}

void recursive_constraint_expansion() {
    for (std::string_view source : {
        "interface I<U> {}\nclass Box<T> where Box<[T]>: I<T> {}\nlet value: Box<Int>",
        "interface I<U> {}\nclass Box<T> where Box<[T]>: I<T> {}\nlet a: Box<Int>\nlet b: Box<Bool>",
        "interface I<U> {}\nclass Box<T> where Box<[T]>: I<T> {}\nlet value = Box<Int>()",
        "interface I<U> {}\nclass Box<T> where Box<T>: I<T> {}\nlet value: Box<Int>",
        "interface I<U> {}\nclass A<T> where B<[T]>: I<T> {}\nclass B<U> where A<[U]>: I<U> {}\nlet value: A<Int>",
        "interface I<U> {}\nclass A where B: I<Int> {}\nclass B where A: I<Int> {}\nlet value: A"
    }) {
        Frontend f; f.check(source);
        const auto uses = f.result.scalar_types->constraints();
        CHECK(!uses.empty() && uses.size() <= 8);
        for (const auto& use : uses) CHECK(use.state == ExpansionState::Deferred);
    }
    Frontend independent;
    independent.check("interface I {}\nclass Good: I {}\ninterface J<U> {}\nclass Box<T: I> where Box<[T]>: J<T> {}\nlet value: Box<Good>", {DiagCode::GenericConstraint});
    CHECK(std::ranges::any_of(independent.result.scalar_types->constraints(), [](const auto& use) { return use.state == ExpansionState::Error; }));
    Frontend finite;
    finite.check("interface I {}\nclass Bad {}\nclass Inner<T: I> {}\nclass Outer<T> where Inner<T>: I {}\nlet value: Outer<Bad>", {DiagCode::GenericConstraint, DiagCode::GenericConstraint});
    CHECK(finite.result.scalar_types->constraints().size() == 2);
    Frontend erased;
    erased.check("interface I {}\nclass Good: I {}\ntype Erase<T> = Good\nclass Box<T> where Erase<Box<[T]>>: I {}\nlet value: Box<Int>");
    CHECK(erased.result.scalar_types->constraints().size() <= 4 &&
          std::ranges::any_of(erased.result.scalar_types->constraints(), [](const auto& use) { return use.state == ExpansionState::Deferred; }));
}

void shared_patterns_and_deep_bounds() {
    Frontend shared;
    std::string source;
    for (int i = 0; i < 40; ++i) source += std::format("type F{}<T> = fun(F{}<T>) -> F{}<T>\n", i, i + 1, i + 1);
    source += "type F40<T> = T\nfun leaf<T>(value: F0<T>): T { loop {} }\nlet value: F0<Int>\nlet result = leaf(value)";
    shared.check(source, {}, false);
    CHECK(shared.value("result") == Type::Int);
    // Matching the same graph against an expected function type, for a
    // generic function used as a value, visits each pair once too.
    Frontend valued;
    source.erase(source.rfind("let value"));
    source += "let f: fun(F0<Int>) -> Int = leaf";
    valued.check(source, {}, false);
    CHECK(valued.value("f").kind == Type::Function);
    // So does checking that a closure's parameter type leaves no type
    // parameter open.
    Frontend mapped;
    source.erase(source.rfind("let f"));
    source += "fun map<T, U>(x: T, f: fun(T) -> U): U = f(x)\nlet value: F0<Int>\nlet result = map(value, { x -> true })";
    mapped.check(source, {}, false);
    CHECK(mapped.value("result") == Type::Bool);
    Frontend bounds;
    source = "interface I0 { fun read(): Int }\n";
    for (int i = 1; i < 5000; ++i) source += std::format("interface I{}: I{} {{}}\n", i, i - 1);
    source += "class C: I4999 { fun read(): Int = 1 }\nfun accept<T: I0 & I4999>(value: T): T = value\nlet value = accept(C())";
    bounds.check(source, {}, false);
    CHECK(bounds.name(bounds.value("value")) == "C" && bounds.call("value").state == CallState::Resolved);
}
} // namespace

int main() {
    nongeneric_nominal_constraints();
    recursive_constraint_expansion();
    nondependent_protocol_positions();
    late_nominal_anchors();
    speculative_constraint_uses();
    optional_nominal_anchors();
    inferred_arguments_and_literals();
    checked_bounds_and_where();
    overloads_and_rejected_constraints();
    dependent_inference_and_constraints();
    shared_patterns_and_deep_bounds();
}
