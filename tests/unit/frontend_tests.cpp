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
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>

#include "frontend/session.hpp"
#include "syntax/ast_dump.hpp"
#include "syntax/lexer.hpp"
#include "syntax/parser.hpp"

using namespace phs;
using namespace phs::ast;

namespace {

template <typename T, typename B>
concept CanCast = requires(B* node) { as<T>(node); cast<T>(node); };

static_assert(CanCast<IntExpr, Expr> && CanCast<IntExpr, const Expr>);
static_assert(!CanCast<IntExpr, Item> && !CanCast<VarStmt, Expr>);
static_assert(!CanCast<IntExpr, volatile Expr> && !CanCast<Expr, Expr>);
static_assert(!CanCast<TypeDeclItem, Item> && !CanCast<const IntExpr, Expr>);
static_assert(std::same_as<decltype(as<IntExpr>(static_cast<const Expr*>(nullptr))), const IntExpr*>);
static_assert(std::same_as<decltype(cast<IntExpr>(static_cast<Expr*>(nullptr))), IntExpr*>);
static_assert(std::same_as<decltype(cast<VarStmt>(static_cast<const Stmt*>(nullptr))), const VarStmt*>);
static_assert(std::same_as<decltype(as_type_decl(static_cast<const Item*>(nullptr))), const TypeDeclItem*>);
static_assert(std::same_as<decltype(std::declval<const DiagEngine&>().all()), std::span<const Diagnostic>>);
static_assert(!std::convertible_to<Modifier, u16>);
static_assert(!std::default_initializable<TypeDeclItem>);
static_assert(diag_code(DiagCode::UnexpectedCharacter) == "E0001");
static_assert(diag_code(DiagCode::NestingLimit) == "E0112");

struct Frontend {
    FrontendSession session;
    SourceManager& sources = session.sources();
    Interner& names = session.names();
    DiagEngine& diag = session.diagnostics();
    // Low-level tests also exercise parse_file with a prebuilt token stream.
    Arena arena;

