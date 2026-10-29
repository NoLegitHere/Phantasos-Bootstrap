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
        const auto id = session.sources().add("composites.phs", source); CHECK(id);
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

void canonical_annotations_and_aliases() {
    Frontend f;
    f.check(R"(
type Numbers = [Int8]
type Same = [Int8]
type Mutable = *mut Numbers?
type Callback = fun(Int8, Numbers) -> Bool
class C {}
type Object = C
let a: Numbers = [1, 2]
let b: Same = a
let p: Mutable
let callback: Callback
let object: Object = C()
let direct: C = object
)");
    const auto* numbers = f.result.scalar_types->alias(f.id("Numbers", BindingKind::TypeAlias));
    const auto* same = f.result.scalar_types->alias(f.id("Same", BindingKind::TypeAlias));
    CHECK(numbers && same && numbers->state == ExpansionState::Resolved && numbers->scalar == same->scalar);
    CHECK(f.value("a") == f.value("b") && f.value("a").kind == Type::Array);
    CHECK(f.result.scalar_types->descriptor(f.value("a"))->element == Type::Int8);
    CHECK(f.value("p").kind == Type::Pointer && f.result.scalar_types->descriptor(f.value("p"))->mutable_pointer);
    CHECK(f.value("callback").kind == Type::Function && f.name(f.value("callback")) == "fun(Int8, [Int8]) -> Bool");
    CHECK(f.value("object") == f.value("direct") && f.name(f.value("object")) == "C");
    CHECK(f.result.scalar_types->descriptor(f.value("object"))->declaration == f.id("C", BindingKind::Type));
}

void contextual_arrays_and_indexing() {
    Frontend f;
    f.check(R"(
let bytes: [UInt8] = [0, 255]
let empty: [Int8] = []
let inferred = [1, 2]
let nested: [[Int8]] = [[1], [2, 3]]
let value = bytes[0]
let inner = nested[0][0]
fun sum(values: [Int8]): Int8 = values[0]
let called = sum([127])
fun wide(values: [UInt64]): UInt64 = values[0]
let maximum = wide([18446744073709551615])
fun build() = [1, 2]
let returned = build()
let parenthesized: [Int8] = ([1, 2])
let parenthesizedCall = sum(([127]))
let parenthesizedIndex = ([1])[0]
fun nonreturning(x: Never): Int = [1][x]
)");
    CHECK(f.value("bytes").kind == Type::Array && f.value("empty").kind == Type::Array);
    CHECK(f.value("value") == Type::UInt8 && f.value("inner") == Type::Int8);
    CHECK(f.value("called") == Type::Int8 && f.value("maximum") == Type::UInt64);
    CHECK(f.name(f.value("inferred")) == "[Int]" && f.value("returned") == f.value("inferred"));
    CHECK(f.name(f.value("nested")) == "[[Int8]]");
    CHECK(f.name(f.value("parenthesized")) == "[Int8]" && f.value("parenthesizedCall") == Type::Int8);
    CHECK(f.value("parenthesizedIndex") == Type::Int);
    for (const auto& test : std::vector<std::pair<std::string_view, DiagCode>>{
        {"let x: [Int8] = [128]", DiagCode::ScalarLiteralRange},
        {"let x: [Int] = [true]", DiagCode::ScalarTypeMismatch},
        {"let x = [1, [2]]", DiagCode::ScalarTypeMismatch},
        {"let x = [[1], 2]", DiagCode::ScalarTypeMismatch},
        {"let x = [1]\nlet bad = x[true]", DiagCode::InvalidArrayIndex},
        {"let bad = [][true]", DiagCode::InvalidArrayIndex},
        {"let bad = [1][(none)]", DiagCode::InvalidArrayIndex},
        {"let x: [Int]? = [1]\nlet bad = x[0]", DiagCode::InvalidArrayIndex},
        {"fun f(x: Int) {}\nlet bad = f([])", DiagCode::NoMatchingCall},
        {"fun f(x: [Int8]) {}\nlet bad = f([128])", DiagCode::ScalarLiteralRange}
    }) { Frontend bad; bad.check(test.first, {test.second}); }
    Frontend overload;
    overload.check("fun f(x: [Int]) {}\nfun f(x: [Bool]) {}\nlet bad = f([])", {DiagCode::AmbiguousCall});
}

