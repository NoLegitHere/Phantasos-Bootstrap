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
        const auto id = session.sources().add("types.phs", source);
        CHECK(id);
        const auto* syntax = session.parse(*id);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        result = session.check(*id);
        CHECK(result.resolution && result.scalar_types);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        const auto errors = session.diagnostics().error_count();
        CHECK(session.check(*id).scalar_types == result.scalar_types);
        CHECK(session.diagnostics().error_count() == errors);
    }

    Type binding(std::string_view name, usize occurrence = 0) const {
        for (usize i = 0; i < result.resolution->bindings().size(); ++i) {
            const auto& b = result.resolution->bindings()[i];
            if (session.names().str(b.name.sym) == name) {
                if (occurrence == 0) return result.scalar_types->binding(BindingId{i});
                --occurrence;
            }
        }
        CHECK(false);
        return Type::Error;
    }
};

void retained_inference_and_context() {
    Frontend f;
    f.check(R"(
let inferred = later
let later: Int8 = (1 + 2) * 3
let defaultInt = 1 + 2
let defaultFloat = 1.0 + 2.0
let smallFloat: Float32 = 1.0 + 2.0
let left = 1 + later
let right = later + 1
let comparison = later < 2
let text = "hello"
let character = 'x'
let unit = ()
fun conditions(flag: Bool, count: Int8 = -128): Int8 {
    var local: Int8 = 1
    local += 2
    if flag && !false { local = 3 }
    while count < 10 { break }
    return local
}
)");
    CHECK(f.binding("inferred") == Type::Int8);
    CHECK(f.binding("later") == Type::Int8);
    CHECK(f.binding("left") == Type::Int8 && f.binding("right") == Type::Int8);
    CHECK(f.binding("defaultInt") == Type::Int && f.binding("defaultFloat") == Type::Float64);
    CHECK(f.binding("smallFloat") == Type::Float32 && f.binding("comparison") == Type::Bool);
    CHECK(f.binding("text") == Type::String && f.binding("character") == Type::Char && f.binding("unit") == Type::Unit);
    CHECK(f.binding("flag") == Type::Bool && f.binding("count") == Type::Int8 && f.binding("local") == Type::Int8);
    const auto* var = ast::cast<ast::VarItem>(f.result.syntax->items[1]);
    const auto* mul = ast::cast<ast::BinaryExpr>(var->init);
    CHECK(f.result.scalar_types->expression(mul) == Type::Int8);
    CHECK(f.result.scalar_types->expression(mul->lhs) == Type::Int8);
    CHECK(f.result.scalar_types->expression(mul->rhs) == Type::Int8);
}

void diagnostics_and_suppression() {
    Frontend f;
    f.check(R"(
let a: Int = true
let cascading = a + 1
let b: Bool = 1
let c = true + false
let d = ~1.0
let e = 1 && true
let f = 1 + 1.0
let fixed: Int8 = 1
let widening: Int64 = fixed
fun defaults(x: Bool = 1): Bool = 1
fun returns(): Int { return }
fun unit() { return 1 }
fun assignment() { var value = 1; value = false; if 1 {}; while "bad" { break } }
)", {DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch,
      DiagCode::InvalidScalarOperand, DiagCode::InvalidScalarOperand,
      DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch,
      DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch,
      DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch,
      DiagCode::ScalarTypeMismatch});
    CHECK(f.binding("a") == Type::Error && f.binding("cascading") == Type::Error);
}