    const File* parse(std::string_view source) {
        const auto file = sources.add("test.phs", source);
        CHECK(file);
        return session.parse(*file);
    }
};

void ast_casts_and_modifiers() {
    IntExpr value;
    value.value = 42;
    Expr* node = &value;
    const Expr* constant = node;
    CHECK(as<IntExpr>(node) == &value && cast<IntExpr>(constant)->value == 42);
    CHECK(as<StringExpr>(constant) == nullptr && as<IntExpr>(static_cast<Expr*>(nullptr)) == nullptr);
    for (ItemKind kind : {ItemKind::Class, ItemKind::Struct, ItemKind::Actor, ItemKind::Interface, ItemKind::Enum}) {
        TypeDeclItem declaration(kind);
        Item* item = &declaration;
        const Item* constant_item = item;
        CHECK(as_type_decl(item) == &declaration && cast_type_decl(constant_item)->kind == kind);
        CHECK(as<FunItem>(constant_item) == nullptr);
    }
    FunItem function;
    CHECK(as_type_decl(static_cast<const Item*>(&function)) == nullptr);
    CHECK(as_type_decl(static_cast<Item*>(nullptr)) == nullptr);
    Modifier mods = Modifier::Open | Modifier::Async;
    mods |= Modifier::Unsafe;
    CHECK(has_modifier(mods, Modifier::Open | Modifier::Unsafe));
    CHECK(!has_modifier(mods, Modifier::Final) && !has_modifier(mods, Modifier::None));
}

void lexer_values_and_spans() {
    Frontend frontend;
    const auto file = frontend.sources.add("tokens", "name 0x2a 1.5 'x' \"hi\"");
    CHECK(file);
    const auto tokens = lex_file(frontend.sources, *file, frontend.names, frontend.diag);
    CHECK(frontend.diag.error_count() == 0 && tokens.size() == 6);
    CHECK(tokens[0].kind == Tok::Ident && frontend.names.str(tokens[0].symbol()) == "name");
    CHECK(tokens[1].kind == Tok::IntLit && tokens[1].int_value() == 42);
    CHECK(tokens[2].kind == Tok::FloatLit && tokens[2].float_value() == 1.5);
    CHECK(tokens[3].kind == Tok::CharLit && tokens[3].char_value() == 'x');
    CHECK(tokens[4].kind == Tok::StringLit && frontend.names.str(tokens[4].symbol()) == "hi");
    CHECK(tokens.back().kind == Tok::Eof && tokens.back().span.lo == frontend.sources.file(*file).text.size());
    for (const Token& token : tokens) CHECK(token.span.file == *file && token.span.lo <= token.span.hi);
    // The public dump API accepts any contiguous read-only token sequence.
    const std::array prefix{tokens[0], tokens[1]};
    CHECK(dump_tokens(prefix, frontend.names, frontend.sources).find("IntLit 42") != std::string::npos);
}

void block_comment_newlines() {
    for (std::string_view ending : {"\n", "\r\n", "\r"}) {
        Frontend frontend;
        const std::string source = "let a = 1 /* outer" + std::string(ending) +
            " /* nested" + std::string(ending) + " */" + std::string(ending) + "*/ let b = 2";
        const auto id = frontend.sources.add("comments.phs", source);
        CHECK(id);
        auto tokens = lex_file(frontend.sources, *id, frontend.names, frontend.diag);
        CHECK(std::ranges::count(tokens, Tok::Newline, &Token::kind) == 1);
        const auto newline = std::ranges::find(tokens, Tok::Newline, &Token::kind);
        CHECK(newline->span.lo == frontend.sources.file(*id).text.find('\n'));
        CHECK(newline->span.hi == newline->span.lo + 1);
        const auto* file = parse_file(*id, std::move(tokens), frontend.arena, frontend.names, frontend.diag);
        CHECK(frontend.diag.error_count() == 0 && file->items.size() == 2);
        CHECK(frontend.names.str(file->items[1]->name.sym) == "b");
    }

    // An inner brace restores significant newlines even inside a call. The
    // returned value must stay on the return's physical line in both cases.
    for (std::string_view source : {"fun f() { return /*\n*/ 42 }", "let x = f(if true { return /*\n*/ 42 })"}) {
        Frontend frontend;
        const auto* file = frontend.parse(source);
        CHECK(frontend.diag.error_count() == 0 && file->items.size() == 1);
        const Expr* body = nullptr;
        if (const auto* fun = as<FunItem>(file->items[0])) {
            body = fun->body;
        } else {
            const auto* call = cast<CallExpr>(cast<VarItem>(file->items[0])->init);
            body = cast<IfExpr>(call->args[0].value)->then_block;
        }
        const auto* block = cast<BlockExpr>(body);
        CHECK(block->stmts.size() == 2);
        CHECK(cast<ReturnStmt>(block->stmts[0])->value == nullptr);
        CHECK(cast<IntExpr>(cast<ExprStmt>(block->stmts[1])->expr)->value == 42);
    }

    for (std::string_view source : {
        "let x = f(1 /*\n /* nested\n*/ */ + 2)",
        "let x = [1 /*\n*/ + 2]",
        "let x = \"value \\(1 /*\n*/ + 2)\"",
        "/* leading\n*/ let x = 1 /* same line */ + 2",
    }) {
        Frontend frontend;
        const auto id = frontend.sources.add("continued.phs", source);
        CHECK(id);
        auto tokens = lex_file(frontend.sources, *id, frontend.names, frontend.diag);
        CHECK(std::ranges::count(tokens, Tok::Newline, &Token::kind) == 0);
        const auto* file = parse_file(*id, std::move(tokens), frontend.arena, frontend.names, frontend.diag);
        CHECK(frontend.diag.error_count() == 0 && file->items.size() == 1);
    }

    Frontend frontend;
    const auto id = frontend.sources.add("unfinished.phs", "let x = 1\n/*\n /* nested */\n");
    CHECK(id);
    const auto tokens = lex_file(frontend.sources, *id, frontend.names, frontend.diag);
    CHECK(std::ranges::count(tokens, Tok::Newline, &Token::kind) == 1);
    CHECK(tokens.back().kind == Tok::Eof);
    CHECK(frontend.diag.error_count() == 1 && frontend.diag.all()[0].code == DiagCode::UnterminatedComment);
}

void token_dump_locations() {
    Frontend frontend;
    const auto id = frontend.sources.add("locations.phs", "/*\xCE\xB2\xF0\x9F\x98\x80*/\t+\xFF+\r\n+");
    CHECK(id);
    const auto tokens = lex_file(frontend.sources, *id, frontend.names, frontend.diag);
    CHECK(tokens.size() == 5 && frontend.diag.error_count() == 1);
    CHECK(frontend.diag.all()[0].code == DiagCode::InvalidUtf8);
    CHECK(dump_tokens(tokens, frontend.names, frontend.sources) ==
          "1:8 Plus (sp)\n1:10 Plus (sp)\n1:11 Newline\n2:1 Plus (sp)\n2:2 Eof\n");
    CHECK(dump_tokens(std::span(tokens).subspan(3), frontend.names, frontend.sources) ==
          "2:1 Plus (sp)\n2:2 Eof\n");
    CHECK(dump_tokens({}, frontend.names, frontend.sources).empty());

    const auto other = frontend.sources.add("other.phs", "\n+");
    CHECK(other);
    const Token plus{.kind = Tok::Plus, .span = {*other, 1, 2}};
    const std::array reordered{tokens[1], tokens[0], tokens[0], plus, tokens[4]};
    CHECK(dump_tokens(reordered, frontend.names, frontend.sources) ==
          "1:10 Plus (sp)\n1:8 Plus (sp)\n1:8 Plus (sp)\n2:1 Plus\n2:2 Eof\n");
}

void parser_structure_and_recovery() {
    Frontend frontend;
    const File* file = frontend.parse("public async fun calc() = 1 + 2 * 3\nlet broken = ;\nlet tail = 7\n");
    CHECK(file->items.size() == 3 && frontend.diag.error_count() == 1);
    CHECK(frontend.diag.all().front().code == DiagCode::ExpectedExpression);
    const Item* item = file->items.front();
    const auto* function = cast<FunItem>(item);
    CHECK(function->vis == Visibility::Public && has_modifier(function->mods, Modifier::Async));
    const Expr* body = function->body;
    const auto* add = cast<BinaryExpr>(body);
    CHECK(add->op == BinOp::Add && cast<IntExpr>(add->lhs)->value == 1);
    const auto* multiply = cast<BinaryExpr>(add->rhs);
    CHECK(multiply->op == BinOp::Mul && cast<IntExpr>(multiply->lhs)->value == 2);
    CHECK(cast<IntExpr>(multiply->rhs)->value == 3);
    CHECK(cast<IntExpr>(cast<VarItem>(file->items.back())->init)->value == 7);
    CHECK(dump_ast(*file, frontend.names).find("Let tail\n    Int 7") != std::string::npos);
}

void diagnostics_are_file_local() {
    Frontend frontend;
    // The first file's unterminated delimiter must not suppress EOF errors in
    // the second file when a compilation shares one diagnostic engine.
    const auto first = frontend.sources.add("first.phs", "(");
    CHECK(first);
    const auto tokens = lex_file(frontend.sources, *first, frontend.names, frontend.diag);
    CHECK(tokens.back().kind == Tok::Eof);
    const usize before = frontend.diag.error_count();
    CHECK(before == 1 && frontend.diag.all()[0].code == DiagCode::UnclosedDelimiter);
    const File* second = frontend.parse("let x =");
    CHECK(frontend.diag.error_count() == before + 1);
    CHECK(frontend.diag.all().back().code == DiagCode::ExpectedExpression);
    CHECK(frontend.diag.all().back().span.file == second->file);
}

std::string rendered(const DiagEngine& diag) {
    test::TempDirectory temp;
    const auto path = temp.path / "diagnostics.txt";
    std::FILE* raw = nullptr;
#ifdef _WIN32
    CHECK(::_wfopen_s(&raw, path.c_str(), L"w+b") == 0);
#else
    raw = std::fopen(path.c_str(), "w+b");
#endif
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> stream(raw, &std::fclose);
    CHECK(stream);
    diag.render(stream.get());
    CHECK(std::fseek(stream.get(), 0, SEEK_SET) == 0);
    std::string result;
    std::array<char, 1024> buffer;
    while (const auto count = std::fread(buffer.data(), 1, buffer.size(), stream.get()))
        result.append(buffer.data(), count);
    CHECK(!std::ferror(stream.get()));
    std::erase(result, '\r');
    return result;
}

void diagnostic_rendering_and_order() {
    SourceManager sources;
    const auto first = sources.add("first.phs", "a\t\xC3\xA9\xFF\n");
    const auto second = sources.add("second.phs", "z");
    CHECK(first && second);
    DiagEngine diag(sources);
    CHECK(diag.in_source_order().empty());
    diag.error(DiagCode::ExpectedToken, {*second, 0, 1}, "later file");
    auto& error = diag.error(DiagCode::ExpectedExpression, {*first, 2, 5}, "first message {}", 42);
    error.notes.push_back({{*first, 0, 1}, "context"});
    diag.warning(DiagCode::ExpectedExpression, {*first, 2, 5}, "same position");
    CHECK(diag.error_count() == 2 && diag.all().size() == 3);
    const auto ordered = diag.in_source_order();
    CHECK(ordered.size() == 3);
    CHECK(ordered[0] == &diag.all()[1] && ordered[1] == &diag.all()[2] && ordered[2] == &diag.all()[0]);
    CHECK(ordered[0]->notes.size() == 1);
    CHECK(diag.all().front().span.file == *second);  // Querying preserves emission order too.
    const std::string output = rendered(diag);
    const auto a = output.find("first.phs:1:3: error[E0101]: first message 42");
    const auto b = output.find("first.phs:1:3: warning[E0101]: same position");
    const auto c = output.find("second.phs:1:1: error[E0100]: later file");
    CHECK(a != std::string::npos && b != std::string::npos && c != std::string::npos && a < b && b < c);
    CHECK(output.find("first.phs:1:1: note: context") != std::string::npos);
    CHECK(output.find("a\t\xC3\xA9\xEF\xBF\xBD") != std::string::npos);
    CHECK(output.find("      |  \t^^\n") != std::string::npos);
    CHECK(diag.all().front().span.file == *second);  // Rendering does not reorder storage.
}

}  // namespace

