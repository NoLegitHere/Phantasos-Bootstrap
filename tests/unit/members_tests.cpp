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
using namespace phs::ast;

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};
    void check(std::string_view source, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        const auto id = session.sources().add("members.phs", source); CHECK(id);
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
    Type value(std::string_view name, BindingKind kind = BindingKind::Variable) const { return result.scalar_types->binding(id(name, kind)); }
    const Expr* init(std::string_view name) const { return cast<VarItem>(result.resolution->binding(id(name)).item)->init; }
    const CheckedCall& call(std::string_view name) const {
        const auto* record = result.scalar_types->call(cast<CallExpr>(init(name))); CHECK(record); return *record;
    }
};

void fields_and_methods() {
    Frontend f;
    f.check(R"(
struct Point(var x: Int8, y: Int8 = 0) {
    let copied = x
    fun read() = self.x
    fun implicit() = read()
    fun shift(amount: Int8 = 1): Int8 = amount
    fun pick(value: Int): Int = value
    fun pick(value: Bool): Bool = value
    static fun make(): Point = Point(1)
}
let point = Point(2)
let field = point.x
let inferred = point.copied
let method = point.read()
let computed = Point(3).read()
let chained = Point.make().x
let implicit = point.implicit()
let picked = point.pick(true)
let defaulted = point.shift()
let labeled = (point.shift)(amount: 3)
fun write(p: inout Point) { p.x = 1 }
)");
    for (std::string_view name : {"field", "inferred", "method", "computed", "chained", "implicit", "defaulted", "labeled"})
        CHECK(f.value(name) == Type::Int8);
    CHECK(f.value("picked") == Type::Bool);
    CHECK(f.call("method").kind == CallKind::Method && f.call("method").receiver);
    CHECK(f.call("defaulted").parameters.empty() && f.call("labeled").parameters == std::vector<usize>{0});
    const auto* member = cast<MemberExpr>(f.init("field"));
    const auto* record = f.result.scalar_types->member(member); CHECK(record);
    CHECK(record->state == ResolutionState::Resolved && record->owner == f.id("Point", BindingKind::Type));
    CHECK(record->candidates == std::vector<BindingId>{f.id("x", BindingKind::Field)});
    CHECK(f.result.resolution->reference(member->name.span)->state == ResolutionState::Deferred);
    const auto* call = cast<CallExpr>(f.init("picked"));
    CHECK(f.result.scalar_types->member(call->callee)->candidates.size() == 2);
    const auto* implicit = cast<FunItem>(f.result.resolution->binding(f.id("implicit", BindingKind::Function)).item);
    const auto* callee = cast<CallExpr>(implicit->body)->callee;
    const auto* name = cast<NameExpr>(callee);
    CHECK(f.result.resolution->reference(name->name.span)->receiver == record->owner);
    CHECK(f.result.scalar_types->member(callee)->candidates == std::vector<BindingId>{f.id("read", BindingKind::Function)});
}

void static_aliases_and_callable_fields() {
    Frontend f;
    f.check(R"(
class Fields {
    let stored: Int8 = 1
    let selected: Int8 = match true {
        true -> stored
        false -> stored
    }
}
let initialized = Fields().selected
class Namespace {
    static let count: Int16 = 3
    static fun make(value: Int8 = 1): Int8 = value
    class Nested(let value: Bool) {}
    type Byte = UInt8
    type Number = Int8
    static fun Number(value: Bool): Bool = value
    static fun recur(): Int8 = Self.make()
}
type Alias = Namespace
let staticField = Alias.count
let staticMethod = (Alias).make()
let nested = Alias.Nested(true).value
let numeric = Alias.Byte(255)
let valueNamespace = Alias.Number(true)
let selfType = Alias.recur()
extern "C" { fun produce(): fun(Int8) -> Bool }
class Holder(let callback: fun(Int8) -> Bool) {}
let indirect = Holder(produce()).callback(1)
class Mixed {
    static fun pick(value: Int): Int = value
    fun pick(value: Bool): Bool = value
}
let typeCall = Mixed.pick(1)
let instanceCall = Mixed().pick(true)
)");
    CHECK(f.value("staticField") == Type::Int16 && f.value("staticMethod") == Type::Int8);
    CHECK(f.value("initialized") == Type::Int8);
    CHECK(f.value("nested") == Type::Bool && f.value("numeric") == Type::UInt8);
    CHECK(f.value("valueNamespace") == Type::Bool && f.value("selfType") == Type::Int8);
    CHECK(f.call("staticMethod").kind == CallKind::Method && !f.call("staticMethod").receiver);
    CHECK(f.value("indirect") == Type::Bool && f.call("indirect").kind == CallKind::IndirectFunction);
    CHECK(f.call("indirect").target == f.id("callback", BindingKind::Field));
    CHECK(f.value("typeCall") == Type::Int && f.value("instanceCall") == Type::Bool);
    const auto* record = f.result.scalar_types->member(f.init("staticField")); CHECK(record);
    for (int i = 0; i < 128; ++i) {
        const auto file = f.session.sources().add("extra.phs", "let x = 1"); CHECK(file);
        CHECK(f.session.check(*file).scalar_types);
    }
    CHECK(f.result.scalar_types->member(f.init("staticField")) == record && record->type_receiver);
}

