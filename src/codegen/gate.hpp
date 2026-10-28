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

#include "sema/types.hpp"

namespace phs {

// Accepts the subset that stage0 code generation lowers: top-level functions,
// generic ones too, over Bool and built-in integer types, with let/var locals,
// assignment, if/while/loop/break/continue/return, direct calls, arithmetic,
// comparisons, short-circuit logic and unary -, ! and ~. Everything else, and
// any expression whose type is not known, reports E0600; parameters and locals
// cannot be Unit or Never. A program needs one `fun main()` with no parameters
// that returns Int, Unit or Never (E0601). The generic instances a program
// needs are bounded in nesting and number (E0602).
// Run after check succeeds; the AST and Resolution are unchanged, and
// ScalarTypes only gains the types of instances.
// Returns true when the file is inside the subset.
bool check_codegen_subset(const ast::File& file, const Resolution& resolution, ScalarTypes& types,
                          const Interner& names, DiagEngine& diag);

}  // namespace phs
