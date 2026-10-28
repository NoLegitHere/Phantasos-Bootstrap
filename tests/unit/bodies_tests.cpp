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
        const auto id = session.sources().add("bodies.phs", source); CHECK(id);
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

const CheckedBody& body(const Frontend& f, std::string_view name, BindingKind kind = BindingKind::Function) {
    const auto* result = f.result.scalar_types->body(f.id(name, kind)); CHECK(result); return *result;
}

void symbolic_bodies_and_returns() {
    Frontend f;
    f.check(R"(
fun identity<T>(x: T) = x
fun forward<U>(x: U) = identity(x)
fun wrap<T>(x: T) = [x]
fun make<T>(): T { loop {} }
fun defaulted<T>(x: T = make<T>()): T = x
let integer = identity(1)
let boolean = forward(true)
let array = wrap(1)
let defaultValue = defaulted<Int>()
)");
    CHECK(f.value("integer") == Type::Int && f.value("boolean") == Type::Bool);
    CHECK(f.name(f.value("array")) == "[Int]" && f.value("defaultValue") == Type::Int);
    for (std::string_view name : {"identity", "forward", "wrap", "defaulted"}) CHECK(body(f, name).state == ExpansionState::Resolved);
    const auto* signature = f.result.scalar_types->signature(f.id("identity", BindingKind::Function));
    CHECK(signature && signature->result.kind == Type::TypeParameter && signature->result == signature->parameters.front());
    const auto before = body(f, "identity");
    for (int i = 0; i < 32; ++i) {
        const auto file = f.session.sources().add("extra.phs", "let x = 1"); CHECK(file); CHECK(f.session.check(*file).scalar_types);
    }
    CHECK(body(f, "identity").state == before.state && f.name(f.value("array")) == "[Int]");
}

void invalid_bodies_without_instantiation() {
    for (const auto& [source, code] : std::vector<std::pair<std::string_view, DiagCode>>{
        {"fun bad<T>(x: T): Int = x", DiagCode::ScalarTypeMismatch},
        {"fun bad<T>(x: T): T = true", DiagCode::ScalarTypeMismatch},
        {"fun bad<T, U>(x: T): U = x", DiagCode::ScalarTypeMismatch},
        {"fun bad<T>(x: T) = x + 1", DiagCode::InvalidScalarOperand},
        {"fun bad<T>(x: T) = x.missing()", DiagCode::UnknownMember},
        {"interface I { type Item; fun get(): Item }\nfun bad<T: I>(x: T) = x.get().missing()", DiagCode::UnknownMember},
        {"interface Read { fun read(x: Int): Int }\nfun bad<T: Read>(x: T): Int = x.read(true)", DiagCode::NoMatchingCall},
        {"fun bad<T>(x: T = true): T = x", DiagCode::ScalarTypeMismatch},
        {"fun bad<T>(x: T) { if x {} }", DiagCode::ScalarTypeMismatch},
        {"interface I {}\nclass Requires<T: I> {}\nfun bad<T>(x: T): Requires<T> { loop {} }", DiagCode::GenericConstraint},
        {"class Wrong {}\nfun bad<T: Wrong>(x: T): T = x", DiagCode::GenericConstraint}
    }) {
        Frontend f; f.check(source, {code}); CHECK(body(f, "bad").state == ExpansionState::Error);
    }
    Frontend once;
    once.check("fun bad<T>(x: T): Int = x\nlet a = bad(1)\nlet b = bad(true)", {DiagCode::ScalarTypeMismatch});
    CHECK(body(once, "bad").state == ExpansionState::Error);
}