void integer_and_float_boundaries() {
    Frontend valid;
    valid.check(R"(
let i8: Int8 = -128
let i8max: Int8 = 127
let u8: UInt8 = 255
let i16: Int16 = -32768
let u16: UInt16 = 65535
let i32: Int32 = -2147483648
let u32: UInt32 = 4294967295
let i64: Int64 = -9223372036854775808
let u64: UInt64 = 18446744073709551615
let native: Int = -(9223372036854775808)
let f32: Float32 = -3.4028234e38
let f64: Float64 = 1.0e300
)");
    Frontend invalid;
    invalid.check(R"(
let a: Int8 = 128
let b: Int8 = -129
let c: UInt8 = 256
let d: Int16 = 32768
let e: UInt16 = 65536
let f: Int32 = -2147483649
let g: UInt32 = 4294967296
let h: Int64 = 9223372036854775808
let i = 9223372036854775808
let j: Float32 = 1.0e39
let k: UInt8 = -1
)", {DiagCode::ScalarLiteralRange, DiagCode::ScalarLiteralRange, DiagCode::ScalarLiteralRange,
      DiagCode::ScalarLiteralRange, DiagCode::ScalarLiteralRange, DiagCode::ScalarLiteralRange,
      DiagCode::ScalarLiteralRange, DiagCode::ScalarLiteralRange, DiagCode::ScalarLiteralRange,
      DiagCode::ScalarLiteralRange, DiagCode::InvalidScalarOperand});
}

void operators() {
    Frontend f;
    f.check(R"(
fun ops(a: Int8, b: Int8, c: Float32, d: Float32) {
    a + b; a - b; a * b; a / b; a % b
    a &+ b; a &- b; a &* b; a & b; a | b; a ^ b; a << b; a >> b
    a == b; a != b; a < b; a <= b; a > b; a >= b; ~a; -a
    c + d; c - d; c * d; c / d; c == d; c < d; -c
    true == false; 'a' < 'b'; "a" == "b"; () == ()
    var copy: Float32 = c; copy += d
}
fun bottom(x: Never): Int8 = x + 1
fun negatedBottom(x: Never): Int = -x
)");
    Frontend bad;
    bad.check(R"(
fun ops(a: Int8, b: Int16, u: UInt8) {
    a + b; a == b; -u
    true < false; "a" + "b"; 1.0 % 2.0; 1.0 &+ 2.0
    var flag = true; flag += false
}
)", {DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch,
      DiagCode::InvalidScalarOperand, DiagCode::InvalidScalarOperand, DiagCode::InvalidScalarOperand,
      DiagCode::InvalidScalarOperand, DiagCode::InvalidScalarOperand, DiagCode::InvalidScalarOperand});
}

void deferred_types_and_shadowing() {
    Frontend f;
    f.check(R"(
type Number = Int
type Wide = UInt64
struct Bool {}
let alias: Number = 1
let wideAlias: Wide = 18446744073709551615
let wideOptional: UInt64? = 18446744073709551615
let wideArray: [UInt64] = [18446744073709551615]
let wideBranch: UInt64 = if true { 18446744073709551615 } else { 0 }
let nominal: Bool = Bool()
let cyclicA = cyclicB
let cyclicB = cyclicA
let opaque = foreign()
let wideCall = takesWide(18446744073709551615)
extern "C" { fun foreign(): Int }
fun takesWide(x: UInt64): UInt64 = x
fun generic<T>(x: T): T = x
fun optional(x: Int?) { if let value = x { value + 1 } }
fun names(x: Int8) { let x = x + 1; if true { let x = "inner"; x }; x }
)");
    CHECK(f.binding("alias") == Type::Int && f.binding("nominal").kind == Type::Nominal);
    CHECK(f.binding("wideAlias") == Type::UInt64 && f.binding("wideOptional") == Type{Type::UInt64}.optional());
    CHECK(f.binding("wideArray").kind == Type::Array && f.binding("wideCall") == Type::UInt64);
    CHECK(f.binding("wideBranch") == Type::UInt64);
    CHECK(f.binding("cyclicA") == Type::Deferred && f.binding("cyclicB") == Type::Deferred);
    CHECK(f.binding("opaque") == Type::Int);
    CHECK(f.binding("x", 3) == Type::Int8); // takesWide, generic, optional, then names' parameter
    CHECK(f.binding("x", 4) == Type::Int8 && f.binding("x", 5) == Type::String);
}