void nominal_constructors_and_mismatches() {
    Frontend f;
    f.check(R"(
struct Point(x: Int8, y: Int8 = 0) { fun selfType(): Point = self }
type Alias = Point
let first = Point(1)
let second: Alias = Alias(y: 2, x: 3)
let points: [Point] = [first, second]
fun echo(value: Alias): Point = value
let result = echo(first)
)");
    CHECK(f.value("first").kind == Type::Nominal && f.value("first") == f.value("second"));
    CHECK(f.value("result") == f.value("first"));
    CHECK(f.call("first").kind == CallKind::NominalConstructor && f.call("first").state == CallState::Resolved);
    CHECK(f.call("second").parameters == std::vector<usize>({1, 0}));
    for (const auto& test : std::vector<std::pair<std::string_view, DiagCode>>{
        {"class A {}\nclass B {}\nlet bad: A = B()", DiagCode::ScalarTypeMismatch},
        {"class A {}\nlet bad: A = 1", DiagCode::ScalarTypeMismatch},
        {"open class A {}\nclass B: A {}\nlet bad: B = 1", DiagCode::ScalarTypeMismatch},
        {"open class A {}\nclass B: A {}\nlet bad: B = []", DiagCode::ScalarTypeMismatch},
        {"struct P(x: Int8) {}\nlet bad = P(true)", DiagCode::NoMatchingCall},
        {"interface Marker {}\nclass C(value: Int) where Int: Marker {}\nlet bad = C(true)", DiagCode::NoMatchingCall},
        {"struct P(x: Int8) {}\nlet bad = P()", DiagCode::NoMatchingCall},
        {"struct P(x: Int8) {}\nlet bad = P(128)", DiagCode::ScalarLiteralRange},
        {"abstract class A {}\nlet bad = A()", DiagCode::NotConstructible},
        {"interface A {}\nlet bad = A()", DiagCode::NotConstructible}
    }) { Frontend bad; bad.check(test.first, {test.second}); }
    Frontend inherited;
    inherited.check("open class A {}\nclass B: A {}\nlet base: A = B()\nfun f(x: A) {}\nlet pending = f(B())");
    CHECK(inherited.call("pending").state == CallState::Resolved && inherited.value("pending") == Type::Unit);
}

void pointers_functions_and_deferred_features() {
    Frontend f;
    f.check(R"(
type Pointer = *mut Int8
type Shared = shared Int8
type Weak = weak Int8
type Fallible = fun(Int) -> Int throws
type AlsoFallible = fun(Int) -> Int throws(Error)
type Callback = fun(Int8) -> Bool
let optionalFunction: Callback?
let fallible: Fallible
let sameFunction: AlsoFallible = fallible
let pointer: Pointer
let sharedValue: Shared
let weakValue: Weak
extern "C" { fun produce(): fun(Int8) -> Bool }
let callback = produce()
let result = callback(1)
fun invoke(callback: fun(Int8) -> Bool): Bool = callback(1)
let applied = invoke(callback)
class C { init(value: Int8) {} }
let secondary = C(1)
)");
    CHECK(f.value("pointer").kind == Type::Pointer && f.result.scalar_types->descriptor(f.value("pointer"))->mutable_pointer);
    CHECK(f.value("sharedValue").kind == Type::Shared && f.value("weakValue").kind == Type::Weak);
    CHECK(f.value("result") == Type::Bool && f.value("applied") == Type::Bool);
    CHECK(f.name(f.value("optionalFunction")) == "(fun(Int8) -> Bool)?");
    CHECK(f.value("fallible") == f.value("sameFunction"));
    CHECK(f.call("result").kind == CallKind::IndirectFunction && f.call("result").target == f.id("callback"));
    CHECK(f.call("secondary").state == CallState::Deferred);
    Frontend wrong;
    wrong.check("extern \"C\" { fun produce(): fun(Int8) -> Bool }\nlet callback = produce()\nlet bad = callback(true)", {DiagCode::NoMatchingCall});
    Frontend pointer_mismatch;
    pointer_mismatch.check("let x: *Int8\nlet y: *Bool = x", {DiagCode::ScalarTypeMismatch});
    Frontend function_mismatch;
    function_mismatch.check("extern \"C\" { fun produce(): fun(Int) -> Int }\nlet wrong: fun(Bool) -> Int = produce()", {DiagCode::ScalarTypeMismatch});
}

