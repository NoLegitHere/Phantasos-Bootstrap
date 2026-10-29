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

#include "frontend/session.hpp"

#include "codegen/gate.hpp"
#include "sema/flow.hpp"
#include "sema/validate.hpp"
#include "syntax/lexer.hpp"
#include "syntax/parser.hpp"

namespace phs {

FrontendSession::FrontendSession(SourceLimits limits) : sources_(limits) {
    seed_keywords(names_);
}

const ast::File* FrontendSession::parse(FileId file) {
    auto& state = files_[file.value];
    if (!state.syntax)
        state.syntax = parse_file(file, lex_file(sources_, file, names_, diag_), arena_, names_, diag_);
    return state.syntax;
}

CheckResult FrontendSession::check(FileId file) {
    const auto* syntax = parse(file);
    auto& state = files_.at(file.value);
    if (!state.checked) {
        state.checked = true;
        if (!diag_.has_errors(file)) {
            validate_file(*syntax, names_, diag_);
            if (!diag_.has_errors(file)) state.resolution = resolve_file(*syntax, names_, diag_);
            if (!diag_.has_errors(file))
                state.scalar_types = check_scalar_types(*syntax, *state.resolution, names_, diag_);
            if (!diag_.has_errors(file) && state.scalar_types)
                check_flow(*syntax, *state.resolution, *state.scalar_types, names_, diag_);
        }
    }
    return {syntax, state.resolution ? &*state.resolution : nullptr,
            state.scalar_types ? &*state.scalar_types : nullptr};
}

bool FrontendSession::check_codegen(FileId file) {
    const auto result = check(file);
    auto& state = files_.at(file.value);
    if (!state.gated) {
        state.gated = true;
        state.codegen = !diag_.has_errors(file) &&
                        check_codegen_subset(*result.syntax, *result.resolution, *state.scalar_types, names_, diag_);
    }
    return state.codegen;
}

const mir::Program* FrontendSession::lower(FileId file) {
    if (!check_codegen(file)) return nullptr;
    auto& state = files_.at(file.value);
    if (!state.mir) state.mir = mir::lower(*state.syntax, *state.resolution, *state.scalar_types, names_, sources_);
    return &*state.mir;
}

}  // namespace phs
