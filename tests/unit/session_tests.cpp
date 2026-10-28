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

#include <string>
#include <type_traits>

#include "frontend/session.hpp"
#include "syntax/ast_dump.hpp"

using namespace phs;

static_assert(!std::is_copy_constructible_v<FrontendSession>);
static_assert(!std::is_move_constructible_v<FrontendSession>);

namespace {
FileId add(FrontendSession& session, std::string_view text) {
    const auto file = session.sources().add("test.phs", text);
    CHECK(file);
    return *file;
}

void diagnostic_counts_are_file_local() {
    FrontendSession session;
    const auto a = add(session, "a"), b = add(session, "b"), c = add(session, "c");
    auto& diag = session.diagnostics();
    CHECK(!diag.has_errors(a) && diag.error_count(b) == 0 && diag.error_count() == 0);
    diag.warning(DiagCode::ExpectedToken, {c, 0, 1}, "warning only");
    auto& first = diag.error(DiagCode::UnknownValue, {a, 0, 1}, "first error");
    first.notes.push_back({{b, 0, 1}, "context in another file"});
    CHECK(diag.has_errors(a) && !diag.has_errors(b) && !diag.has_errors(c));
    diag.error(DiagCode::UnknownValue, {a, 0, 1}, "second error");
    diag.error(DiagCode::UnknownValue, {b, 0, 1}, "third error");
    CHECK(diag.error_count() == 3 && diag.all().size() == 4);
    CHECK(diag.error_count(a) == 2 && diag.error_count(b) == 1 && diag.error_count(c) == 0);
    const auto ordered = diag.in_source_order();
    CHECK(ordered.front()->span.file == a && ordered.back()->span.file == c);
    CHECK(diag.error_count(a) + diag.error_count(b) + diag.error_count(c) == diag.error_count());
}

void independent_files_and_phase_gating() {
    FrontendSession session;
    auto& diag = session.diagnostics();
    // Keyword seeding happens before the caller interns any application names.
    session.names().intern("application_name");
    const auto lexical = add(session, "$\nfun bad() { break }");
    CHECK(!session.check(lexical).resolution);
    CHECK(diag.error_count(lexical) == 1);
    CHECK(diag.all().back().code == DiagCode::UnexpectedCharacter);

    const auto syntax = add(session, "fun bad() { let x = ; }");
    const auto syntax_result = session.check(syntax);
    CHECK(syntax_result.syntax && !syntax_result.resolution && diag.has_errors(syntax));
    const auto syntax_errors = diag.error_count();
    CHECK(session.parse(syntax) == syntax_result.syntax);
    CHECK(!session.check(syntax).resolution && diag.error_count() == syntax_errors);

    const auto structural = add(session, "fun bad(let x: Missing) = unknown");
    const auto* parsed = session.parse(structural);
    CHECK(parsed && !diag.has_errors(structural)); // parse remains syntax-only
    const std::string before = dump_ast(*parsed, session.names());
    const auto checked = session.check(structural);
    CHECK(checked.syntax == parsed && !checked.resolution);
    CHECK(dump_ast(*parsed, session.names()) == before);
    CHECK(diag.error_count(structural) == 1 && diag.all().back().code == DiagCode::InvalidParameterBinding);

    const auto names = add(session, "fun bad() = unknown");
    const auto name_result = session.check(names);
    CHECK(name_result.resolution && diag.error_count(names) == 1);
    CHECK(diag.all().back().code == DiagCode::UnknownValue);
    const auto total = diag.error_count();
    CHECK(session.check(names).resolution == name_result.resolution);
    CHECK(session.check(structural).syntax == parsed);
    CHECK(diag.error_count() == total); // failed phases are cached too

    const auto valid = add(session, "fun valid(x: Int) = x");
    diag.warning(DiagCode::ExpectedToken, {valid, 0, 3}, "warning does not block checking");
    const auto good = session.check(valid);
    CHECK(good.resolution && !diag.has_errors(valid));
    CHECK(good.syntax->items.size() == 1 && good.syntax->items[0]->kind == ast::ItemKind::Fun);
    CHECK(diag.error_count() == total);
}

void stable_results_and_isolated_names() {
    FrontendSession session;
    const auto first = add(session, "let x = 1\nfun f() = x");
    const auto checked = session.check(first);
    CHECK(checked.resolution && !session.diagnostics().has_errors(first));
    const auto* references = checked.resolution->references().data();
    CHECK(checked.resolution->references().size() == 1);
    const auto candidates = checked.resolution->candidates(references->target);
    CHECK(candidates.size() == 1);
    const auto* declaration = &checked.resolution->binding(candidates.front());
    CHECK(session.names().str(declaration->name.sym) == "x");
    for (int i = 0; i < 128; ++i) {
        const auto another = add(session, std::format("let other{} = 1", i));
        CHECK(session.check(another).resolution);
    }
    CHECK(session.check(first).resolution == checked.resolution);
    CHECK(session.parse(first) == checked.syntax);
    CHECK(checked.resolution->references().data() == references);
    CHECK(&checked.resolution->binding(candidates.front()) == declaration);
    CHECK(session.names().str(declaration->name.sym) == "x");
    const auto unrelated = add(session, "let value = x");
    CHECK(session.check(unrelated).resolution);
    CHECK(session.diagnostics().has_errors(unrelated));
    CHECK(!session.diagnostics().has_errors(first));
}

void custom_source_limits() {
    FrontendSession session({.max_file_bytes = 20, .max_files = 1});
    const auto too_large = session.sources().add("large", std::string(21, 'x'));
    CHECK(!too_large && too_large.error().kind == SourceErrorKind::TooLarge);
    const auto file = session.sources().add("valid", "let x = 1");
    CHECK(file && file->value == 0 && session.check(*file).resolution);
    const auto too_many = session.sources().add("extra", "");
    CHECK(!too_many && too_many.error().kind == SourceErrorKind::TooManyFiles);
    CHECK(!session.diagnostics().has_errors(*file));
}
}  // namespace

int main() {
    diagnostic_counts_are_file_local();
    independent_files_and_phase_gating();
    stable_results_and_isolated_names();
    custom_source_limits();
}
