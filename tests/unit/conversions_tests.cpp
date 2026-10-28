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
static_assert(Type{Type::Int8}.optional().element() == Type::Int8);
static_assert(Type{}.optional() == Type::Deferred);

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};
    void check(std::string_view source, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        const auto id = session.sources().add("conversions.phs", source);
        CHECK(id);
        const auto* syntax = session.parse(*id);
        if (session.diagnostics().has_errors(*id)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        result = session.check(*id);
        CHECK(result.scalar_types);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        const auto count = session.diagnostics().error_count();
        CHECK(session.check(*id).scalar_types == result.scalar_types && session.diagnostics().error_count() == count);
    }
    BindingId binding(std::string_view name) const {
        for (usize i = 0; i < result.resolution->bindings().size(); ++i) {
            const auto& b = result.resolution->bindings()[i];
            if (b.kind == BindingKind::Variable && session.names().str(b.name.sym) == name) return BindingId{i};
        }
        CHECK(false);
        return {};
    }
    Type type(std::string_view name) const { return result.scalar_types->binding(binding(name)); }
    const ast::CallExpr* call(std::string_view name) const {
        return ast::cast<ast::CallExpr>(ast::cast<ast::VarItem>(result.resolution->binding(binding(name)).item)->init);
    }
    const CheckedConversion& conversion(std::string_view name) const {
        const auto* conversion = result.scalar_types->conversion(call(name));
        CHECK(conversion);
        return *conversion;
    }
};

void conversions_and_optional_facts() {
    Frontend f;
    f.check(R"(
let small: Int8 = -128
let widened = Int64(small)
let literal = Int8(-128)
let maximum = UInt64(18446744073709551615)
let checked = Int8.checked(1000)
let wrapped = Int8.truncating(maximum)
let annotated: Int8? = checked
let lifted: Int8? = small
let nested: Int8? ? = checked
let unwrapped = checked!
fun preserve(value: Int8?): Int8? = value
let viaFunction = preserve(checked)
fun inferred() = Int8.checked(1)
let inferredResult = inferred()
let comparison = checked == small
)");
    CHECK(f.type("widened") == Type::Int64 && f.type("literal") == Type::Int8);
    CHECK(f.type("checked") == Type{Type::Int8}.optional());
    CHECK(f.type("annotated") == f.type("lifted") && f.type("annotated") == f.type("checked"));
    CHECK(f.type("nested") == Type{Type::Int8}.optional().optional());
    CHECK(f.type("unwrapped") == Type::Int8 && f.type("viaFunction") == f.type("checked"));
    CHECK(f.type("inferredResult") == f.type("checked") && f.type("comparison") == Type::Deferred);
    CHECK(scalar_type_name(f.type("nested")) == "Int8??");
    const auto& wide = f.conversion("widened");
    CHECK(wide.state == CallState::Resolved && wide.kind == ConversionKind::Widening);
    CHECK(wide.source == Type::Int8 && wide.destination == Type::Int64);
    CHECK(f.conversion("checked").kind == ConversionKind::Checked && f.conversion("checked").source == Type::Int);
    CHECK(f.conversion("wrapped").kind == ConversionKind::Truncating && f.conversion("wrapped").source == Type::UInt64);
    const auto* call = f.result.scalar_types->call(f.call("checked"));
    CHECK(call && call->kind == CallKind::NumericConversion && call->state == CallState::Resolved);
    CHECK(f.result.resolution->binding(call->target).kind == BindingKind::BuiltinType);
    const auto* saved = &f.conversion("checked");
    for (int i = 0; i < 128; ++i) {
        const auto id = f.session.sources().add("another", "let value = UInt8.truncating(1000)");
        CHECK(id && f.session.check(*id).scalar_types && !f.session.diagnostics().has_errors(*id));
    }
    CHECK(&f.conversion("checked") == saved && saved->state == CallState::Resolved);
}

void widening_boundaries() {
    for (std::string_view source : {
        "let x: Int8 = 1\nlet y = Int16(x)", "let x: UInt8 = 1\nlet y = UInt16(x)",
        "let x: UInt8 = 255\nlet y = Int16(x)", "let x: UInt32 = 1\nlet y = Int64(x)",
        "let x: Int = 1\nlet y = Int64(x)", "let x: Int64 = 1\nlet y = Int(x)",
        "let x: UInt = 1\nlet y = UInt64(x)", "let x: Float32 = 1.0\nlet y = Float64(x)"
    }) { Frontend f; f.check(source); }
    for (std::string_view source : {
        "let x: Int16 = 1\nlet y = Int8(x)", "let x: UInt16 = 1\nlet y = Int16(x)",
        "let x: Int8 = 1\nlet y = UInt64(x)", "let x: UInt64 = 1\nlet y = Int64(x)",
        "let x: Float64 = 1.0\nlet y = Float32(x)"
    }) { Frontend f; f.check(source, {DiagCode::NarrowingConversion}); }
    Frontend range;
    range.check("let bad = Int8(128)", {DiagCode::ScalarLiteralRange});
    Frontend unsigned_negative;
    unsigned_negative.check("let bad = UInt8(-1)", {DiagCode::InvalidScalarOperand});
}