void cast_type_context_and_follow_tokens() {
    for (std::string_view op : {"as", "is"}) {
        for (std::string_view type : {"fun() -> Int", "fun(A) -> Int", "fun(A<B>) -> Int",
             "fun([A<B>], fun(B) -> C) -> Result<T> throws(E<U>)"}) {
            Frontend f;
            const auto* file = f.parse(std::format("let value = x {} {} < y", op, type));
            if (f.diag.has_errors(file->file)) f.diag.render(stderr);
            CHECK(!f.diag.has_errors(file->file));
            const auto* comparison = cast<BinaryExpr>(cast<VarItem>(file->items[0])->init);
            CHECK(comparison->op == BinOp::Lt);
            const auto* conversion = cast<CastExpr>(comparison->lhs);
            CHECK(conversion->type->kind == TypeKind::Fun && conversion->type->args.empty() == type.starts_with("fun()"));
            CHECK(f.names.str(cast<NameExpr>(comparison->rhs)->name.sym) == "y");
        }
        for (std::string_view comparison : {">", ">="}) {
            for (std::string_view rhs : {"w", "1", "true", "\"text\"", "(w)", "!flag", ".Case"}) {
                Frontend f;
                const auto* file = f.parse(std::format("let value = foo(x {} Int < y, z {} {})", op, comparison, rhs));
                if (f.diag.has_errors(file->file)) f.diag.render(stderr);
                CHECK(!f.diag.has_errors(file->file));
                const auto* call = cast<CallExpr>(cast<VarItem>(file->items[0])->init);
                CHECK(call->args.size() == 2);
                CHECK(cast<BinaryExpr>(call->args[0].value)->op == BinOp::Lt);
                CHECK(cast<BinaryExpr>(call->args[1].value)->op == (comparison == ">" ? BinOp::Gt : BinOp::Ge));
            }
        }
    }
    // Cast types have more legal continuations than postfix generic values.
    for (std::string_view tail : {"+ y", "* y", "== y", "< y", ">> y", "?? y", "is Other", "as Other", "? ? + y"}) {
        Frontend f;
        const auto* file = f.parse(std::format("let value = x as Container<T> {}", tail));
        if (f.diag.has_errors(file->file)) f.diag.render(stderr);
        CHECK(!f.diag.has_errors(file->file));
        const auto* init = cast<VarItem>(file->items[0])->init;
        const auto* conversion = as<CastExpr>(init);
        if (!conversion) conversion = cast<CastExpr>(cast<BinaryExpr>(init)->lhs);
        else if (as<CastExpr>(conversion->operand)) conversion = cast<CastExpr>(conversion->operand);
        const auto* type = conversion->type;
        while (type->kind == TypeKind::Optional) type = type->inner;
        CHECK(type->kind == TypeKind::Path && type->args.size() == 1);
    }
    for (std::string_view source : {"let value = (x as Container<T>).field",
                                   "let value = (x as fun(A) -> Result<T>)(arg)"}) {
        Frontend f;
        const auto* file = f.parse(source);
        CHECK(!f.diag.has_errors(file->file));
    }
}

