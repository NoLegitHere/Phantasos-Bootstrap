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

// Checks that each typed `match` covers every value of its scrutinee
// (E0407, 3.36), naming a value it misses, and warns about arms that earlier
// arms already cover (E0408). Guarded arms cover nothing. Matches with
// patterns of generic enums are skipped, and ones too large to check are
// E0409. Run after scalar typing succeeds; the AST, Resolution and
// ScalarTypes are unchanged.
void check_matches(const Resolution& resolution, const ScalarTypes& types, const Interner& names, DiagEngine& diag);

}  // namespace phs
