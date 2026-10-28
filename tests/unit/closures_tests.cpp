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

using namespace phs;
using Type = ScalarType;

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};

    void check(std::string_view source, std::initializer_list<DiagCode> expected = {}) {
        const auto id = session.sources().add("closures.phs", source);
        CHECK(id);
        result = session.check(*id);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
    }
    BindingId binding(std::string_view name, BindingKind kind = BindingKind::Local, usize occurrence = 0) const {
        for (usize i = 0; i < result.resolution->bindings().size(); ++i) {
            const auto& b = result.resolution->bindings()[i];
            if (b.kind == kind && session.names().str(b.name.sym) == name && !occurrence--) return BindingId{i};
        }
        CHECK(false);
        return {};
    }
    std::string type(BindingId id) const {
        return result.scalar_types->type_name(result.scalar_types->binding(id), *result.resolution, session.names());
    }
    std::string type(std::string_view name, BindingKind kind = BindingKind::Local, usize occurrence = 0) const {
        return type(binding(name, kind, occurrence));
    }
    // The closure that initializes a local, through any parentheses.
    const ast::ClosureExpr* closure(std::string_view local) const {
        const ast::Expr* init = nullptr;
        const auto id = binding(local);
        // Locals have no item; find the declaration by its name span.
        for_each_var([&](const ast::VarStmt& var) {
            if (var.name.span.lo == result.resolution->binding(id).name.span.lo) init = var.init;
        });
        while (const auto* paren = ast::as<ast::ParenExpr>(init)) init = paren->inner;
        CHECK(init && init->kind == ast::ExprKind::Closure);
        return ast::cast<ast::ClosureExpr>(init);
    }
    std::vector<std::string> captures(const ast::ClosureExpr* closure) const {
        std::vector<std::string> names;
        for (const auto& capture : result.resolution->captures(closure))
            names.emplace_back(session.names().str(result.resolution->binding(capture.binding).name.sym));
        return names;
    }
    template <typename F>
    void for_each_var(F&& visit) const {
        for (const auto* item : result.syntax->items) {
            const auto* fun = ast::as<ast::FunItem>(item);
            if (!fun || !fun->body || fun->expr_body) continue;
            for (const auto* stmt : ast::cast<ast::BlockExpr>(fun->body)->stmts)
                if (const auto* var = ast::as<ast::VarStmt>(stmt)) visit(*var);
        }
    }
};

void parameter_and_result_types() {
    Frontend f;
    f.check(R"(
fun apply(f: fun(Int) -> Int, x: Int): Int = f(x)
fun makeAdder(n: Int): fun(Int) -> Int = { add -> add + n }
struct Handler(run: fun(Int8) -> Int8)
fun main() {
    let double = { a: Int -> a * 2 }
    let zero = { -> 0 }
    let flag = { -> true }
    let wide: fun(Int64, Int64) -> Int64 = { b, c -> b + c + 1 }
    var current: fun(UInt) -> UInt = { d -> d }
    current = { e -> e + 1 }
    let handler = Handler(run: { g -> g })
    let applied = apply({ h -> h * 2 }, 1)
    let curried: fun(Int) -> fun(Int16) -> Int16 = { i -> { j -> j } }
    let early = { k: Int ->
        if k < 0 {
            return 0
        }
        k + 1
    }
    let returns = { -> return 5 }
    let optional: (fun(Bool) -> Bool)? = { m -> m }
}
)");
    using enum BindingKind;
    CHECK(f.type("double") == "fun(Int) -> Int" && f.type("zero") == "fun() -> Int" && f.type("flag") == "fun() -> Bool");
    CHECK(f.type("wide") == "fun(Int64, Int64) -> Int64" && f.type("b", Parameter) == "Int64" && f.type("c", Parameter) == "Int64");
    CHECK(f.type("d", Parameter) == "UInt" && f.type("e", Parameter) == "UInt");
    CHECK(f.type("g", Parameter) == "Int8" && f.type("h", Parameter) == "Int" && f.type("applied") == "Int");
    CHECK(f.type("i", Parameter) == "Int" && f.type("j", Parameter) == "Int16");
    CHECK(f.type("add", Parameter) == "Int" && f.type("early") == "fun(Int) -> Int");
    CHECK(f.type("returns") == "fun() -> Int" && f.type("m", Parameter) == "Bool");
    CHECK(f.type("optional") == "(fun(Bool) -> Bool)?");
    // A literal in the body takes the expected result type.
    const auto* wide = f.closure("wide");
    const auto* tail = ast::cast<ast::ExprStmt>(wide->body->stmts.back())->expr;
    CHECK(f.result.scalar_types->expression(ast::cast<ast::BinaryExpr>(tail)->rhs) == Type::Int64);
    // No local or parameter is left without a type.
    for (usize i = 0; i < f.result.resolution->bindings().size(); ++i) {
        const auto kind = f.result.resolution->bindings()[i].kind;
        if (kind == Local || kind == Parameter) {
            const auto type = f.result.scalar_types->binding(BindingId{i});
            CHECK(type.kind != Type::Deferred && type.kind != Type::Error);
        }
    }
}

