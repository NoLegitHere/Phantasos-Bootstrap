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
        const auto id = session.sources().add("generics.phs", source); CHECK(id);
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

void applications_and_substituted_members() {
    Frontend f;
    // `itself` and `items` move out of a borrow (3.30), which typing ignores.
    f.check(R"(
type List<T> = [T]
type Rows<T> = List<List<T>>
type Identity<T> = T
type Applied = Identity<Int8>
type Callable<T, E> = fun([T?], *mut E) -> shared T throws(E)
class Box<T>(let value: T) {
    type Values = [T]
    fun get(): T = value
    fun take(x: T): T = x
    fun other<U>(x: U): U = x
    fun shadow<T>(x: T): T = x
    fun values(x: own Values): Values = x
    fun itself(): Self = self
    static fun staticValue<U>(x: U): U = x
}
type Wrapped<T> = Box<List<T>>
let number = Box<Int8>(1)
let flag = Box<Bool>(true)
let field = number.value
let flagField = flag.value
let method = number.get()
let argument = number.take(2)
let other = number.other<Bool>(true)
let shadowed = number.shadow<Bool>(true)
let values = number.values([1, 2])
let itself = number.itself()
let statically = Box<Int>.staticValue<Bool>(true)
let wrapped: Wrapped<Int> = Wrapped<Int>([1])
let items = wrapped.value
let rows: Rows<Int8> = [[1], []]
let callback: Callable<Int, Bool>
let converted = Identity<Int8>(1)
let applied: Applied = 1
)", {DiagCode::MoveOutOfBorrow, DiagCode::MoveOutOfBorrow});
    CHECK(f.name(f.value("number")) == "Box<Int8>" && f.name(f.value("flag")) == "Box<Bool>");
    CHECK(f.value("field") == Type::Int8 && f.value("flagField") == Type::Bool);
    CHECK(f.value("method") == Type::Int8 && f.value("argument") == Type::Int8);
    CHECK(f.value("other") == Type::Bool && f.value("statically") == Type::Bool);
    CHECK(f.value("shadowed") == Type::Bool && f.name(f.value("values")) == "[Int8]");
    CHECK(f.value("itself") == f.value("number"));
    CHECK(f.name(f.value("wrapped")) == "Box<[Int]>" && f.name(f.value("items")) == "[Int]");
    CHECK(f.name(f.value("rows")) == "[[Int8]]");
    CHECK(f.name(f.value("callback")) == "fun([Int?], *mut Bool) -> shared Int throws(Bool)");
    CHECK(f.value("converted") == Type::Int8 && f.value("applied") == Type::Int8);
    CHECK(f.call("other").type_arguments == std::vector<Type>{Type::Bool});
    CHECK(f.call("argument").parameter_types == std::vector<Type>{Type::Int8});
    CHECK(f.result.scalar_types->descriptor(f.value("number"))->arguments == std::vector<Type>{Type::Int8});
}

void explicitly_applied_functions() {
    Frontend f;
    // `elements` moves an array out of a borrowed parameter (3.30), which typing ignores.
    f.check(R"(
fun identity<T>(x: T): T = x
fun elements<T>(x: [T]): [T] = x
fun twice<T, U>(first: T, second: U): T = first
fun overload<T>(x: T): T = x
fun overload<T, U>(x: T): Bool = true
fun constant<T>() = 1
let integer = identity<Int8>(1)
let boolean = identity<Bool>(true)
let lifted: Int? = 1
let optional = identity<Int?>(lifted)
let array = elements<Int8>([1, 2])
let reordered = twice<Int, Bool>(second: true, first: 1)
let overloadOne = overload<Int>(1)
let overloadTwo = overload<Int, Bool>(1)
let inferredReturn = constant<Int>()
)", {DiagCode::MoveOutOfBorrow});
    CHECK(f.value("integer") == Type::Int8 && f.value("boolean") == Type::Bool);
    CHECK(f.value("optional") == Type{Type::Int}.optional() && f.name(f.value("array")) == "[Int8]");
    CHECK(f.value("reordered") == Type::Int && f.call("reordered").parameters == (std::vector<usize>{1, 0}));
    CHECK(f.value("overloadOne") == Type::Int && f.value("overloadTwo") == Type::Bool);
    CHECK(f.value("inferredReturn") == Type::Int);
    const auto* signature = f.result.scalar_types->signature(f.id("identity", BindingKind::Function));
    CHECK(signature && signature->generic && signature->parameters.size() == 1 && signature->parameters.front().kind == Type::TypeParameter);
    CHECK(signature->result == signature->parameters.front()); // applications never overwrite symbolic declaration facts
    const auto* retained = f.result.scalar_types->call(f.invocation("integer"));
    for (int i = 0; i < 128; ++i) {
        const auto file = f.session.sources().add("other.phs", "let value = 1"); CHECK(file);
        CHECK(f.session.check(*file).scalar_types);
    }
    CHECK(f.result.scalar_types->call(f.invocation("integer")) == retained);
}