void discarded_numeric_expressions() {
    Frontend valid;
    valid.check(R"(
fun defaults() { 1; 1.0; (2 + 3) }
let contextual: UInt64 = if true { 1; 18446744073709551615 } else { 18446744073709551615 }
fun use(value: UInt64) {}
fun calls() { use(18446744073709551615) }
)");
    const auto* body = ast::cast<ast::BlockExpr>(ast::cast<ast::FunItem>(valid.result.syntax->items[0])->body);
    CHECK(valid.result.scalar_types->expression(ast::cast<ast::ExprStmt>(body->stmts[0])->expr) == Type::Int);
    CHECK(valid.result.scalar_types->expression(ast::cast<ast::ExprStmt>(body->stmts[1])->expr) == Type::Float64);
    CHECK(valid.result.scalar_types->expression(ast::cast<ast::ExprStmt>(body->stmts[2])->expr) == Type::Int);
    const auto* branch = ast::cast<ast::IfExpr>(ast::cast<ast::VarItem>(valid.result.syntax->items[1])->init);
    CHECK(valid.result.scalar_types->expression(ast::cast<ast::ExprStmt>(branch->then_block->stmts[0])->expr) == Type::Int);
    // The used branch tails take the annotation's type.
    CHECK(valid.result.scalar_types->expression(ast::cast<ast::ExprStmt>(branch->then_block->stmts[1])->expr) == Type::UInt64);
    const auto* otherwise = ast::cast<ast::BlockExpr>(branch->else_branch);
    CHECK(valid.result.scalar_types->expression(ast::cast<ast::ExprStmt>(otherwise->stmts[0])->expr) == Type::UInt64);
    const auto* calls = ast::cast<ast::BlockExpr>(ast::cast<ast::FunItem>(valid.result.syntax->items[3])->body);
    const auto* call = ast::cast<ast::CallExpr>(ast::cast<ast::ExprStmt>(calls->stmts[0])->expr);
    CHECK(valid.result.scalar_types->expression(call->args[0].value) == Type::UInt64);

    for (std::string_view source : {
        "fun f() { 9223372036854775808; return }",
        "fun f() { 9223372036854775808 + 1; return }",
        "fun f() { (9223372036854775808) }",
        "fun f() { if true { 9223372036854775808 } }",
        "fun f() { (if true { 9223372036854775808 }) }",
        "fun f() { while true { 9223372036854775808; break } }",
        "fun f() { for x in [1] { 9223372036854775808 } }",
        "fun f() { loop { 9223372036854775808; break } }",
        "fun f() { defer { 9223372036854775808 } }",
        "fun f() { unsafe { 9223372036854775808 } }",
        "fun f() { match true { true -> 9223372036854775808 } }",
        "let x: UInt64 = if true { 9223372036854775808; 18446744073709551615 } else { 0 }"
    }) {
        Frontend invalid;
        invalid.check(source, {DiagCode::ScalarLiteralRange});
    }
}

void logical_operands_with_deferred_calls() {
    for (std::string_view expression : {"1 && opaque()", "opaque() && 1", "1 || opaque()", "opaque() || 1"}) {
        Frontend invalid;
        invalid.check(std::format("fun opaque<T>(): Bool = true\nfun f() {{ if {} {{}} }}", expression),
                      {DiagCode::ScalarTypeMismatch});
    }
    Frontend valid;
    valid.check(R"(
fun opaque<T>(): Bool = true
let left = true && opaque()
let right = opaque() || false
fun conditions() { if left {}; if right {} }
)");
    CHECK(valid.binding("left") == Type::Deferred && valid.binding("right") == Type::Deferred);
}

