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

#include "support/diag.hpp"
#include "syntax/ast.hpp"

namespace phs {

// Initial semantic pass: declaration, binding and control-flow constraints
// that do not require name resolution or type inference. The AST is unchanged.
// Call only after successful lexing/parsing of this file. A diagnostic for a
// different file does not prevent validation. All storage stays caller-owned.
void validate_file(const ast::File& file, const Interner& names, DiagEngine& diag);

}  // namespace phs