void stable_descriptors_and_deep_aliases() {
    Frontend f;
    f.check("let values: [Int8] = [1]");
    const auto type = f.value("values");
    const auto* descriptor = f.result.scalar_types->descriptor(type);
    for (int i = 0; i < 128; ++i) {
        const auto id = f.session.sources().add("another", "class Own {}\nlet value = Own()");
        CHECK(id && f.session.check(*id).scalar_types && !f.session.diagnostics().has_errors(*id));
    }
    CHECK(f.result.scalar_types->descriptor(type) == descriptor && descriptor->element == Type::Int8);
    Frontend deep;
    std::string source;
    for (int i = 0; i < 10000; ++i) source += std::format("type A{} = [A{}]\n", i, i + 1);
    source += "type A10000 = Int8\nlet value: A0";
    deep.check(source, {}, false);
    CHECK(deep.value("value").kind == Type::Array);
    CHECK(deep.name(deep.value("value")).size() == 20004);
}

void nested_array_defaults_and_never() {
    for (std::string_view array : {"[[], [1, true]]", "[[1, true], []]", "[[], ([1, true])]",
                                   "[[], [[], [1, true]]]"}) {
        Frontend bad;
        bad.check(std::format("let values = {}", array), {DiagCode::ScalarTypeMismatch});
    }
    Frontend f;
    f.check(R"(
extern "C" { fun stop(): Never }
let last = [1, stop()]
let first = [stop(), 1]
let allNever = [stop(), stop()]
let nested = [[stop()], [1]]
let emptyFirst = [[], [1]]
let typed: Int8 = 1
let anchored = [stop(), typed, 2]
fun opaque<T: Copy>(): UInt64 = UInt64(18446744073709551615)
let pendingFirst = [opaque<Int>(), 18446744073709551615]
let pendingLast = [18446744073709551615, opaque<Int>()]
)");
    CHECK(f.name(f.value("last")) == "[Int]" && f.value("first") == f.value("last"));
    CHECK(f.name(f.value("nested")) == "[[Int]]" && f.value("emptyFirst") == f.value("nested"));
    CHECK(f.name(f.value("anchored")) == "[Int8]");
    CHECK(f.value("allNever") == Type::Deferred);
    CHECK(f.value("pendingFirst") == Type::Deferred && f.value("pendingLast") == Type::Deferred);
    Frontend independent;
    independent.check("fun opaque<T: Copy>(): Int = 1\nlet values = [true, 1, opaque<Int>()]", {DiagCode::ScalarTypeMismatch});
}

void nested_concrete_anchors() {
    Frontend f;
    f.check(R"(
let byte: Int8 = 1
let left = [[1], [byte]]
let right = [[byte], [1]]
let parenthesized = [([1]), ([byte])]
let deep = [[[1]], [[byte]]]
let empty = [[], [[1]], [[byte]]]
extern "C" { fun stop(): Never }
let bottom = [[stop(), 1], [byte]]
let wide: UInt64 = 18446744073709551615
let maximum = [[18446744073709551615], [wide]]
let single: Float32 = 1.0
let floats = [[1.0], [single]]
)");
    CHECK(f.name(f.value("left")) == "[[Int8]]" && f.value("left") == f.value("right"));
    CHECK(f.value("parenthesized") == f.value("left") && f.value("bottom") == f.value("left"));
    CHECK(f.name(f.value("deep")) == "[[[Int8]]]" && f.value("empty") == f.value("deep"));
    CHECK(f.name(f.value("maximum")) == "[[UInt64]]");
    CHECK(f.name(f.value("floats")) == "[[Float32]]");
    for (std::string_view array : {"[[128], [byte]]", "[[byte], [128]]"}) {
        Frontend bad;
        bad.check(std::format("let byte: Int8 = 1\nlet values = {}", array), {DiagCode::ScalarLiteralRange});
    }
    for (std::string_view array : {"[[byte], [wide]]", "[[wide], [byte]]"}) {
        Frontend bad;
        bad.check(std::format("let byte: Int8 = 1\nlet wide: Int16 = 1\nlet values = {}", array), {DiagCode::ScalarTypeMismatch});
    }
}