void captures() {
    Frontend f;
    f.check(R"(
fun main(limit: Int) {
    let base = 1
    var seen = 2
    let alone = { x: Int -> x + 1 }
    let reads = { -> base + limit + base }
    let nested = { -> { y: Int -> y + seen + base } }
    let shadowed = { base: Int -> base }
    let local = { -> 
        let inner = 3
        { -> inner + limit }
    }
    let pair = (1, 2)
    let (left, right) = pair
    let destructured = { -> right + left }
}
)");
    CHECK(f.captures(f.closure("alone")).empty());
    CHECK(f.captures(f.closure("reads")) == std::vector<std::string>({"base", "limit"}));
    // A nested closure's captures pass through the closure around it.
    const auto* nested = f.closure("nested");
    CHECK(f.captures(nested) == std::vector<std::string>({"seen", "base"}));
    const auto* inner = ast::cast<ast::ClosureExpr>(ast::cast<ast::ExprStmt>(nested->body->stmts.back())->expr);
    CHECK(f.captures(inner) == std::vector<std::string>({"seen", "base"}));
    CHECK(f.captures(f.closure("shadowed")).empty());
    const auto* local = f.closure("local");
    CHECK(f.captures(local) == std::vector<std::string>({"limit"}));
    const auto* returned = ast::cast<ast::ClosureExpr>(ast::cast<ast::ExprStmt>(local->body->stmts.back())->expr);
    CHECK(f.captures(returned) == std::vector<std::string>({"inner", "limit"}));
    CHECK(f.captures(f.closure("destructured")) == std::vector<std::string>({"right", "left"}));
    // Each capture remembers its first use.
    const auto first = f.result.resolution->captures(f.closure("reads")).front();
    CHECK(first.use.lo < f.result.resolution->captures(f.closure("reads"))[1].use.lo);
}