void bounds_associated_types_and_generic_conformance() {
    Frontend f;
    f.check(R"(
interface Mark { fun mark(): Int }
interface Source { type Item: Mark; fun get(): Item }
interface Refined: Source { fun echo(x: Item): Item = x }
class Marked: Mark { fun mark() = 1 }
class Box<T: Mark>(let value: T): Refined { type Item = T; fun get() = value }
fun implied<T>(x: Box<T>): Int = x.value.mark()
fun read<T: Source>(x: T): Int = x.get().mark()
fun item<T: Source>(x: T): T.Item = x.get()
fun echo<T: Refined>(x: T): T.Item = x.echo(x.get())
let result = read(Box(Marked()))
let value = item(Box(Marked()))
let impliedValue = implied(Box(Marked()))
let echoed = echo(Box(Marked()))
interface Unbounded { type Item; fun get(): Item }
class Other: Unbounded { type Item = Marked; fun get(): Marked = Marked() }
fun constrained<T: Unbounded>(x: T): Int where T.Item: Mark = x.get().mark()
let whereValue = constrained(Other())
)");
    CHECK(f.value("result") == Type::Int && f.value("whereValue") == Type::Int && f.name(f.value("value")) == "Marked");
    CHECK(f.value("impliedValue") == Type::Int && body(f, "implied").state == ExpansionState::Resolved);
    CHECK(f.name(f.value("echoed")) == "Marked" && body(f, "echo").state == ExpansionState::Resolved);
    for (std::string_view name : {"read", "item", "constrained"}) CHECK(body(f, name).state == ExpansionState::Resolved);
    const auto* schema = f.result.scalar_types->signature(f.id("item", BindingKind::Function));
    CHECK(schema && schema->result.kind == Type::Projection && f.name(schema->result) == "T.Item");
    const auto* hierarchy = f.result.scalar_types->hierarchy(f.id("Box", BindingKind::Type)); CHECK(hierarchy);
    CHECK(hierarchy->state == ExpansionState::Resolved && hierarchy->conformance == ExpansionState::Resolved);
    CHECK(hierarchy->witnesses.size() == 3);
    for (const auto& witness : hierarchy->witnesses) CHECK(witness.state == ExpansionState::Resolved && witness.implementation.valid());
    CHECK(body(f, "Box", BindingKind::Type).state == ExpansionState::Resolved);
}

void self_defaults_and_properties() {
    Frontend f;
    f.check(R"(
interface Clone { fun clone(x: Self): Self = x }
class C: Clone {}
fun clone<T: Clone>(x: T): T = x.clone(x)
let cloned = clone(C())
interface Rewrite { fun rewrite(x: Self): Self }
class D: Rewrite { fun rewrite(x: D): D = D() }
fun rewrite<T: Rewrite>(x: T): T = x.rewrite(x)
let rewritten = rewrite(D())
interface Has { type Item; var value: Item }
class Cell<T>(var value: T): Has { type Item = T }
fun get<T: Has>(x: T) = x.value
let field = get(Cell(1))
interface Factory { static fun make(): Self }
class Product: Factory { static fun make(): Self = Product() }
fun create<T: Factory>(): T = T.make()
let product = create<Product>()
)");
    CHECK(f.name(f.value("cloned")) == "C" && f.name(f.value("rewritten")) == "D" && f.value("field") == Type::Int);
    CHECK(body(f, "clone").state == ExpansionState::Resolved && body(f, "rewrite").state == ExpansionState::Resolved && body(f, "get").state == ExpansionState::Resolved);
    const auto* properties = f.result.scalar_types->hierarchy(f.id("Cell", BindingKind::Type)); CHECK(properties);
    CHECK(properties->conformance == ExpansionState::Resolved && properties->witnesses.size() == 2);
    CHECK(f.name(f.value("product")) == "Product" && body(f, "create").state == ExpansionState::Resolved);
}

void rejected_dependent_conformance() {
    for (std::string_view source : {
        "interface I { type Item }\nclass Bad: I {}",
        "interface I { fun f(x: Self): Self }\nclass Bad: I { fun f(x: Int): Bad = Bad() }",
        "interface I { var value: Int }\nclass Bad: I { let value: Int = 1 }",
        "interface I { let value: Int }\nclass Bad: I { let value: Bool = true }",
        "interface Mark {}\ninterface I { type Item: Mark }\nclass Bad: I { type Item = Int }",
        "interface Mark {}\ninterface I { type Item: Mark }\nclass Bad<T>: I { type Item = T }",
        "interface Base {}\ninterface I where Self: Base {}\nclass Bad: I {}"
    }) { Frontend f; f.check(source, {DiagCode::MissingImplementation}); }
}