void rejected_applications() {
    for (const auto& [source, code] : std::vector<std::pair<std::string_view, DiagCode>>{
        {"class Box<T> {}\nlet x: Box<Int, Bool>", DiagCode::GenericArguments},
        {"class Box<T> {}\nlet x = Box<Int, Bool>()", DiagCode::GenericArguments},
        {"let x: Int<Bool>", DiagCode::GenericArguments},
        {"fun f<T>(x: T<Int>) {}", DiagCode::GenericArguments},
        {"type Id<T> = T\nlet x: Id<Int, Bool>", DiagCode::GenericArguments},
        {"fun f<T>(x: T): T = x\nlet x = f<Int, Bool>(1)", DiagCode::GenericArguments},
        {"fun f(x: Int): Int = x\nlet x = f<Int>(1)", DiagCode::GenericArguments},
        {"fun f<T>(x: T): T = x\nlet x = f<Int>(true)", DiagCode::NoMatchingCall},
        {"fun f<T>(x: T): T = x\nlet x = f<Int8>(128)", DiagCode::ScalarLiteralRange},
        {"fun f<T>(x: T): T = x\nlet x: Bool = f<Int>(1)", DiagCode::ScalarTypeMismatch},
        {"class Box<T>(let value: T) {}\nlet x = Box<Int>(true)", DiagCode::NoMatchingCall},
        {"class Box<T>(let value: T) {}\nlet x: Box<Int> = Box<Bool>(true)", DiagCode::ScalarTypeMismatch},
        {"class Box<T>(let value: T) {}\nlet x = Box<Int>(1).missing", DiagCode::UnknownMember},
        {"type A<T> = A<T>\nlet x: A<Int>", DiagCode::AliasCycle},
        {"type A<T> = B<[T]>\ntype B<U> = A<U>\nlet x: A<Int>", DiagCode::AliasCycle},
        {"type A<T> = Box<A<[T]>>\nclass Box<T> {}\nlet x: A<Int>", DiagCode::AliasCycle}
    }) { Frontend bad; bad.check(source, {code}); }
}

void pending_contracts() {
    Frontend f;
    f.check(R"(
interface I { fun read(): Int }
class Box<T>(let value: T) where T: Copy {}
let constrained = Box<Int>(1)
fun bound<T: Copy>(x: T): T = x
let bounded = bound<Int>(1)
fun inferred<T>(x: T): T = x
let inference = inferred(1)
let noneArgument = inferred<Int?>(none)
extern "C" { fun deferredValue<T>(marker: inout Bool): T }
fun dependentDefault<T>(x: T = deferredValue<T>(true)): T = x
let defaulted = dependentDefault<Int>()
class Owner<T> { class Nested {} }
let captured: Owner.Nested
open class Base<T> { let value: T }
class Derived<T>: Base<T> {}
let inherited: Derived<Int>
let dependent = inherited.value
)");
    CHECK(f.value("inference") == Type::Int && f.call("inference").inferred_type_arguments);
    for (std::string_view name : {"constrained", "bounded", "noneArgument", "defaulted", "captured", "dependent"})
        CHECK(f.value(name) == Type::Deferred);
}

void omitted_default_facts() {
    const std::string declarations = R"(
fun identity<T>(x: inout Bool): Int = 1
fun pending<T>(x: Int = identity<T>(true)): Int = x
class Pending<T>(x: Int = identity<T>(true)) {}
class Owner<T> { fun read(x: Int = identity<T>(true)): Int = x }
fun known<T>(x: Int = make()): Int = x
fun nested<T>(x: Int = pending<T>()): Int = x
fun closed<T>(x: Int = known<Int>()): Int = x
fun recursive<T>(x: Int = recursive<Int>()): Int = x
class Known<T>(x: Int = make()) {}
fun literal<T>(x: Int8 = 1): Int8 = x
fun make() = 1
)";
    const std::string calls = R"(