void errors() {
    for (const auto& test : std::vector<std::pair<std::string_view, std::initializer_list<DiagCode>>>{
        {"fun main() { let f = { x -> x } }", {DiagCode::ClosureInference}},
        {"fun main() { let f: Int = { -> 1 } }", {DiagCode::ScalarTypeMismatch}},
        {"fun main() { let f: fun() -> Int = { x -> 1 } }", {DiagCode::ScalarTypeMismatch}},
        {"fun main() { let f: fun(Int) -> Int = { x: UInt -> 1 } }", {DiagCode::ScalarTypeMismatch}},
        {"fun main() { let f = { x: Int -> if x > 0 { return 'a' }; 1 } }", {DiagCode::ScalarTypeMismatch}},
        {"fun main() { let f: fun() -> Int = { -> true } }", {DiagCode::ScalarTypeMismatch}},
        {"fun main() { let f: fun() -> Int8 = { -> 300 } }", {DiagCode::ScalarLiteralRange}},
        {"fun take(f: fun(Int) -> Int) {}\nfun take(f: fun(UInt) -> Int) {}\nfun main() { take({ x -> 1 }) }",
         {DiagCode::ClosureInference}},
        {"fun main(n: inout Int) { let f = { -> n } }", {DiagCode::InvalidCapture}},
        {"fun main(s: String) { let f = { -> s } }", {DiagCode::InvalidCapture}},
        {"fun main(s: own String, t: (Int, Bool)) { let f = { -> s.clone() }\nlet g = { -> t } }", {}},
        {"fun main() { var n = 0\nlet f = { -> n = 1 } }", {DiagCode::ImmutableAssignment}},
        {"fun main() { let n: Int\nlet f = { -> n }\nn = 1 }", {DiagCode::UninitializedUse}},
        {"fun main() { let f = { x: Int -> x = 1 } }", {DiagCode::ImmutableAssignment}},
        // Closure parameters are borrowed too; a nested closure copies them only when they are Copy.
        {"fun main() { let f = { s: String -> { -> s } } }", {DiagCode::InvalidCapture}},
        // A closure's own locals are its own, and assignable.
        {"fun main() { let f = { -> var n = 0\nn = 1\nn } }", {}},
        {"fun main() { while true { let f = { -> break } } }", {DiagCode::InvalidControlFlow}},
        {"struct S(var v: Int) {\nmut fun f() { let g = { -> v } }\n}", {DiagCode::InvalidCapture}},
        {"struct S(var v: Int) {\nfun f() { let g = { -> v }\nlet h: fun() -> Unit = { -> self.v = 1 } }\n}",
         {DiagCode::ImmutableAssignment}},
        {"struct S(var v: String) {\nfun f() { let g = { -> v } }\n}", {DiagCode::InvalidCapture}},
        {"struct Box<T>(value: T) {\nfun f() { let g = { -> self.value } }\n}", {DiagCode::InvalidCapture}},
        // Instances substitute their arguments, also inside composite field types.
        {"struct PairBox<T>(value: (T, Int))\nfun main(b: PairBox<Int>, c: PairBox<String>) { let f = { -> b }\nlet g = { -> c } }",
         {DiagCode::InvalidCapture}},
        {"struct Box<T>(value: T)\nstruct Outer<U>(inner: Box<U>)\nfun main(o: Outer<Int>, p: Outer<String>) { let f = { -> o }\nlet g = { -> p } }",
         {DiagCode::InvalidCapture}},
        // Constructors and destructors capture `self` under the same rules.
        {"class C(var n: Int) {\ndeinit { let f = { -> self } }\n}", {DiagCode::InvalidCapture}},
        {"struct S(var n: Int) {\ninit(m: Int) { n = m\nlet f = { -> n } }\n}", {}},
        {"struct S(var n: Int) {\ninit(m: Int) { n = m\nlet f: fun() -> Unit = { -> self.n = 2 } }\n}", {DiagCode::ImmutableAssignment}},
        // Promoted constructor parameters are parameters in defaults and field initializers.
        {"class C(let text: String, let read: fun() -> String = { -> text })", {DiagCode::InvalidCapture}},
        {"class C(let count: Int, let read: fun() -> Int = { -> count })", {}},
        // So do instance field initializers.
        {"class C {\nvar n: Int = 0\nlet read: fun() -> Int = { -> n }\n}", {DiagCode::InvalidCapture}},
        {"struct S {\nvar n: Int = 0\nlet read: fun() -> Int = { -> n }\n}", {}},
        {"struct S {\nvar n: Int = 0\nlet set: fun() -> Unit = { -> n = 1 }\n}", {DiagCode::ImmutableAssignment}},
        // A direct call uses `self` only when the overload it selected is an instance method.
        {"class C {\nfun pick(x: Int) {}\nstatic fun pick(x: Bool) {}\nfun test() { let f = { -> pick(true) } }\n}", {}},
        {"class C {\nfun pick(x: Int) {}\nstatic fun pick(x: Bool) {}\nfun test() { let f = { -> pick(1) } }\n}",
         {DiagCode::InvalidCapture}},
        // References inside match arms are captures too.
        {"fun main(n: inout Int) { let g = { -> match 0 { _ -> n } } }", {DiagCode::InvalidCapture}},
        {"fun main() { let n: Int\nlet g = { -> match 0 { _ -> n } }\nn = 1 }", {DiagCode::UninitializedUse}},
        // A returned branch expression gives the inferred result too.
        {"fun main() { let f = { -> return if true { 1 } else { 2 } }\nlet n: Bool = f() }", {DiagCode::ScalarTypeMismatch}},
        {"fun main() { let f = { -> return if true { 1 } else { 'a' } } }", {DiagCode::ScalarTypeMismatch}},
        // When no candidate accepts the call's shape, the call reports it.
        {"fun take(f: fun(Int) -> Int, n: Int) {}\nfun main() { take({ x -> x }) }", {DiagCode::NoMatchingCall}},
        {"fun main(g: fun(fun(Int) -> Int, Int) -> Unit) { g({ x -> x }) }", {DiagCode::NoMatchingCall}},
        {"struct H(run: fun(Int) -> Int, n: Int)\nfun main() { let h = H(run: { x -> x }) }", {DiagCode::NoMatchingCall}},
        // A constructor named through an alias, and overloads filtered by shape, still give parameter types.
        {"struct Handler(run: fun(Int) -> Int)\ntype Alias = Handler\nfun main() { let h = Alias(run: { x -> x }) }", {}},
        {"fun take<T>(x: T, y: Int) {}\nfun take(f: fun(Int) -> Int) {}\nfun main() { take({ x -> x }) }", {}},
    }) { Frontend bad; bad.check(test.first, test.second); }
}
}  // namespace

int main() {
    parameter_and_result_types();
    captures();
    errors();
}
