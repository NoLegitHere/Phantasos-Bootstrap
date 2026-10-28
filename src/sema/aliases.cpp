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

#include "sema/types.hpp"

#include <algorithm>
#include <unordered_set>

namespace phs {
namespace {
using namespace ast;
ExpandedType failed() { return {ExpansionState::Error, {}, ScalarType::Error}; }
}  // namespace

AliasExpansion::Lookup AliasExpansion::binding(BindingId id, const Resolution& resolution) const {
    const auto& declaration = resolution.binding(id);
    if (declaration.kind == BindingKind::TypeAlias) {
        if (progress_[id.value] != 2) return {{}, id};
        return {aliases_.at(id.value)};
    }
    if (declaration.kind != BindingKind::BuiltinType && declaration.kind != BindingKind::Type) return {};
    if (declaration.kind == BindingKind::Type && !cast_type_decl(declaration.item)->generics.empty()) return {};
    const auto it = builtins_.find(id.value);
    return {{ExpansionState::Resolved, id, it == builtins_.end() ? ScalarType{} : it->second}};
}

AliasExpansion::Lookup AliasExpansion::path(std::span<const Name> parts, NameSpace space, const Resolution& resolution) const {
    Lookup result;
    for (usize p = 0; p < parts.size(); ++p) {
        const auto* reference = resolution.reference(parts[p].span);
        if (reference && reference->state == ResolutionState::Resolved) {
            const auto ids = resolution.candidates(reference->target);
            if (ids.size() != 1) return {};
            result = binding(ids.front(), resolution);
        } else {
            if (p == 0 || result.type.state != ExpansionState::Resolved || result.type.optional_depth || !result.type.target.valid()) return {};
            const auto& owner = resolution.binding(result.type.target);
            if (!owner.members.valid()) return {};
            if (space == NameSpace::Value) {
                if (member_values_.contains(member_key(owner.members.value, parts[p].sym.id))) return {};
            }
            const auto it = member_types_.find(member_key(owner.members.value, parts[p].sym.id));
            if (it == member_types_.end()) {
                if (space == NameSpace::Type && cast_type_decl(owner.item)->supers.empty())
                    return {failed(), {}, parts[p], result.type.target};
                return {};
            }
            result = binding(it->second, resolution);
        }
        if (result.pending.valid() || result.type.state != ExpansionState::Resolved) return result;
    }
    return result;
}

AliasExpansion::Lookup AliasExpansion::lookup(const TypeExpr* syntax, const Resolution& resolution) const {
    u32 depth = 0;
    while (syntax && syntax->kind == TypeKind::Optional) { ++depth; syntax = syntax->inner; }
    if (!syntax) return {};
    Lookup result;
    if (syntax->kind == TypeKind::Path) {
        result = path(syntax->path.parts, NameSpace::Type, resolution);
        if (!syntax->args.empty() && !result.pending.valid() && result.type.state != ExpansionState::Error) return {};
    } else if (syntax->kind == TypeKind::Unit) result = binding(unit_, resolution);
    else if (syntax->kind == TypeKind::SelfType) {
        const auto* reference = resolution.reference(syntax->span);
        if (!reference || reference->state != ResolutionState::Resolved) return {};
        const auto ids = resolution.candidates(reference->target);
        if (ids.size() != 1) return {};
        result = binding(ids.front(), resolution);
    } else return {};
    if (result.type.state == ExpansionState::Resolved) {
        result.type.optional_depth += depth;
        if (result.type.scalar.kind != ScalarType::Deferred && result.type.scalar.kind != ScalarType::Error)
            result.type.scalar.optional_depth += depth;
    }
    return result;
}

TypeExpansion AliasExpansion::type(const TypeExpr* syntax, const Resolution& resolution) const {
    const auto result = lookup(syntax, resolution);
    return {result.type, result.unknown, result.owner};
}

ExpandedType AliasExpansion::entity(const Expr* syntax, const Resolution& resolution) const {
    std::vector<Name> parts;
    while (syntax) {
        if (const auto* paren = as<ParenExpr>(syntax)) syntax = paren->inner;
        else if (const auto* member = as<MemberExpr>(syntax)) { parts.push_back(member->name); syntax = member->base; }
        else if (const auto* name = as<NameExpr>(syntax)) { parts.push_back(name->name); break; }
        else return {};
    }
    std::reverse(parts.begin(), parts.end());
    return path(parts, NameSpace::Value, resolution).type;
}

class AliasExpander {
public:
    AliasExpander(const Resolution& resolution, const Interner& names, DiagEngine& diag)
        : resolution_(resolution), names_(names), diag_(diag) {}