void numeric_operands_with_deferred_calls() {
    const auto reject = [](std::string_view expr) {
        Frontend invalid;
        invalid.check(std::format("fun opaque<T>(): Int = 1\nlet broken = {}\nlet dependent = broken + opaque()", expr),
                      {DiagCode::InvalidScalarOperand});
        CHECK(invalid.binding("broken") == Type::Error && invalid.binding("dependent") == Type::Error);
    };
    for (std::string_view op : {"+", "-", "*", "/"}) {
        for (std::string_view bad : {"true", "'x'", "\"text\"", "()"}) {
            reject(std::format("{} {} opaque()", bad, op));
            reject(std::format("opaque() {} {}", op, bad));
        }
    }
    for (std::string_view op : {"%", "&+", "&-", "&*", "&", "|", "^", "<<", ">>"}) {
        reject(std::format("1.0 {} opaque()", op));
        reject(std::format("opaque() {} 1.0", op));
    }
    for (std::string_view op : {"<", "<=", ">", ">="}) {
        reject(std::format("true {} opaque()", op));
        reject(std::format("opaque() {} true", op));
    }
    Frontend typed;
    typed.check("fun opaque<T>(): Int = 1\nlet f: Float32 = 1.0\nlet bad = f & opaque()",
                {DiagCode::InvalidScalarOperand});

    for (std::string_view op : {"+=", "-=", "*=", "/="}) {
        Frontend invalid;
        invalid.check(std::format("fun opaque<T>(): Int = 1\nfun f() {{ var flag = true; flag {} opaque() }}", op),
                      {DiagCode::InvalidScalarOperand});
    }
    for (std::string_view op : {"%=", "&=", "|=", "^=", "<<=", ">>="}) {
        Frontend invalid;
        invalid.check(std::format("fun opaque<T>(): Int = 1\nfun f() {{ var number = 1.0; number {} opaque() }}", op),
                      {DiagCode::InvalidScalarOperand});
    }
    for (std::string_view assignment : {"target += true", "target &= 1.0"}) {
        Frontend invalid;
        invalid.check(std::format("fun f<T>(value: T) {{ var target = value; {} }}", assignment),
                      {DiagCode::InvalidScalarOperand});
    }

    Frontend valid;
    valid.check(R"(
fun opaque<T>(): Int = 1
fun floating<T>(): Float64 = 1.0
fun character<T>(): Char = 'a'
fun boolean<T>(): Bool = true
let sum = 1 + opaque()
let product = floating() * 1.0
let bits = opaque() & 1
let ordered = 'a' < character()
let equal = true == boolean()
fun assignments() { var count = 1; count += opaque(); var flag = true; flag = boolean() }
)");
    for (std::string_view name : {"sum", "product", "bits", "ordered", "equal"})
        CHECK(valid.binding(name) == Type::Deferred);
}

void primary_constructor_field_types() {
    Frontend valid;
    valid.check(R"(
struct S(sx: Int8) { fun read(): Int8 = sx }
class C(let cx: Int16 = 1, var flag: Bool = true, temporary: UInt16 = 2) {
    init { cx; flag; temporary }
    fun read(): Int16 = cx
    mut fun update() { flag = false }
}
actor A(let ax: Float32 = 1.0) { fun read(): Float32 = ax }
)");
    const auto bindings = valid.result.resolution->bindings();
    const auto check_pair = [&](std::string_view name, Type expected) {
        std::vector<BindingId> ids;
        for (usize i = 0; i < bindings.size(); ++i)
            if (valid.session.names().str(bindings[i].name.sym) == name) ids.push_back(BindingId{i});
        CHECK(ids.size() == 2 && ids[0] != ids[1]);
        const auto& parameter = valid.result.resolution->binding(ids[0]);
        const auto& field = valid.result.resolution->binding(ids[1]);
        CHECK(parameter.kind == BindingKind::Parameter && field.kind == BindingKind::Field);
        CHECK(parameter.scope != field.scope && parameter.name.span.lo == field.name.span.lo);
        CHECK(valid.result.scalar_types->binding(ids[0]) == expected);
        CHECK(valid.result.scalar_types->binding(ids[1]) == expected);
    };
    check_pair("sx", Type::Int8);
    check_pair("cx", Type::Int16);
    check_pair("flag", Type::Bool);
    check_pair("ax", Type::Float32);
    CHECK(valid.binding("temporary") == Type::UInt16);

    for (std::string_view source : {
        "struct S(x: Int8) { fun f(): Bool = x }",
        "class C(let x: Int8) { fun f(): Bool = x }",
        "class C(var x: Int8) { mut fun f() { x = false } }",
        "actor A(let x: Int8) { fun f(): Bool = x }",
        "actor A(var x: Int8) { mut fun f() { x = false } }"
    }) {
        Frontend invalid;
        invalid.check(source, {DiagCode::ScalarTypeMismatch});
    }
    Frontend invalid_default;
    invalid_default.check("class C(let x: Int8 = 128) {}", {DiagCode::ScalarLiteralRange});
    Frontend invalid_operator;
    invalid_operator.check("struct S(flag: Bool) { fun f() = flag + 1 }", {DiagCode::InvalidScalarOperand});

    Frontend shadowed;
    shadowed.check(R"(
struct S(value: Int8) { fun f(value: Bool): Bool = value }
struct Generic<T>(value: T) { fun f(): T = value }
)");
    CHECK(shadowed.binding("value", 0) == Type::Int8 && shadowed.binding("value", 1) == Type::Int8);
    CHECK(shadowed.binding("value", 2).kind == Type::TypeParameter && shadowed.binding("value", 3) == shadowed.binding("value", 2));
    CHECK(shadowed.binding("value", 4) == Type::Bool);
}