void errors_and_deferred_receivers() {
    for (const auto& [source, code] : std::vector<std::pair<std::string_view, DiagCode>>{
        {"class C {}\nlet bad = C().missing", DiagCode::UnknownMember},
        {"open class B {}\nclass D: B { fun local(): Int = 1 }\nlet bad = D.local(true)", DiagCode::InvalidMemberReceiver},
        {"class C {}\nlet bad = C().missing.next()", DiagCode::UnknownMember},
        {"class C(x: Int) {}\nlet bad = C(1).x", DiagCode::UnknownMember},
        {"struct C(x: Int) {}\nlet bad = C.x", DiagCode::InvalidMemberReceiver},
        {"class C { static let x: Int = 1 }\nlet bad = C().x", DiagCode::InvalidMemberReceiver},
        {"class C { fun read(): Int = 1 }\nlet bad = C.read()", DiagCode::InvalidMemberReceiver},
        {"class C(let x: Int) { static fun read(): Int = x }", DiagCode::InvalidMemberReceiver},
        {"class C(let x: Int) { fun read(y: Int = x): Int = y }", DiagCode::InvalidMemberReceiver},
        {"class C { fun read(): Int = 1; static fun bad() = read() }", DiagCode::InvalidMemberReceiver},
        {"class C(let x: Int) {}\nfun write(c: C) { c.x = true }", DiagCode::ScalarTypeMismatch},
        {"class C(let x: Int) {}\nlet bad = C(1).x()", DiagCode::NotCallable},
        {"class C { fun read(x: Int): Int = x }\nlet bad = C().read(true)", DiagCode::NoMatchingCall},
        {"class C { fun read(x: Int8): Int8 = x }\nlet bad = C().read(128)", DiagCode::ScalarLiteralRange},
        {"class C { fun f(x: Int): Int = x; fun f(x: Int64): Int64 = x }\nlet x = C().f(Int8(1))", DiagCode::NoMatchingCall},
        {"class C { fun f(x: Int8): Int8 = x; fun f(x: Int64): Int64 = x }\nlet x = C().f(1)", DiagCode::AmbiguousCall},
        {"class C { let v: Int = 1 }\nlet bad = C()?.v", DiagCode::NotOptional},
        {"let bad = none?.v", DiagCode::NotOptional},
        {"class C { let v: Int = 1 }\nfun f(c: C?): Int? = c?.missing", DiagCode::UnknownMember}
    }) { Frontend bad; bad.check(source, {code}); }
    Frontend f;
    f.check(R"(
open class Base { let value: Int = 1 }
class Derived: Base {
    fun local(): Int = 1
    fun half(): Int? = none
    fun nothing() {}
}
let inherited = Derived().value
let hierarchy = Derived().local()
class Box<T>(let value: T) {}
fun generic(box: Box<Int>, error: Error, sharedValue: shared Derived, optional: Derived?) {
    let dependent = box.value
    let builtin = error.message
    let wrapped = sharedValue.value
    let maybe = optional?.value
    let flat = optional?.half()
    let called = optional?.local()
    let unit = optional?.nothing()
}
class C { fun read(): Int = 1 }
let saved = C().read
let pending = saved()
class GenericMethods {
    fun pick<T>(value: T): Int = 1
    fun pick(value: Int): Int = value
}
let genericCall = GenericMethods().pick(1)
)");
    CHECK(f.value("inherited") == Type::Int && f.value("saved") == Type::Deferred && f.value("pending") == Type::Deferred);
    CHECK(f.value("hierarchy") == Type::Int);
    CHECK(f.value("genericCall") == Type::Int);
    CHECK(f.value("dependent", BindingKind::Local) == Type::Int);
    for (std::string_view name : {"builtin", "wrapped"}) CHECK(f.value(name, BindingKind::Local) == Type::Deferred);
    // `?.` reads the member of the value inside the optional (3.6).
    CHECK(f.value("maybe", BindingKind::Local) == Type{Type::Int}.optional());
    CHECK(f.value("flat", BindingKind::Local) == Type{Type::Int}.optional());
    CHECK(f.value("called", BindingKind::Local) == Type{Type::Int}.optional());
    CHECK(f.value("unit", BindingKind::Local) == Type::Unit);
}

void selected_dependencies_and_long_chains() {
    Frontend f;
    f.check(R"(
class C {
    fun start() = self.pick(true)
    fun pick(value: Int) = start()
    fun pick(value: Bool) = 1
    fun cycle() = cycle()
    let a = b
    let b = a
}
let value = C().start()
let cycle = C().cycle()
let fieldCycle = C().a
)");
    CHECK(f.value("value") == Type::Int && f.value("cycle") == Type::Deferred && f.value("fieldCycle") == Type::Deferred);
    Frontend deep;
    std::string source = "class Node(let next: Node, let value: Int) { fun hop(): Node = self.next }\nfun visit(node: Node) { let field = node";
    for (int i = 0; i < 10000; ++i) source += ".next";
    source += ".value\nlet called = node";
    for (int i = 0; i < 10000; ++i) source += ".hop()";
    source += ".value }";
    // `hop` moves a class out of a borrowed field (3.30), which typing ignores.
    deep.check(source, {DiagCode::MoveOutOfBorrow}, false);
    CHECK(deep.value("field", BindingKind::Local) == Type::Int && deep.value("called", BindingKind::Local) == Type::Int);
}
} // namespace

int main() {
    fields_and_methods();
    static_aliases_and_callable_fields();
    errors_and_deferred_receivers();
    selected_dependencies_and_long_chains();
}
