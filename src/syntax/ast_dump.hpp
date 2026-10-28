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

#pragma once

#include <string>
#include <span>

#include "support/interner.hpp"
#include "syntax/ast.hpp"
#include "syntax/token.hpp"

namespace phs {

// Stable, human-readable renderings used by `phantasos-stage0 lex|parse` and
// the golden tests.
std::string dump_tokens(std::span<const Token> tokens, const Interner& in, const SourceManager& sm);
std::string dump_ast(const ast::File& file, const Interner& in);

}  // namespace phs