void session_gating_and_lifetimes() {
    FrontendSession session;
    const auto invalid = session.sources().add("invalid", "let x = unknown");
    CHECK(invalid && !session.check(*invalid).scalar_types);
    const auto source = session.sources().add("types", "let x: Bool = 1");
    CHECK(source);
    CHECK(!session.diagnostics().has_errors(*source) && session.parse(*source));
    const auto result = session.check(*source);
    CHECK(result.scalar_types && session.diagnostics().error_count(*source) == 1);
    const auto* expr = ast::cast<ast::VarItem>(result.syntax->items.front())->init;
    CHECK(result.scalar_types->expression(expr) == Type::Int);
    for (int i = 0; i < 128; ++i) {
        const auto file = session.sources().add("other", "let x = false");
        CHECK(file && session.check(*file).scalar_types && !session.diagnostics().has_errors(*file));
    }
    CHECK(session.check(*source).scalar_types == result.scalar_types);
    CHECK(result.scalar_types->expression(expr) == Type::Int);
    CHECK(session.diagnostics().error_count(*source) == 1);
}

void long_trees_and_forward_chains() {
    Frontend f;
    std::string source = "let sum: Int8 = 1";
    for (int i = 0; i < 20000; ++i) source += "+1";
    for (int i = 0; i < 10000; ++i) source += std::format("\nlet n{} = n{}", i, i + 1);
    source += "\nlet n10000: Int8 = 1";
    f.check(source, {}, false);
    CHECK(f.binding("sum") == Type::Int8 && f.binding("n0") == Type::Int8);
}
}  // namespace