void generic_cast_control_flow_delimiters() {
    for (std::string_view op : {"is", "as"}) {
        for (std::string_view type : {"Box<Int>", "Box<Outer<Int>>", "Box<Int> ? ?", "fun(A) -> Box<Int>"}) {
            Frontend f;
            const auto* file = f.parse(std::format(
                "fun f(x: Int) {{\nif x {} {} {{}}\nwhile x {} {} {{}}\nfor element in x {} {} {{}}\n"
                "let result = match x {} {} {{ _ if x {} {} -> 0 }}\n}}",
                op, type, op, type, op, type, op, type, op, type));
            if (f.diag.has_errors(file->file)) f.diag.render(stderr);
            CHECK(!f.diag.has_errors(file->file));
            const auto* body = cast<BlockExpr>(cast<FunItem>(file->items[0])->body);
            CHECK(body->stmts.size() == 4);
            const auto* branch = cast<IfExpr>(cast<ExprStmt>(body->stmts[0])->expr);
            const auto* loop = cast<WhileStmt>(body->stmts[1]);
            const auto* iteration = cast<ForStmt>(body->stmts[2]);
            const auto* match = cast<MatchExpr>(cast<VarStmt>(body->stmts[3])->init);
            CHECK(match->arms.size() == 1 && match->arms[0].guard);
            for (const auto* expr : {branch->cond.expr, loop->cond.expr, iteration->iter, match->scrutinee, match->arms[0].guard}) {
                const auto* conversion = cast<CastExpr>(expr);
                CHECK(conversion->op == (op == "is" ? CastOp::Is : CastOp::As));
                const auto* target = conversion->type;
                if (target->kind == TypeKind::Fun) target = target->inner;
                while (target->kind == TypeKind::Optional) target = target->inner;
                CHECK(target->kind == TypeKind::Path && target->args.size() == 1);
            }
        }
    }
}

void multiline_alternative_patterns() {
    for (std::string_view newline : {"\n", "\r\n", "\r"}) {
        Frontend f;
        const auto* file = f.parse(std::format("fun f(x: Int) = match x {{ 1 |{} 2 -> 0 }}", newline));
        CHECK(!f.diag.has_errors(file->file));
        const auto* match = cast<MatchExpr>(cast<FunItem>(file->items[0])->body);
        CHECK(match->arms.size() == 1 && cast<AltPat>(match->arms[0].pattern)->alts.size() == 2);
    }
}

int main() {
    ast_casts_and_modifiers();
    cast_type_context_and_follow_tokens();
    generic_cast_control_flow_delimiters();
    multiline_alternative_patterns();
    lexer_values_and_spans();
    block_comment_newlines();
    token_dump_locations();
    parser_structure_and_recovery();
    diagnostics_are_file_local();
    diagnostic_rendering_and_order();
}