let deferred = pending<Int>()
let deferredConstructor = Pending<Int>()
let deferredMethod = Owner<Int>().read()
let supplied = pending<Int>(1)
let suppliedConstructor = Pending<Int>(1)
let suppliedMethod = Owner<Int>().read(1)
let resolved = known<Int>()
let resolvedConstructor = Known<Int>()
let resolvedLiteral = literal<Int>()
let nestedPending = nested<Int>()
let closedDefault = closed<Int>()
let recursiveDefault = recursive<Int>()
)";
    for (bool forward : {false, true}) {
        Frontend f;
        f.check(forward ? calls + declarations : declarations + calls);
        for (std::string_view name : {"deferred", "deferredConstructor", "deferredMethod", "nestedPending", "recursiveDefault"})
            CHECK(f.value(name) == Type::Deferred && f.call(name).state == CallState::Deferred);
        for (std::string_view name : {"supplied", "suppliedMethod", "resolved", "closedDefault"})
            CHECK(f.value(name) == Type::Int && f.call(name).state == CallState::Resolved);
        CHECK(f.call("suppliedConstructor").state == CallState::Resolved && f.call("resolvedConstructor").state == CallState::Resolved);
        CHECK(f.value("resolvedLiteral") == Type::Int8);
    }
}

void alias_qualified_applications() {
    Frontend f;
    f.check(R"(
class Outer { class Box<T>(let value: T) {}; type Identity<T> = T }
type Alias = Outer
type Again = Alias
type Applied = Again.Box<Int>
let value: Applied = Outer.Box<Int>(1)
let direct: Outer.Box<Int> = value
let alias: Alias.Box<Int> = value
let identity: Again.Identity<Int> = 1
type Optional = Outer?
let pending: Optional.Box<Int, Bool> = true
class Captured<T> { class Nested<U> {} }
type Dependent = Captured
let captured: Dependent.Nested<Int> = false
)");
    CHECK(f.value("value") == f.value("alias") && f.value("direct") == f.value("alias"));
    CHECK(f.value("identity") == Type::Int && f.value("pending") == Type::Deferred && f.value("captured") == Type::Deferred);
    for (const auto& [use, code] : std::vector<std::pair<std::string_view, DiagCode>>{
        {"let bad: Alias.Box<Int> = Outer.Box<Bool>(true)", DiagCode::ScalarTypeMismatch},
        {"let bad: Alias.Box<Int, Bool>", DiagCode::GenericArguments},
        {"let bad: Alias.Identity<Int, Bool>", DiagCode::GenericArguments},
        {"let bad: Alias.Missing<Int>", DiagCode::UnknownType}
    }) {
        Frontend bad;
        bad.check("class Outer { class Box<T>(let value: T) {}; type Identity<T> = T }\ntype Alias = Outer\n" + std::string(use), {code});
    }
}

void substituted_member_type_entities() {
    Frontend f;
    f.check(R"(
class Token {}
class Pair<A, B>(let first: A, let second: B) {}
class Box<T> { type Value = T; type Again = Value; type Maybe = T?; type Convert<U> = U; type PairWith<U> = Pair<U, T> }
let number = Box<Int8>.Value(1)
let again = Box<Int>.Again(1)
let object = Box<Token>.Value()
let converted = Box<Int>.Convert<Int8>(1)
let pair = Box<Int>.PairWith<Bool>(true, 1)
let optional = Box<Int>.Maybe(1)
)");
    CHECK(f.value("number") == Type::Int8 && f.value("again") == Type::Int && f.value("converted") == Type::Int8);
    CHECK(f.call("number").kind == CallKind::NumericConversion && f.call("object").kind == CallKind::NominalConstructor);
    CHECK(f.name(f.value("object")) == "Token" && f.name(f.value("pair")) == "Pair<Bool, Int>");
    CHECK(f.value("optional") == Type::Deferred);
    Frontend bad;
    bad.check("class Box<T> { type Value = T }\nlet bad: Bool = Box<Int>.Value(1)", {DiagCode::ScalarTypeMismatch});
    Frontend shadowed;
    shadowed.check("class Box<T> { type Value = T; static let Value = true }\nlet bad = Box<Int>.Value(1)", {DiagCode::NotCallable});
}

