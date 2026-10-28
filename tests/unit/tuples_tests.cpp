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
#include <utility>
#include <vector>

#include "frontend/session.hpp"
#include "syntax/ast_dump.hpp"
#include "syntax/lexer.hpp"

using namespace phs;
using Type = ScalarType;

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};
    void check(std::string_view source, std::initializer_list<DiagCode> expected = {}) {
        const auto id = session.sources().add("tuples.phs", source); CHECK(id);
        const auto* syntax = session.parse(*id);
        if (session.diagnostics().has_errors(*id)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump_ast(*syntax, session.names());
        result = session.check(*id);
        CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual); std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
    }
    BindingId id(std::string_view name, BindingKind kind = BindingKind::Variable) const {
        for (usize i = 0; i < result.resolution->bindings().size(); ++i) {
            const auto& b = result.resolution->bindings()[i];
            if (b.kind == kind && session.names().str(b.name.sym) == name) return BindingId{i};
        }
        CHECK(false); return {};
    }
    Type value(std::string_view name, BindingKind kind = BindingKind::Variable) const {
        return result.scalar_types->binding(id(name, kind));
    }
    Type local(std::string_view name) const { return value(name, BindingKind::Local); }
    const ast::Expr* init(std::string_view name) const {
        return ast::cast<ast::VarItem>(result.resolution->binding(id(name)).item)->init;
    }
    Type expression(const ast::Expr* expr) const { return result.scalar_types->expression(expr); }
    std::string name(Type type) const { return result.scalar_types->type_name(type, *result.resolution, session.names()); }
};

// Digits after '.' are tuple indexes, even after a line break.
void tuple_index_tokens() {
    FrontendSession session;
    const auto id = session.sources().add("index.phs", "t.0.1 + 0.5 + t.\n2.3");
    CHECK(id);
    std::vector<Tok> kinds;
    for (const auto& token : lex_file(session.sources(), *id, session.names(), session.diagnostics())) kinds.push_back(token.kind);
    CHECK(!session.diagnostics().has_errors(*id));
    CHECK((kinds == std::vector{Tok::Ident, Tok::Dot, Tok::IntLit, Tok::Dot, Tok::IntLit, Tok::Plus, Tok::FloatLit, Tok::Plus,
                                Tok::Ident, Tok::Dot, Tok::Newline, Tok::IntLit, Tok::Dot, Tok::IntLit, Tok::Eof}));
}

void descriptors_and_defaults() {
    Frontend f;
    f.check(R"(
type Pair = (Int8, Int8)
let defaults = (1, 2.5, (true, 'c'))
let typed: ((Int8, Bool), [UInt8]) = ((1, true), [2])
let optional: (Int, Int)? = (1, 2)
let alias: Pair = (3, 4)
let same: (Int8, Int8) = alias
let grouped: (Int) = 5
let first = typed.0.0
let nested = typed.0.1
let indexed = typed.1[0]
let literal = (1, 2).1
let parenthesized: (Int8, Int8) = ((1, 2))
)");
    CHECK(f.name(f.value("defaults")) == "(Int, Float64, (Bool, Char))");
    const auto typed = f.value("typed");
    CHECK(typed.kind == Type::Tuple && f.name(typed) == "((Int8, Bool), [UInt8])");
    const auto* descriptor = f.result.scalar_types->descriptor(typed);
    CHECK(descriptor && descriptor->parameters.size() == 2 && descriptor->parameters[1].kind == Type::Array);
    // Literal elements take their types from the expected tuple type.
    const auto* literal = ast::cast<ast::TupleExpr>(f.init("typed"));
    CHECK(f.expression(literal) == typed.scalar());
    CHECK(f.expression(ast::cast<ast::TupleExpr>(literal->elems[0])->elems[0]) == Type::Int8);
    CHECK(f.name(f.value("optional")) == "(Int, Int)?" && f.name(f.expression(f.init("optional"))) == "(Int, Int)");
    CHECK(f.value("alias") == f.value("same") && f.name(f.value("alias")) == "(Int8, Int8)");
    CHECK(f.value("grouped") == Type::Int);
    CHECK(f.value("first") == Type::Int8 && f.value("nested") == Type::Bool && f.value("indexed") == Type::UInt8);
    CHECK(f.value("literal") == Type::Int && f.value("parenthesized") == f.value("alias"));
}