void optionals() {
    // `if let` and `while let` bind the value inside an optional, `??`
    // falls back to a value or another optional of the same type, and
    // equality compares optionals with `none`, values and each other.
    Frontend f;
    f.check(R"(
fun find(n: Int): Int8? {
    if n == 0 { return none }
    return 1
}
fun use(n: Int): Int8 {
    let found = find(n)
    var total: Int8 = 0
    if let inner = found { total += inner }
    while let next = find(n) { total += next; break }
    if let annotated: Int8 = found { total += annotated }
    let fallback = found ?? 2
    let chained = found ?? find(1)
    let both = found ?? find(1) ?? 3
    let none1 = found == none
    let none2 = none != found
    let value = found == 1
    let pair = found == find(2)
    let text: String? = "t"
    let strings = text == "t"
    let deep: Int8? ? = found
    let deepValue = deep ?? none
    let parenthesized = found == (none)
    let kept = found ?? none
    let branched = found ?? if n == 1 { find(2) } else { none }
    let branchedInner = found ?? if n == 1 { 1 } else { 2 }
    if let picked: Int8 = if n == 1 { found } else { find(2) } { total += picked }
    return total + fallback + both
}
)");
    CHECK(f.binding("inner") == Type::Int8 && f.binding("next") == Type::Int8 && f.binding("annotated") == Type::Int8);
    CHECK(f.binding("fallback") == Type::Int8 && f.binding("chained") == Type{Type::Int8}.optional());
    CHECK(f.binding("both") == Type::Int8);
    for (const char* name : {"none1", "none2", "value", "pair", "strings", "parenthesized"}) CHECK(f.binding(name) == Type::Bool);
    CHECK(f.binding("deepValue") == Type{Type::Int8}.optional() && f.binding("kept") == Type{Type::Int8}.optional());
    CHECK(f.binding("branched") == Type{Type::Int8}.optional() && f.binding("branchedInner") == Type::Int8);
    CHECK(f.binding("picked") == Type::Int8);
    const auto* use = ast::cast<ast::FunItem>(f.result.syntax->items[1]);
    // `none` takes the optional type it is compared with.
    const auto* none1 = ast::cast<ast::VarStmt>(ast::cast<ast::BlockExpr>(use->body)->stmts[8]);
    CHECK(f.result.scalar_types->expression(ast::cast<ast::BinaryExpr>(none1->init)->rhs) == Type{Type::Int8}.optional());

    // Only optionals unwrap or fall back.
    Frontend errors;
    errors.check(R"(
fun bad(i: Int, o: Int?, flag: Bool, s: String?) {
    if let x = i {}
    if let y = none {}
    while let z = 1 { break }
    let a = i ?? 1
    let b = none ?? 1
    let c = o ?? "s"
    let d = o == true
    if let e: Bool = o {}
    if let g: Int = if flag { s } else { o } {}
}
)", {DiagCode::NotOptional, DiagCode::NotOptional, DiagCode::NotOptional, DiagCode::NotOptional,
      DiagCode::NotOptional, DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch,
      DiagCode::ScalarTypeMismatch});
    CHECK(errors.binding("x") == Type::Error && errors.binding("a") == Type::Error);

    // Composite optionals still defer their equality.
    Frontend composite;
    composite.check("fun pairs(a: (Int8, Int8)?, b: (Int8, Int8)?) {\n    let same = a == b\n    let literal = a == (1, 2)\n}\n");
    CHECK(composite.binding("same") == Type::Deferred && composite.binding("literal") == Type::Deferred);
}