void pending_constructor_conventions() {
    for (std::string_view convention : {"inout", "borrow", "own"}) {
        Frontend f;
        f.check(std::format("class Box<T>(value: {} T) {{}}\nlet value = Box<Int>(1)\nlet pending: Bool = Box<Int>(1)", convention));
        CHECK(f.value("value") == Type::Deferred && f.call("value").state == CallState::Deferred);
        Frontend bad;
        bad.check(std::format("class Box<T>(value: {} T) {{}}\nlet bad = Box<Int>(true)", convention), {DiagCode::NoMatchingCall});
    }
}

void iterative_alias_graphs() {
    Frontend f;
    std::string source;
    for (int i = 0; i < 10000; ++i) source += std::format("type A{}<T> = A{}<T>\n", i, i + 1);
    source += "type A10000<T> = [T]\nlet values: A0<Int8> = [1]\n";
    f.check(source, {}, false);
    CHECK(f.name(f.value("values")) == "[Int8]");
    Frontend shared;
    source.clear();
    for (int i = 0; i < 32; ++i) source += std::format("type B{}<T> = fun(B{}<T>) -> B{}<T>\n", i, i + 1, i + 1);
    source += "type B32<T> = T\nlet value: B0<Int>\n";
    shared.check(source, {}, false);
    CHECK(shared.value("value").kind == Type::Function);
    CHECK(shared.name(shared.value("value")).ends_with("..."));
}
void generic_function_values() {
    // A generic function as a value takes its type arguments from the
    // expected function type or from explicit arguments, and a closure passed
    // to a generic function takes its parameter types from the call's other
    // arguments; its result may infer the rest.
    Frontend f;
    f.check(R"(
fun identity<T>(x: T): T = x
fun apply<T>(f: fun(T) -> T, x: T): T = f(x)
fun map<T, U>(x: T, f: fun(T) -> U): U = f(x)
fun compose<A, B, C>(f: fun(A) -> B, g: fun(B) -> C): fun(A) -> C = { a -> g(f(a)) }
let expected: fun(Int8) -> Int8 = identity
let explicit = identity<Bool>
let passed = apply(identity, 1)
let closure = apply({ s -> s + 1 }, 2)
let mapped = map(3, { x -> x > 1 })
let composed = compose({ a: Int -> a > 1 }, { b -> !b })
)");
    CHECK(f.name(f.value("expected")) == "fun(Int8) -> Int8" && f.name(f.value("explicit")) == "fun(Bool) -> Bool");
    CHECK(f.value("passed") == Type::Int && f.value("closure") == Type::Int && f.value("mapped") == Type::Bool);
    CHECK(f.name(f.value("composed")) == "fun(Int) -> Bool");
    // Inside its own body a generic function is a value for its own type
    // parameters, and explicit type arguments give a closure its type
    // over literal defaults.
    Frontend own;
    own.check(R"(
fun repeat<T>(x: T, n: Int): T {
    let same: fun(T, Int) -> T = repeat
    let pair: fun((T, Int), Int) -> (T, Int) = repeat
    return x
}
fun apply<T>(f: fun(T) -> T, x: T): T = f(x)
let annotated = apply<Int8>({ s: Int8 -> s }, 1)
let inferred = apply<Int8>({ s -> s + 1 }, 1)
let literal = apply({ x: Int8 -> x }, 1)
let both = apply({ x: Int8 -> x }, { y: Int8 -> y }(2))
struct Box<T>(value: T) {
    fun map<U>(f: fun(T) -> U): U = f(value)
}
let mapped = Box(value: 1).map<Bool>({ x: Int -> x > 0 })
let mappedInferred = Box(value: 1).map<Int>({ x -> x + 1 })
struct Pair<A, B>(f: fun(A) -> B)
type Flip<A, B> = Pair<B, A>
let flipped = Flip<Int, Bool>({ x: Bool -> 1 })
let flippedInferred = Flip<Int, Bool>({ x -> 1 })
fun make<T, U>(x: T, f: fun(T) -> fun(Int) -> U): fun(Int) -> U = f(x)
let nested = make(3, { x -> { y -> x > y } })
fun pick<T>(x: T): T = x
fun pick<T>(x: T, y: T) = use(pick)
fun use(h: fun(Int) -> Int): Int = h(1)
let picked = use(pick)
let explicitOne: fun(Int) -> Int = pick<Int>
let explicitTwo: fun(Int, Int) -> Int = pick<Int>
struct Handler(run: fun(Int) -> Int)
type Plain<T> = Handler
let plain = Plain<Bool>(run: { x -> x * 2 })
fun mixed(x: Int): Int = x
fun mixed<T>(x: T): T = x
let mixedValue: fun(Bool) -> Bool = mixed<Bool>
let mixedSingle = mixed<Int8>
fun phantom<T, U>(x: T): T = x
fun phantom<T, U>(x: T, y: T): T = y
let seeded: fun(Int) -> Int = phantom<Int, Bool>
)");
    CHECK(own.name(own.value("mixedValue")) == "fun(Bool) -> Bool" && own.name(own.value("mixedSingle")) == "fun(Int8) -> Int8");
    CHECK(own.name(own.value("seeded")) == "fun(Int) -> Int");
    CHECK(own.name(own.value("explicitTwo")) == "fun(Int, Int) -> Int" && own.name(own.value("plain")) == "Handler");
    CHECK(own.name(own.value("nested")) == "fun(Int) -> Bool" && own.value("picked") == Type::Int);
    CHECK(own.name(own.value("flipped")) == "Pair<Bool, Int>" && own.name(own.value("flippedInferred")) == "Pair<Bool, Int>");
    CHECK(own.value("annotated") == Type::Int8 && own.value("inferred") == Type::Int8);
    CHECK(own.value("literal") == Type::Int8 && own.value("both") == Type::Int8);
    CHECK(own.value("mapped") == Type::Bool && own.value("mappedInferred") == Type::Int);
    const auto* init = ast::cast<ast::VarItem>(f.result.resolution->binding(f.id("explicit")).item)->init;
    const auto arguments = f.result.scalar_types->function_arguments(init);
    CHECK(arguments.size() == 1 && arguments.front() == Type::Bool);
    for (const auto& [source, code] : std::vector<std::pair<std::string_view, DiagCode>>{
        {"fun f<T>(x: T): T = x\nlet g = f<Int, Bool>", DiagCode::GenericArguments},
        {"fun f(x: Int): Int = x\nlet g = f<Int>", DiagCode::GenericArguments},
        {"fun f(x: Int): Int = x\nfun f(x: Bool): Bool = x\nlet g = f<Int>", DiagCode::GenericArguments},
        {"fun f<T>(x: T): T = x\nlet g: fun(Int) -> Bool = f", DiagCode::ScalarTypeMismatch},
        {"fun f<T>(g: fun(T) -> T): Int = 0\nlet x = f({ s -> s })", DiagCode::ClosureInference},
        {"fun f<T>(p: *T): *T = p\nlet g: fun(*Int) -> *mut Int = f", DiagCode::ScalarTypeMismatch},
        {"fun f<T>(x: T): T = x\nfun f<T>(x: T, y: T): T = y\nlet g: fun(Int) -> Int = f<Bool>", DiagCode::ScalarTypeMismatch},
        {"fun f<T>(x: T): T = x\nfun f(x: Int): Int = x\nlet g = f<Int, Bool>", DiagCode::GenericArguments},
    }) { Frontend bad; bad.check(source, {code}); }
    // Closures that would infer each other's parameter types infer neither.
    Frontend both;
    both.check("fun f<A, B>(g: fun(A) -> B, h: fun(B) -> A): Int = 0\nlet x = f({ s -> s }, { t -> t })",
               {DiagCode::ClosureInference, DiagCode::ClosureInference});
}

} // namespace

int main() {
    omitted_default_facts();
    alias_qualified_applications();
    substituted_member_type_entities();
    pending_constructor_conventions();
    applications_and_substituted_members();
    explicitly_applied_functions();
    rejected_applications();
    pending_contracts();
    generic_function_values();
    iterative_alias_graphs();
}
