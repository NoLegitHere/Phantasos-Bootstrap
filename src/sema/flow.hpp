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

#include <set>
#include <unordered_set>
#include <utility>

#include "sema/types.hpp"

namespace phs {

// Flow-sensitive checks for callable bodies: every path of a block-bodied
// function with a non-Unit result returns or diverges (E0500), locals declared
// without an initializer are definitely initialized before use (E0501), and
// assignments only target `var` locals/globals, `inout` parameters or a `let`
// local that is still definitely unassigned (E0502). Field assignments and
// `mut fun` calls need a mutable place (also E0502, spec 3.24). Closure bodies
// are checked as callables of their own, whose captures are read-only copies;
// capturing an `inout` or non-Copy borrowed parameter is E0503 (3.29). A
// non-Copy local or `own` parameter cannot be used after it moved (E0504,
// 3.30), only what the callable owns can move (E0505), and an `inout`
// argument or `mut fun` receiver cannot overlap another argument of the same
// call, and a `for` loop body cannot change or move what the loop runs over
// (E0506, 3.32, 3.33). Run after scalar typing succeeds; the AST,
// Resolution and ScalarTypes are unchanged.
//
// A value of generic type counts as Copy when it moves out of a borrow, since
// what it stands for is unknown. The result names those moves, and loop
// elements captured that way, so that an instance whose type argument owns
// an array can copy them.
struct FlowFacts {
    std::unordered_set<const ast::Expr*> copies;
    std::set<std::pair<const ast::ClosureExpr*, u32>> copied_captures;  // closure, binding
};
FlowFacts check_flow(const ast::File& file, const Resolution& resolution, const ScalarTypes& types,
                     const Interner& names, DiagEngine& diag);

}  // namespace phs