void invalid_shapes_operands_and_optionals() {
    for (std::string_view expr : {"Int64()", "Int64(1, 2)", "Int64(value: 1)", "Int8.checked()", "Int8.truncating(value: 1)"}) {
        Frontend f; f.check(std::format("let bad = {}", expr), {DiagCode::ConversionArguments});
    }
    for (std::string_view expr : {"Int64(true)", "Int8.checked(\"bad\")", "Int8.truncating('a')", "Float64(())",
                                  "Int8.checked(none)", "Int64(([1, 2]))"}) {
        Frontend f; f.check(std::format("let bad = {}", expr), {DiagCode::ConversionOperand});
    }
    Frontend optional_source;
    optional_source.check("let result = Int8.checked(1)\nlet bad = Int64(result)", {DiagCode::ConversionOperand});
    Frontend scalar_target;
    scalar_target.check("let bad: Int8 = Int8.checked(1)", {DiagCode::ScalarTypeMismatch});
    Frontend different_optional;
    different_optional.check("let bad: UInt8? = Int8.checked(1)", {DiagCode::ScalarTypeMismatch});
    Frontend optional_operand;
    optional_operand.check("let result = Int8.checked(1)\nlet bad = result + 1", {DiagCode::InvalidScalarOperand});
    Frontend function_argument;
    function_argument.check("fun f(x: Int8) = x\nlet bad = f(Int8.checked(1))", {DiagCode::NoMatchingCall});
    Frontend cascade;
    cascade.check("let bad = Int64(true + 1)", {DiagCode::InvalidScalarOperand});
}

void deferred_policies_and_shadowing() {
    Frontend f;
    f.check(R"(
fun opaque<T>(): Int = 1
let pending = Int8.checked(opaque())
let value: Int = 1
let cross = Float64(value)
let fraction: Float64 = 1.0
let floatingNarrow = Float32.checked(fraction)
type Alias = Int8
let alias = Alias.truncating(1)
struct Int8 { static fun checked(value: Int): Bool = true }
let shadowed = Int8.checked(1)
)", {DiagCode::UnknownMember});
    // The file's Int8 shadows the built-in throughout, including pending.
    CHECK(!f.result.scalar_types->conversion(f.call("pending")));
    CHECK(!f.result.scalar_types->conversion(f.call("alias")) && !f.result.scalar_types->conversion(f.call("shadowed")));
    CHECK(f.type("alias") == Type::Error && f.type("shadowed") == Type::Bool);
    CHECK(f.conversion("cross").state == CallState::Deferred && f.conversion("cross").source == Type::Int);
    CHECK(f.conversion("floatingNarrow").state == CallState::Deferred);
    Frontend pending;
    pending.check("fun opaque<T>(): Int = 1\nlet result = Int8.checked(opaque())");
    CHECK(pending.conversion("result").state == CallState::Deferred && pending.type("result") == Type::Deferred);
    Frontend value_shadow;
    value_shadow.check("fun Int64(value: Bool): Bool = value\nlet result = Int64(true)");
    CHECK(!value_shadow.result.scalar_types->conversion(value_shadow.call("result")) && value_shadow.type("result") == Type::Bool);
}

void optional_arguments_and_long_dependencies() {
    Frontend f;
    f.check(R"(
fun choose(value: Int8): Int8 = value
fun choose(value: Int8?): Int8? = value
let scalar = choose(Int8(1))
let optional = choose(Int8.checked(1))
fun rank(value: Int?): Bool = true
fun rank(value: Int16): String = "narrow"
let preferred = rank(1)
)");
    CHECK(f.type("scalar") == Type::Int8 && f.type("optional") == Type{Type::Int8}.optional());
    CHECK(f.type("preferred") == Type::Bool);
    Frontend chain;
    std::string source;
    for (int i = 0; i < 10000; ++i) source += std::format("let n{} = Int64(n{})\n", i, i + 1);
    source += "let n10000: Int8 = 1";
    chain.check(source, {}, false);
    CHECK(chain.type("n0") == Type::Int64);
}

void bare_unit_returns_lift_like_explicit_unit() {
    Frontend f;
    f.check(R"(
fun bare(): Unit? { return }
fun explicitUnit(): Unit? { return () }
fun tupleSpelling(): ()? { return }
fun nested(): Unit? ? { return }
fun plain() { return }
let bareResult = bare()
let explicitResult = explicitUnit()
let tupleResult = tupleSpelling()
let nestedResult = nested()
let plainResult = plain()
)");
    const auto optional_unit = Type{Type::Unit}.optional();
    CHECK(f.type("bareResult") == optional_unit && f.type("explicitResult") == optional_unit);
    CHECK(f.type("tupleResult") == optional_unit && f.type("nestedResult") == optional_unit.optional());
    CHECK(f.type("plainResult") == Type::Unit);
    for (std::string_view type : {"Int", "Int8?", "Bool?", "Never", "Never?"}) {
        Frontend bad;
        bad.check(std::format("fun f(): {} {{ return }}", type), {DiagCode::ScalarTypeMismatch});
    }
}
}  // namespace

int main() {
    conversions_and_optional_facts();
    widening_boundaries();
    invalid_shapes_operands_and_optionals();
    deferred_policies_and_shadowing();
    optional_arguments_and_long_dependencies();
    bare_unit_returns_lift_like_explicit_unit();
}