    AliasExpansion run() {
        const auto bindings = resolution_.bindings();
        result_.progress_.resize(bindings.size(), 2);
        dependencies_.resize(bindings.size());
        qualified_paths_.resize(bindings.size());
        positions_.resize(bindings.size());
        std::unordered_set<usize> interfaces;
        for (usize i = 0; i < bindings.size(); ++i) {
            const auto& b = bindings[i];
            if (b.kind == BindingKind::BuiltinType) {
                if (names_.str(b.name.sym) == "Unit") result_.unit_ = BindingId{i};
                for (u8 k = ScalarType::Int8; k <= ScalarType::Never; ++k) {
                    const ScalarType type{static_cast<ScalarType::Kind>(k)};
                    if (names_.str(b.name.sym) == scalar_type_name(type)) result_.builtins_.emplace(i, type);
                }
            }
            if (!b.members.valid()) continue;
            if (b.item->kind == ItemKind::Interface) interfaces.insert(b.members.value);
            for (BindingId member : resolution_.scopes()[b.members.value].bindings) {
                const auto& child = resolution_.binding(member);
                const u64 key = AliasExpansion::member_key(b.members.value, child.name.sym.id);
                if (child.kind == BindingKind::Type || child.kind == BindingKind::TypeAlias) result_.member_types_.try_emplace(key, member);
                else result_.member_values_.insert(key);
            }
        }
        for (usize i = 0; i < bindings.size(); ++i) {
            if (bindings[i].kind != BindingKind::TypeAlias) continue;
            result_.aliases_.emplace(i, ExpandedType{});
            const auto* alias = cast<TypeAliasItem>(bindings[i].item);
            if (alias->type && alias->generics.empty() && !interfaces.contains(bindings[i].scope.value)) result_.progress_[i] = 0;
        }
        // Include dependencies inside deferred composites so transparent alias
        // cycles cannot hide behind arrays, pointers or function types.
        for (const auto& [id, fact] : result_.aliases_) {
            (void)fact;
            if (result_.progress_[id] != 0) continue;
            std::vector<const TypeExpr*> pending{cast<TypeAliasItem>(bindings[id].item)->type};
            auto& deps = dependencies_[id];
            while (!pending.empty()) {
                const auto* type = pending.back(); pending.pop_back();
                if (type->kind == TypeKind::Path) {
                    if (type->path.parts.size() > 1) qualified_paths_[id].push_back(type);
                    for (const Name& name : type->path.parts) {
                        const auto* ref = resolution_.reference(name.span);
                        if (!ref || ref->state != ResolutionState::Resolved) continue;
                        for (BindingId target : resolution_.candidates(ref->target))
                            if (result_.aliases_.contains(target.value) && result_.progress_[target.value] == 0) deps.push_back(target.value);
                    }
                }
                if (type->inner) pending.push_back(type->inner);
                if (type->throws_type) pending.push_back(type->throws_type);
                for (const auto* arg : type->args) pending.push_back(arg);
            }
            std::sort(deps.begin(), deps.end());
            deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        }
        struct Frame { usize id, next = 0, checked = 0, path_next = 0; };
        std::vector<Frame> stack;
        const auto push = [&](usize id) {
            result_.progress_[id] = 1;
            positions_[id] = stack.size();
            stack.push_back({id});
        };
        for (usize root = 0; root < bindings.size(); ++root) {
            if (result_.progress_[root] != 0) continue;
            push(root);
            while (!stack.empty()) {
                auto& frame = stack.back();
                auto& fact = result_.aliases_.at(frame.id);
                auto& deps = dependencies_[frame.id];
                if (frame.next < deps.size()) {
                    const usize dep = deps[frame.next++];
                    if (result_.progress_[dep] == 0) push(dep);
                    else if (result_.progress_[dep] == 1 && result_.aliases_.at(dep).state != ExpansionState::Error) {
                        auto& d = diag_.error(DiagCode::AliasCycle, bindings[frame.id].name.span,
                                              "cyclic type alias expansion involving '{}'", names_.str(bindings[dep].name.sym));
                        for (usize p = positions_[dep]; p < stack.size(); ++p) {
                            const auto id = stack[p].id;
                            result_.aliases_.at(id) = failed();
                            d.notes.push_back({cast<TypeAliasItem>(bindings[id].item)->type->span,
                                               std::format("alias '{}' expands here", names_.str(bindings[id].name.sym))});
                        }
                    }
                    continue;
                }
                while (frame.checked < deps.size()) {
                    const auto dep = deps[frame.checked++];
                    if (result_.aliases_.at(dep).state == ExpansionState::Error) fact = failed();
                }
                if (fact.state != ExpansionState::Error) {
                    // A composite result can remain deferred while a path
                    // inside it still exposes an unconditional alias edge.
                    // Resolve each such path after its lexical prefixes, and
                    // resume it when a newly discovered dependency finishes.
                    const auto& paths = qualified_paths_[frame.id];
                    bool waiting = false;
                    while (frame.path_next < paths.size()) {
                        const auto lookup = result_.path(paths[frame.path_next]->path.parts, NameSpace::Type, resolution_);
                        if (lookup.pending.valid()) {
                            deps.push_back(lookup.pending.value);
                            waiting = true;
                            break;
                        }
                        ++frame.path_next;
                        if (lookup.type.state == ExpansionState::Error) {
                            report_unknown(lookup);
                            fact = failed();
                            break;
                        }
                    }
                    if (waiting) continue;
                }
                if (fact.state != ExpansionState::Error) {
                    const auto lookup = result_.lookup(cast<TypeAliasItem>(bindings[frame.id].item)->type, resolution_);
                    if (lookup.pending.valid()) { deps.push_back(lookup.pending.value); continue; }
                    report_unknown(lookup);
                    fact = lookup.type;
                    if (fact.state != ExpansionState::Error) fact.syntax = cast<TypeAliasItem>(bindings[frame.id].item)->type;
                }
                result_.progress_[frame.id] = 2;
                stack.pop_back();
            }
        }
        return std::move(result_);
    }

private:
    void report_unknown(const AliasExpansion::Lookup& lookup) {
        if (!lookup.unknown.sym.empty())
            diag_.error(DiagCode::UnknownType, lookup.unknown.span, "unknown type '{}' in '{}'",
                        names_.str(lookup.unknown.sym), names_.str(resolution_.binding(lookup.owner).name.sym));
    }

    const Resolution& resolution_;
    const Interner& names_;
    DiagEngine& diag_;
    AliasExpansion result_;
    std::vector<std::vector<usize>> dependencies_;
    std::vector<std::vector<const TypeExpr*>> qualified_paths_;
    std::vector<usize> positions_;
};

AliasExpansion expand_aliases(const Resolution& resolution, const Interner& names, DiagEngine& diag) {
    return AliasExpander(resolution, names, diag).run();
}
}  // namespace phs
