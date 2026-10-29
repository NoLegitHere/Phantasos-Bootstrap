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

// Rejects structs and enums that hold themselves inline, through fields,
// payloads, tuples or optionals, so have no finite size (E0442, 3.37).
// Arrays, classes, pointers and functions hold their values elsewhere. Run
// after scalar typing succeeds; generic instances it needs are interned.
void check_sizes(const Resolution& resolution, ScalarTypes& types, const Interner& names, DiagEngine& diag);

}  // namespace phs