// Enum cases (3.34): qualified and contextual cases, payload arguments, and
// equality of enums without payloads.
void enum_cases() {
    Frontend f;
    f.check(R"(
enum Tok {
    num(Int8)
    pair(a: Int, b: Bool)
    plus
}
enum Dir { up, down }
fun take(t: Tok?): Int = 0
fun pick(flag: Bool): Tok {
    if flag { return .plus }
    return .num(7)
}
let qualified = Tok.plus
let built = Tok.num(3)
let labeled = Tok.pair(b: true, a: 1)
let contextual: Tok = .num(4)
let optional: Tok? = .plus
let argument = take(.pair(1, b: false))
let branches: Tok = if true { .plus } else { Tok.num(1) }
let list: [Tok] = [.plus, .num(2)]
let dir = Dir.up
let same = dir == .down
let differ = Dir.up != dir
let payloads = qualified == .plus
)");
    const auto tok = f.binding("qualified");
    CHECK(tok.kind == Type::Nominal && !tok.optional_depth);
    CHECK(f.binding("built") == tok && f.binding("labeled") == tok && f.binding("contextual") == tok);
    CHECK(f.binding("optional") == tok.optional() && f.binding("argument") == Type::Int);
    CHECK(f.binding("branches") == tok && f.binding("t") == tok.optional());
    CHECK(f.binding("same") == Type::Bool && f.binding("differ") == Type::Bool);
    // Enums with payloads defer their equality, like other composites.
    CHECK(f.binding("payloads") == Type::Deferred);

    // The call records the case and maps the payload to its fields.
    const auto* labeled = ast::cast<ast::VarItem>(f.result.syntax->items[6]);
    const auto* call = f.result.scalar_types->call(ast::cast<ast::CallExpr>(labeled->init));
    CHECK(call && call->kind == CallKind::EnumCase && call->state == CallState::Resolved);
    CHECK(f.session.names().str(f.result.resolution->binding(call->target).name.sym) == "pair");
    CHECK((call->parameters == std::vector<usize>{1, 0}));
    CHECK((call->parameter_types == std::vector<Type>{Type::Int, Type::Bool}));
    // `.num(4)` resolves its case through the expected enum.
    const auto* contextual = ast::cast<ast::CallExpr>(ast::cast<ast::VarItem>(f.result.syntax->items[7])->init);
    CHECK(f.result.scalar_types->call(contextual)->kind == CallKind::EnumCase);
    CHECK((f.result.scalar_types->call(contextual)->parameter_types == std::vector<Type>{Type::Int8}));
    const auto* member = f.result.scalar_types->member(contextual->callee);
    CHECK(member && member->state == ResolutionState::Resolved && member->candidates.size() == 1);
    CHECK(f.result.scalar_types->expression(contextual) == tok);

    Frontend errors;
    errors.check(R"(
enum Tok {
    num(Int8)
    plus
}
enum Other { plus }
let wrongType: Int = Tok.plus
let payloadType = Tok.num(true)
let arity = Tok.num(1, 2)
let label = Tok.num(x: 1)
let range = Tok.num(300)
let unknown: Tok = .nope
let bare = Tok.num
let called = Tok.plus()
let implicitBare: Tok = .num
let implicitCalled: Tok = .plus(1)
let notEnum: Int = .plus
let mixed = Tok.plus == Other.plus
)", {DiagCode::ScalarTypeMismatch, DiagCode::NoMatchingCall, DiagCode::NoMatchingCall, DiagCode::NoMatchingCall,
      DiagCode::ScalarLiteralRange, DiagCode::UnknownMember, DiagCode::CasePayload, DiagCode::CasePayload,
      DiagCode::CasePayload, DiagCode::CasePayload, DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch});
    CHECK(errors.binding("unknown") != Type::Deferred);
    CHECK(errors.binding("mixed") == Type::Error);

    // Generic enums are checked later; their cases stay deferred.
    Frontend generic;
    generic.check("enum Opt<T> { some(T), nothing }\nlet a = Opt<Int>.some(1)\nlet b: Opt<Int> = .nothing\n");
    CHECK(generic.binding("a") == Type::Deferred);
}

// Match patterns (3.35): bindings take payload field types, through nested
// cases and alternatives, and case patterns record the case they match.
void match_patterns() {
    Frontend f;
    f.check(R"(
enum Inner { a(Int8), b }
enum Outer {
    wrap(Inner)
    pair(left: Bool, right: UInt)
    text(String)
}
fun eval(o: Outer, n: Int?): Int {
    let first: Int = match o {
        .wrap(.a(small)) if small > 0 -> 1
        Outer.pair(flag, count) -> 2
        .text(words) | .text(words) -> 3
        .wrap(Inner.b) -> 4
        whole -> 5
    }
    let second: Int = match n {
        none -> 0
        3 -> 3
        value -> 1
    }
    return first + second
}
)");
    CHECK(f.binding("small") == Type::Int8 && f.binding("flag") == Type::Bool && f.binding("count") == Type::UInt);
    CHECK(f.binding("words") == Type::String && f.binding("value") == Type{Type::Int}.optional());
    CHECK(f.binding("whole").kind == Type::Nominal);
    const auto* fun = ast::cast<ast::FunItem>(f.result.syntax->items[2]);
    const auto* var = ast::cast<ast::VarStmt>(ast::cast<ast::BlockExpr>(fun->body)->stmts[0]);
    const auto* match = ast::cast<ast::MatchExpr>(var->init);
    const auto* wrap = ast::cast<ast::CasePat>(match->arms[0].pattern);
    const auto id = f.result.scalar_types->pattern_case(wrap);
    CHECK(id.valid() && f.session.names().str(f.result.resolution->binding(id).name.sym) == "wrap");
    const auto inner = f.result.scalar_types->pattern_case(ast::cast<ast::CasePat>(wrap->args[0]));
    CHECK(inner.valid() && f.session.names().str(f.result.resolution->binding(inner).name.sym) == "a");

    Frontend errors;
    errors.check(R"(
enum Tok {
    num(Int)
    pair(a: Int, b: Bool)
    plus
}
enum Other { plus }
fun bad(t: Tok, i: Int) {
    let a: Int = match t {
        .nope -> 1
        .plus(x) -> 2
        .num -> 3
        .pair(x) -> 4
        Other.plus -> 5
        3 -> 6
        .num(true) -> 7
        .pair(_, n) | .num(n) -> 8
        _ -> 0
    }
    let b: Int = match i {
        .plus -> 1
        _ -> 0
    }
}
)", {DiagCode::UnknownMember, DiagCode::CasePayload, DiagCode::CasePayload, DiagCode::CasePayload,
      DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch,
      DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch});
    // A failed case pattern gives its bindings the error type.
    CHECK(errors.binding("x") == Type::Error);
}