void deferred_array_equality_checks_elements() {
    for (std::string_view expr : {"[1, true] == [true]", "[true] == [1, true]",
                                  "[1, true] != [true]", "[true] != [1, true]",
                                  "([[], [1, true]]) == []"}) {
        Frontend bad;
        bad.check(std::format("let bad = {}", expr), {DiagCode::ScalarTypeMismatch});
        CHECK(bad.value("bad") == Type::Error);
    }
    Frontend both;
    both.check("let bad = [1, true] == [false, 2]", {DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch});
    Frontend valid;
    valid.check(R"(
let same = [1] == [1]
let distinct = [1] != [true]
let empty = [] == []
let byte: Int8 = 1
let nested = ([[1], [byte]]) == [[]]
fun opaque<T: Copy>(): UInt64 = UInt64(18446744073709551615)
let pending = [opaque<Int>(), 18446744073709551615] == []
)");
    for (std::string_view binding : {"same", "distinct", "empty", "nested", "pending"})
        CHECK(valid.value(binding) == Type::Deferred);
}

void function_parameter_variance() {
    Frontend possible;
    possible.check("extern \"C\" { fun produce(): fun(Int?) -> Int }\nlet value: fun(Int) -> Int = produce()");
    CHECK(possible.value("value").kind == Type::Function);
    Frontend unsafe;
    unsafe.check("extern \"C\" { fun produce(): fun(Int) -> Int }\nlet value: fun(Int?) -> Int = produce()", {DiagCode::ScalarTypeMismatch});
    // The two directions of the same descriptor pair are not interchangeable:
    // the result comparison is possible, but the parameter comparison is not.
    Frontend higher_order;
    higher_order.check(R"(
type A = fun(Int?) -> Int
type B = fun(Int) -> Int
extern "C" { fun produce(): fun(A) -> A }
let value: fun(B) -> B = produce()
)", {DiagCode::ScalarTypeMismatch});
}

void bounded_type_names_and_mismatch_dags() {
    TypeTable table;
    Resolution resolution;
    Interner names;
    CHECK(table.name(Type::Int, resolution, names, 0).empty());
    CHECK(table.name(Type::Int, resolution, names, 1) == ".");
    CHECK(table.name(Type::Int, resolution, names, 3) == "Int");
    CHECK(table.name(Type::Int8, resolution, names, 3) == "...");
    CHECK(table.name(Type{Type::Int, 4}, resolution, names, 7) == "Int????");
    const auto optional = table.name(Type{Type::Int, std::numeric_limits<u32>::max()}, resolution, names, 16);
    CHECK(optional.size() <= 16 && optional.ends_with("..."));
    const auto wide = table.intern({.kind = Type::Function, .parameters = std::vector<Type>(10000, Type::Int), .result = Type::Bool});
    const auto wide_name = table.name(wide, resolution, names, 64);
    CHECK(wide_name.size() <= 64 && wide_name.ends_with("..."));

    Frontend f;
    std::string source;
    for (int i = 0; i < 64; ++i) {
        source += std::format("type A{} = fun(A{}, A{}) -> A{}\n", i, i + 1, i + 1, i + 1);
        source += std::format("type B{} = fun(B{}, B{}) -> B{}\n", i, i + 1, i + 1, i + 1);
    }
    source += "type A64 = fun() -> Int\ntype B64 = fun() -> Bool\n";
    source += "fun compare(value: A0) { let bad: B0 = value }";
    f.check(source, {DiagCode::ScalarTypeMismatch}, false);
    const auto& diagnostic = f.session.diagnostics().all()[0];
    CHECK(diagnostic.message.size() < 2200 && diagnostic.message.contains("..."));
    const auto root = f.result.scalar_types->alias(f.id("A0", BindingKind::TypeAlias))->scalar;
    const auto full = f.name(root);
    CHECK(full.size() <= TypeTable::kNameLimit && full.ends_with("..."));
}

void shared_descriptor_graph_relations() {
    Frontend f;
    std::string source;
    for (int i = 0; i < 64; ++i) {
        source += std::format("type A{} = fun(A{}) -> A{}\n", i, i + 1, i + 1);
        source += std::format("type B{} = fun(B{}) -> B{}\n", i, i + 1, i + 1);
    }
    source += "type A64 = fun() -> Int\ntype B64 = fun() -> Int throws\n";
    source += "fun compare(value: A0) { let pending: B0 = value }";
    f.check(source, {}, false);
}
}  // namespace