void unsupported_contracts_stay_deferred() {
    Frontend f;
    f.check(R"(
fun builtin<T: Error>(x: T): Int = x.message
open class Base<T>(let value: T) {}
class Derived<T>: Base<T> { fun read(): T = self.value }
interface Default { type Item = Int; fun get(): Item }
class Pending: Default { fun get(): Int = 1 }
interface Read { fun read(): Int }
class Wrapper<U> {}
class Owner<T> { fun conditional(x: T): Int where T: Read = x.read(); fun other(x: T): Int = x.read(); fun composite(x: Wrapper<T>): Int where Wrapper<T>: Read = x.read() }
interface Recursive { type Item: Recursive }
class Node: Recursive { type Item = Node }
interface Clone { fun clone(): Self }
open class Parent: Clone { open fun clone(): Parent = Parent() }
class Child: Parent {}
class Namespace<T>(let value: T) { type Value = T; fun qualified(): Namespace.Value = value }
fun unbound<U>(x: U): Namespace.Value { loop {} }
let unboundValue = unbound(1)
type BoundNamespace = Namespace<Int>
fun scoped<U>(x: U): BoundNamespace.Value = 1
let scopedValue = scoped(true)
let qualifiedValue = Namespace(1).qualified()
)");
    CHECK(body(f, "builtin").state == ExpansionState::Deferred && body(f, "read").state == ExpansionState::Deferred);
    const auto* hierarchy = f.result.scalar_types->hierarchy(f.id("Pending", BindingKind::Type)); CHECK(hierarchy);
    CHECK(hierarchy->conformance == ExpansionState::Deferred);
    CHECK(body(f, "conditional").state == ExpansionState::Deferred && body(f, "other").state == ExpansionState::Deferred);
    CHECK(body(f, "composite").state == ExpansionState::Deferred);
    CHECK(f.result.scalar_types->hierarchy(f.id("Node", BindingKind::Type))->conformance == ExpansionState::Deferred);
    CHECK(f.result.scalar_types->hierarchy(f.id("Child", BindingKind::Type))->conformance == ExpansionState::Deferred);
    CHECK(f.value("unboundValue") == Type::Deferred && body(f, "unbound").state == ExpansionState::Deferred);
    CHECK(f.value("scopedValue") == Type::Int && f.value("qualifiedValue") == Type::Int);
}