// Coverage stays linear in the arms of wide matches, rejects one too large
// to check, and the size check walks long chains of types once.
void wide_matches_and_long_chains() {
    constexpr int kWide = 10000;
    std::string wide = "enum E {\n";
    for (int i = 0; i < kWide; ++i) wide += "    c" + std::to_string(i) + "(Int)\n";
    wide += "}\nfun f(e: E, n: Int): Int {\n    let a = match e {\n";
    for (int i = 0; i < kWide; ++i) wide += "        .c" + std::to_string(i) + "(x) -> x\n";
    wide += "    }\n    let b = match n {\n";
    for (int i = 0; i < kWide; ++i) wide += "        " + std::to_string(i) + " -> 1\n";
    wide += "        _ -> 0\n    }\n    return a + b\n}\n";
    Frontend f;
    f.check(wide, {}, false);

    // Each case meets every guarded `_` arm before it.
    std::string complex = "enum E {\n";
    for (int i = 0; i < 2000; ++i) complex += "    c" + std::to_string(i) + "(Int)\n";
    complex += "}\nfun f(e: E, g: Bool): Int {\n    return match e {\n";
    for (int i = 0; i < 1100; ++i) complex += "        _ if g -> 0\n";
    for (int i = 0; i < 2000; ++i) complex += "        .c" + std::to_string(i) + "(x) -> x\n";
    complex += "    }\n}\n";
    Frontend too_complex;
    too_complex.check(complex, {DiagCode::MatchTooComplex}, false);

    // Matching a present value through many optional layers is too deep.
    std::string optional = "fun f(x: Bool";
    for (int i = 0; i < 1000; ++i) optional += "? ";
    optional += "): Int {\n    return match x { true -> 1, _ -> 0 }\n}\n";
    Frontend deep;
    deep.check(optional, {DiagCode::MatchTooComplex}, false);

    // Only the two structs on the cycle at the end are reported.
    std::string chain;
    for (int i = 0; i < kWide - 1; ++i) chain += "struct S" + std::to_string(i) + "(a: S" + std::to_string(i + 1) + ", b: Int)\n";
    chain += "struct S" + std::to_string(kWide - 1) + "(a: S" + std::to_string(kWide - 2) + "?)\n";
    Frontend sizes;
    sizes.check(chain, {DiagCode::InfiniteSize, DiagCode::InfiniteSize}, false);
}

int main() {
    retained_inference_and_context();
    diagnostics_and_suppression();
    integer_and_float_boundaries();
    operators();
    deferred_types_and_shadowing();
    discarded_numeric_expressions();
    logical_operands_with_deferred_calls();
    numeric_operands_with_deferred_calls();
    primary_constructor_field_types();
    session_gating_and_lifetimes();
    long_trees_and_forward_chains();
    optionals();
    enum_cases();
    match_patterns();
    wide_matches_and_long_chains();
}