void array_members_and_loops() {
    Frontend f;
    f.check(R"(
let bytes: [UInt8] = [1, 2]
let count = bytes.count
let empty = bytes.isEmpty
let literalCount = [1, 2, 3].count
let maybe: [UInt8]? = [1]
let chainedCount = maybe?.count
let chainedEmpty = maybe?.isEmpty
fun pop(values: inout [Int8]?): Int8? = values?.removeLast()
fun last(values: inout [Int8]): Int8? = values.removeLast()
fun grow(values: inout [Int8]) { values.append(1) }
fun push(values: inout [[Int8]]) { values.append([]) }
fun loops(names: [String], limit: UInt8) {
    for name in names { print(name) }
    for byte in [UInt8(1)] { print(Int(byte)) }
    for small in 0..<limit { print(Int(small)) }
    for index in 0..10 { print(index) }
}
fun opaque<T: Copy>(): Int = 1
fun pending() { for n in [opaque<Int>()] {} }
)");
    CHECK(f.value("count") == Type::Int && f.value("empty") == Type::Bool && f.value("literalCount") == Type::Int);
    CHECK(f.value("chainedCount") == Type{Type::Int}.optional() && f.value("chainedEmpty") == Type{Type::Bool}.optional());
    CHECK(f.result.scalar_types->binding(f.id("name", BindingKind::Local)) == Type::String);
    CHECK(f.result.scalar_types->binding(f.id("byte", BindingKind::Local)) == Type::UInt8);
    CHECK(f.result.scalar_types->binding(f.id("small", BindingKind::Local)) == Type::UInt8);
    CHECK(f.result.scalar_types->binding(f.id("index", BindingKind::Local)) == Type::Int);
    for (const auto& test : std::vector<std::pair<std::string_view, DiagCode>>{
        {"let a = [1]\nlet bad = a.size", DiagCode::UnknownMember},
        {"let a = [1]\nlet bad = a.append", DiagCode::UnknownMember},
        {"fun f(a: inout [Int8]) { a.append(128) }", DiagCode::ScalarLiteralRange},
        {"fun f(a: inout [Int]) { a.append(true) }", DiagCode::ScalarTypeMismatch},
        {"fun f(a: inout [Int]) { a.append() }", DiagCode::NoMatchingCall},
        {"fun f(a: inout [Int]) { a.append(value: 1) }", DiagCode::NoMatchingCall},
        {"fun f(a: inout [Int]) { let x = a.removeLast(1) }", DiagCode::NoMatchingCall},
        {"class C\nlet a = [C()]\nlet bad = a.clone()", DiagCode::UnknownMember},
        {"let a = [1]\nlet bad = a.count()", DiagCode::NotCallable},
        {"let a: [Int]? = [1]\nlet bad = a?.size", DiagCode::UnknownMember},
        {"fun f(a: inout [Int8]?) { a?.append(300) }", DiagCode::ScalarLiteralRange},
        {"fun f() { var a = [1]\n a.count = 2 }", DiagCode::ImmutableAssignment},
        {"fun set(x: inout Bool) {}\nfun f() { var a = [1]\n set(a.isEmpty) }", DiagCode::ImmutableAssignment},
        {"fun f(a: [Int]?) { a?.append(1) }", DiagCode::ImmutableAssignment},
        {"fun f(n: Int) { for i in n {} }", DiagCode::NotIterable},
        {"fun f(a: [Int]?) { for i in a {} }", DiagCode::NotIterable},
        {"fun f() { for i in 1 {} }", DiagCode::NotIterable},
        {"fun f() { for i in 0..<true {} }", DiagCode::InvalidScalarOperand},
        {"fun f(a: Int8) { for i in a..<300 {} }", DiagCode::ScalarLiteralRange},
        {"let r = 0..<3", DiagCode::RangeOutsideFor},
        {"fun f() { for i in [0..<3] {} }", DiagCode::RangeOutsideFor},
        {"fun f(a: [Int]) { for i in a { let x: Bool = i } }", DiagCode::ScalarTypeMismatch}
    }) { Frontend bad; bad.check(test.first, {test.second}); }
}

int main() {
    canonical_annotations_and_aliases();
    contextual_arrays_and_indexing();
    nominal_constructors_and_mismatches();
    pointers_functions_and_deferred_features();
    stable_descriptors_and_deep_aliases();
    nested_array_defaults_and_never();
    nested_concrete_anchors();
    deferred_array_equality_checks_elements();
    function_parameter_variance();
    bounded_type_names_and_mismatch_dags();
    shared_descriptor_graph_relations();
    array_members_and_loops();
}
