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

#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

#include "support/diag.hpp"
#include "syntax/ast.hpp"

namespace phs {

// IDs belong to one Resolution. No pointers into growing containers escape.
template <typename Tag>
struct ResolutionId {
    usize value = std::numeric_limits<usize>::max();
    bool valid() const { return value != std::numeric_limits<usize>::max(); }
    friend bool operator==(ResolutionId, ResolutionId) = default;
};
using ScopeId = ResolutionId<struct ScopeTag>;
using BindingId = ResolutionId<struct BindingTag>;
using BindingSetId = ResolutionId<struct BindingSetTag>;

enum class NameSpace : u8 { Type, Value };
enum class ScopeKind : u8 { Prelude, File, Generics, Members, Parameters, Block, Condition, Loop, MatchArm, Closure };
enum class BindingKind : u8 {
    BuiltinType, BuiltinValue, Type, TypeAlias, Function, Variable, Field,
    EnumCase, GenericParameter, Parameter, Local, Pattern, Import
};
enum class ResolutionState : u8 { Resolved, Deferred, Unresolved };

struct ResolvedBinding {
    ast::Name name;
    BindingKind kind;
    ScopeId scope;
    const ast::Item* item = nullptr;  // non-null for item declarations
    ScopeId members;                 // direct members of a nominal type
};

struct LexicalScope {
    ScopeId parent;
    ScopeKind kind;
    std::span<const BindingId> bindings;  // owned by the Resolution
};

struct NameReference {
    Span span;
    ScopeId scope;
    NameSpace space;
    ResolutionState state;
    BindingSetId target;  // resolved declaration or overload set; invalid otherwise
    BindingId receiver;   // enclosing implicit instance, when available
};

// A local, parameter or pattern binding declared outside a closure and named
// inside it, with the first place the closure names it.
struct Capture {
    BindingId binding;
    Span use;
};

class Resolver;

// Owns scopes, bindings, overload sets and reference records, but not the AST
// or interner. Keep those alive while using item pointers and Symbols. The
// result is immutable through this API; its views live until it is moved or
// destroyed. Deferred references require receiver types or package loading.
class Resolution {
public:
    // Move-only: scopes_ and sets_ hold spans into this object's own storage.
    Resolution() = default;
    Resolution(Resolution&&) = default;
    Resolution& operator=(Resolution&&) = default;
    Resolution(const Resolution&) = delete;
    Resolution& operator=(const Resolution&) = delete;

    std::span<const ResolvedBinding> bindings() const { return bindings_; }
    std::span<const LexicalScope> scopes() const { return scopes_; }
    std::span<const NameReference> references() const { return references_; }
    const ResolvedBinding& binding(BindingId id) const { return bindings_.at(id.value); }
    std::span<const BindingId> candidates(BindingSetId id) const {
        return id.valid() ? sets_.at(id.value) : std::span<const BindingId>{};
    }
    const NameReference* reference(Span span) const;
    // What a closure captures, in order of first use. Names a nested closure
    // captures from outside this closure count as this closure's captures too.
    std::span<const Capture> captures(const ast::ClosureExpr* closure) const {
        const auto it = captures_.find(closure);
        return it == captures_.end() ? std::span<const Capture>{} : std::span<const Capture>(it->second);
    }

private:
    friend class Resolver;
    FileId file_;
    std::vector<ResolvedBinding> bindings_;
    std::vector<LexicalScope> scopes_;
    std::vector<std::span<const BindingId>> sets_;
    std::vector<BindingId> scope_items_, set_items_;  // storage behind scopes_ and sets_
    std::vector<NameReference> references_;  // sorted by span.lo
    static constexpr u32 kReferenceBucketBits = 4;
    std::vector<u32> reference_buckets_;      // (span.lo >> kReferenceBucketBits) -> first index in references_
    std::unordered_map<const ast::ClosureExpr*, std::vector<Capture>> captures_;
};

// Run after parsing and validate_file succeed. Resolves one file; imports
// receive an explicit unsupported diagnostic until package loading exists.
// Traversal is iterative and leaves the AST unchanged. Built-in names may be
// interned. Even on errors, the returned result retains successful bindings.
[[nodiscard]] Resolution resolve_file(const ast::File& file, Interner& names, DiagEngine& diag);

}  // namespace phs