void receiver_contexts_and_nested_interface_types() {
    Frontend f;
    f.check(R"(
class Box<T>(x: T) { let value = x }
let inferred: Int = Box(1).value
interface I { type Item; fun echo(x: Item): Item }
open class Parent { fun echo(x: Int): Int = x }
class Child: Parent, I { type Item = Int }
let echoed = Child().echo(1)
abstract class Base: I { fun echo(x: Int): Int = x }
class Leaf: Base { type Item = Int }
let leafEchoed = Leaf().echo(1)
interface Outer { class Nested {} }
fun nested<T: Outer>(x: T): T.Nested { loop {} }
interface Premise { type Item; fun read(): Int }
class Requires<T: Premise> {}
fun chained<T>(a: Requires<T>, b: Requires<T.Item>, x: T.Item): Int = x.read()
class Target { type This = Self }
class Holder { fun pass(x: own Target.This): Target.This = x }
let passed: Target = Holder().pass(Target())
interface Has { let value: Int }
fun readHas<T: Has>(x: T): Int = x.value
let forwardRead: Int = readHas(ForwardCell())
class ForwardCell: Has { let value = 1 }
interface InferredReq { let value = 1 }
class InferredCell: InferredReq { let value = 2 }
fun readInferred<T: InferredReq>(x: T): Int = x.value
let inferredReqValue: Int = readInferred(InferredCell())
class ChainBox<T>(let value: T, let next: Self) {}
fun makeChainBox(): ChainBox<Int> { loop {} }
let constructedSelf = ChainBox<Int>(2, makeChainBox())
interface LeafTrait { fun read(): Int }
interface Branch { type Child }
interface RootType { type Item: Branch }
class LeafBox<U: LeafTrait> {}
fun multiProjectedWhere<T: RootType>(x: T.Item.Child): Int where T.Item.Child: LeafTrait = x.read()
fun multiProjectedParam<T: RootType>(a: LeafBox<T.Item.Child>, x: T.Item.Child): Int = x.read()
class ReversedWhereBox<A> where A.Item: LeafTrait, A: Premise {}
fun reversedIncomingWhere<T>(a: ReversedWhereBox<T>, x: T.Item): Int = x.read()
interface ReAssocBase { type Item: LeafTrait; fun get(): Item }
interface ReAssocRefined: ReAssocBase { type Item }
fun redeclaredAssoc<T: ReAssocRefined>(x: T): T.Item = x.get()
fun redeclaredAssocBound<T: ReAssocRefined>(x: T): Int = x.get().read()
class ProjectionConformer: I { type Item = Int; fun echo(x: Int): Int = x }
fun takeProjected<T: I>(x: T, y: T.Item): T.Item = y
let inferredProjected = takeProjected(ProjectionConformer(), 1)
interface GenericEcho<A> { fun echo(x: A): A }
fun forwardParamEcho<T: GenericEcho<U.Item>, U: Premise>(x: T, y: U.Item): U.Item = x.echo(y)
class CtorOwner<T>(a: LeafBox<T>, let x: T) { fun ctorRead(): Int = x.read() }
class UnrelatedQualified { type Item = Int; fun f(x: I.Item): I.Item = x }
let deferredUnrelated = UnrelatedQualified().f(true)
fun symId<T, U>(x: T): T where T: U = x
fun symForward<T, U>(x: T): T where T: U = symId<T, U>(x)
fun useInterfaceProp(x: Has): Int = readHas(x)
)");
    CHECK(f.value("inferred") == Type::Int && f.value("echoed") == Type::Int && f.value("leafEchoed") == Type::Int);
    CHECK(f.name(f.value("passed")) == "Target" && f.value("forwardRead") == Type::Int && f.value("inferredReqValue") == Type::Int);
    CHECK(f.name(f.value("constructedSelf")) == "ChainBox<Int>" && f.value("inferredProjected") == Type::Int);
    CHECK(f.value("deferredUnrelated") == Type::Deferred);
    for (std::string_view name : {"echoed", "leafEchoed"}) {
        const auto& call = f.call(name); // the class implementation, not the bodiless requirement
        CHECK(call.state == CallState::Resolved && ast::cast<ast::FunItem>(f.result.resolution->binding(call.target).item)->body);
    }
    CHECK(body(f, "nested").state == ExpansionState::Resolved && body(f, "chained").state == ExpansionState::Resolved);
    CHECK(body(f, "multiProjectedWhere").state == ExpansionState::Resolved && body(f, "multiProjectedParam").state == ExpansionState::Resolved);
    CHECK(body(f, "reversedIncomingWhere").state == ExpansionState::Resolved && body(f, "forwardParamEcho").state == ExpansionState::Resolved);
    CHECK(body(f, "ctorRead").state == ExpansionState::Resolved);
    CHECK(body(f, "symId").state == ExpansionState::Resolved && body(f, "symForward").state == ExpansionState::Deferred);
    CHECK(body(f, "redeclaredAssoc").state == ExpansionState::Resolved && body(f, "redeclaredAssocBound").state == ExpansionState::Resolved);
    const auto* signature = f.result.scalar_types->signature(f.id("nested", BindingKind::Function));
    CHECK(signature && signature->result.kind == Type::Nominal);
    Frontend mismatch;
    mismatch.check("class Box<T>(x: T) { let value = x }\nlet wrong: Bool = Box(1).value", {DiagCode::ScalarTypeMismatch});
    Frontend wrongSelf;
    wrongSelf.check("class C { type This = Self }\nclass Box { fun echo(x: C.This): C.This = x }\nlet wrong: Box = Box().echo(Box())", {DiagCode::NoMatchingCall});
    Frontend forwardMismatch;
    forwardMismatch.check("interface Has { let value: Int }\nfun read<T: Has>(x: T): Int = x.value\nlet wrong: Bool = read(Cell())\nclass Cell: Has { let value = 1 }",
                          {DiagCode::ScalarTypeMismatch});
    Frontend wrongInferredReq;
    wrongInferredReq.check("interface I { let value = 1 }\nclass Bad: I { let value = true }", {DiagCode::MissingImplementation});
    Frontend wrongCtorSelf;
    wrongCtorSelf.check("class Box<T>(let value: T, let next: Self) { fun bad(): Box<Int> = Box<Int>(2, self) }", {DiagCode::NoMatchingCall});
    Frontend wrongProjectedCall;
    wrongProjectedCall.check("interface I { type Item }\nclass C: I { type Item = Int }\nfun take<T: I>(x: T, y: T.Item): T.Item = y\nlet bad = take(C(), true)",
                             {DiagCode::NoMatchingCall});
    Frontend forwardParamBound;
    forwardParamBound.check("interface Root { type Item }\ninterface I<A> { fun echo(x: A): A }\nfun f<T: I<U.Item>, U: Root>(x: T): Bool = x.echo(true)",
                            {DiagCode::NoMatchingCall});
}
} // namespace

int main() {
    symbolic_bodies_and_returns();
    invalid_bodies_without_instantiation();
    bounds_associated_types_and_generic_conformance();
    self_defaults_and_properties();
    rejected_dependent_conformance();
    unsupported_contracts_stay_deferred();
    receiver_contexts_and_nested_interface_types();
}