void destructuring() {
    Frontend f;
    f.check(R"(
fun pair(): (Int8, Bool) = (1, true)
fun main() {
    let (x, y) = pair()
    var (u, v): (UInt, Int16) = (1, 2)
    let (p, q) = (1, (2, 3))
    u += 1
}
)");
    CHECK(f.local("x") == Type::Int8 && f.local("y") == Type::Bool);
    CHECK(f.local("u") == Type::UInt && f.local("v") == Type::Int16);
    CHECK(f.local("p") == Type::Int && f.name(f.local("q")) == "(Int, Int)");
}

void calls_and_generics() {
    Frontend f;
    f.check(R"(
fun take(p: (Int8, UInt)): Int8 = p.0
fun over(p: (Int, Int)): Int8 = 1
fun over(p: (Bool, Int)): Bool = true
fun first<T>(p: (T, Int)): T = p.0
fun swap<A, B>(p: (A, B)): (B, A) = (p.1, p.0)
let taken = take((1, 2))
let byInt = over((1, 2))
let byBool = over((true, 2))
let generic = first((true, 1))
let swapped = swap((1, true))
let array: [(Int8, Int8)] = [(1, 2), (3, 4)]
let inferred = [(1, 2)]
fun same<T>(p: (T, T)): T = p.0
let byte: Int8 = 1
let anchoredFirst = same((byte, 2))
let anchoredLast = same((2, byte))
let nestedAnchor = same(((byte), 2))
let ordered = (1, 2) < (3, 4)
)");
    CHECK(f.value("anchoredFirst") == Type::Int8 && f.value("anchoredLast") == Type::Int8);
    CHECK(f.value("nestedAnchor") == Type::Int8);
    CHECK(f.value("taken") == Type::Int8 && f.value("byInt") == Type::Int8 && f.value("byBool") == Type::Bool);
    CHECK(f.value("generic") == Type::Bool && f.name(f.value("swapped")) == "(Bool, Int)");
    CHECK(f.name(f.value("array")) == "[(Int8, Int8)]" && f.name(f.value("inferred")) == "[(Int, Int)]");
}

void mismatches() {
    for (const auto& test : std::vector<std::pair<std::string_view, DiagCode>>{
        {"let t = (1, 2)\nlet x: Bool = t.0", DiagCode::ScalarTypeMismatch},
        {"let t = (1, 2)\nlet x = t.2", DiagCode::UnknownMember},
        {"let x = (1, 2).4294967295", DiagCode::UnknownMember},
        {"let x: (Int8, Int8) = (1, 300)", DiagCode::ScalarLiteralRange},
        {"let x: (Int, Int) = (1, 2, 3)", DiagCode::ScalarTypeMismatch},
        {"let t = (1, 2)\nlet x: (Int8, Int8) = t", DiagCode::ScalarTypeMismatch},
        {"let x = 5\nlet y = x.0", DiagCode::InvalidMemberReceiver},
        {"let y = (1).0", DiagCode::InvalidMemberReceiver},
        {"let y = (1.5).0", DiagCode::InvalidMemberReceiver},
        {"let x = (1, 2) - (3, 4)", DiagCode::InvalidScalarOperand},
        {"let t: (Int, Int)? = (1, 2)\nlet y = t.0", DiagCode::InvalidMemberReceiver},
        {"let x: Int = (1, 2)", DiagCode::ScalarTypeMismatch},
        {"fun f() { let (a, b) = 5 }", DiagCode::ScalarTypeMismatch},
        {"fun f() { let (a, b) = (1, 2, 3) }", DiagCode::ScalarTypeMismatch},
        {"fun f() { let (a, b): (Int, Int)? = (1, 2) }", DiagCode::ScalarTypeMismatch},
        {"fun f(x: (Int8, Int8)) {}\nlet bad = f((1, 2, 3))", DiagCode::NoMatchingCall},
        {"fun f(x: (Int8, Int8)) {}\nlet bad = f((1, 300))", DiagCode::ScalarLiteralRange},
        {"let x = (1, 2) + (3, 4)", DiagCode::InvalidScalarOperand},
        {"fun f() { let (a, a) = (1, 2) }", DiagCode::DuplicateBinding},
        {"fun f() {\n    let t = (1, 2)\n    t.0 = 3\n}", DiagCode::ImmutableAssignment},
        {"fun f() {\n    let (a, b) = (1, 2)\n    b = 3\n}", DiagCode::ImmutableAssignment},
        {"fun f(t: (Int, Int)) { t.1 = 3 }", DiagCode::ImmutableAssignment},
    }) { Frontend bad; bad.check(test.first, {test.second}); }
}
}  // namespace

int main() {
    tuple_index_tokens();
    descriptors_and_defaults();
    destructuring();
    calls_and_generics();
    mismatches();
}
