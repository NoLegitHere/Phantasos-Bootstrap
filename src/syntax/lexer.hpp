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

#include <vector>

#include "support/diag.hpp"
#include "support/interner.hpp"
#include "support/source.hpp"
#include "syntax/token.hpp"

namespace phs {

// Tokenizes a whole file. The result always ends with a Tok::Eof token.
// Newline tokens are only produced where they are significant (spec 1.3).
std::vector<Token> lex_file(const SourceManager& sm, FileId file, Interner& interner, DiagEngine& diag);

}  // namespace phs
