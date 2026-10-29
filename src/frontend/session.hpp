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

#include <optional>
#include <unordered_map>

#include "codegen/mir.hpp"
#include "sema/types.hpp"
#include "support/arena.hpp"

namespace phs {

struct CheckResult {
    const ast::File* syntax;
    // Null when syntax/structural errors prevented name resolution. A non-null
    // result can contain unresolved names; use diagnostics().has_errors(file)
    // to determine success.
    const Resolution* resolution;
    // Null if earlier phases failed. Flow checks run only after typing succeeds.
    // A non-null result retains known/deferred
    // scalar facts, including on type errors; consult per-file diagnostics.
    // Alias definition errors retain alias records but gate value checking.
    const ScalarTypes* scalar_types;
};

// Owns the storage for one frontend invocation over immutable source files.
// IDs passed to parse/check must belong to sources(). Each phase runs once
// per file; repeated calls reuse results without duplicating diagnostics.
// AST/result pointers stay valid as more files are added and checked, until
// this session is destroyed. Files still resolve independently (no modules).
class FrontendSession {
public:
    explicit FrontendSession(SourceLimits limits = {});
    FrontendSession(const FrontendSession&) = delete;
    FrontendSession& operator=(const FrontendSession&) = delete;
    FrontendSession(FrontendSession&&) = delete;
    FrontendSession& operator=(FrontendSession&&) = delete;

    SourceManager& sources() { return sources_; }
    const SourceManager& sources() const { return sources_; }
    Interner& names() { return names_; }
    const Interner& names() const { return names_; }
    DiagEngine& diagnostics() { return diag_; }
    const DiagEngine& diagnostics() const { return diag_; }

    [[nodiscard]] const ast::File* parse(FileId file);
    [[nodiscard]] CheckResult check(FileId file);
    // Runs check, then the stage0 code generation subset gate (E0600/E0601)
    // if checking succeeded. True when the file can be compiled by stage0.
    [[nodiscard]] bool check_codegen(FileId file);
    // Lowers a file that passes check_codegen to MIR; null otherwise. Cached.
    [[nodiscard]] const mir::Program* lower(FileId file);

private:
    struct FileState {
        const ast::File* syntax = nullptr;
        bool checked = false;
        bool gated = false;
        bool codegen = false;
        std::optional<mir::Program> mir;
        std::optional<Resolution> resolution;
        std::optional<ScalarTypes> scalar_types;
        FlowFacts flow;
    };

    SourceManager sources_;
    Interner names_;
    DiagEngine diag_{sources_};
    Arena arena_;
    // Rehashing invalidates iterators, but preserves FileState addresses.
    std::unordered_map<u32, FileState> files_;
};

}  // namespace phs
