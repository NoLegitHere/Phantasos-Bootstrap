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

#include "sema/size.hpp"

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace phs {
namespace {
using namespace ast;
using Type = ScalarType;

// The values each struct and enum holds inline form a graph over types,
// whose cycles (strongly connected components, after Tarjan) are values that
// hold themselves. A type nested inside itself without end, such as
// `Grow<T>(inner: Grow<Box<T>>)`, has no cycle and is caught by its depth.
// Walks depth first, without recursion, and visits each type once.
class Sizes {
public:
    Sizes(const Resolution& resolution, ScalarTypes& types, const Interner& names, DiagEngine& diag)
        : resolution_(resolution), types_(types), names_(names), diag_(diag) {}

    void check() {
        for (usize i = 0; i < resolution_.bindings().size(); ++i)
            if (value_type(BindingId{i})) visit(types_.declared(BindingId{i}, generics(BindingId{i})));
    }

private:
    // No type written in source nests one declaration this deep.
    static constexpr usize kNesting = 256;
    bool value_type(BindingId id) const {
        const auto& binding = resolution_.binding(id);
        return binding.kind == BindingKind::Type && binding.item && binding.members.valid() &&
               (binding.item->kind == ItemKind::Struct || binding.item->kind == ItemKind::Enum);
    }
    BindingId declaration(Type type) const {
        if (type.optional_depth || type.kind != Type::Nominal) return {};
        const BindingId id = types_.descriptor(type)->declaration;
        return value_type(id) ? id : BindingId{};
    }
    std::vector<BindingId> generics(BindingId id) const {
        std::vector<BindingId> result;
        const auto scope = resolution_.scopes()[resolution_.binding(id).members.value].parent;
        for (const BindingId param : resolution_.scopes()[scope.value].bindings)
            if (resolution_.binding(param).kind == BindingKind::GenericParameter) result.push_back(param);
        return result;
    }

    // The structs and enums a value of `type` holds inline, each with the
    // stored field or case it is held through.
    struct Part { Type type; BindingId via; };
    std::vector<Part> parts(Type type) {
        const BindingId id = declaration(type);
        const auto params = generics(id);
        const auto arguments = types_.descriptor(type)->arguments;
        std::vector<ScalarTypes::TypeArgument> substitution;
        for (usize i = 0; i < params.size() && i < arguments.size(); ++i) substitution.push_back({params[i], arguments[i]});
        std::vector<Part> out;
        const auto hold = [&](Type held, BindingId via) {
            std::vector<Type> work{substitution.empty() ? held : types_.substitute(held, substitution)};
            while (!work.empty()) {
                // An optional holds its value inline, and a tuple its elements.
                const Type next = work.back().scalar();
                work.pop_back();
                if (next.kind == Type::Tuple) {
                    const auto elements = types_.descriptor(next)->parameters;
                    work.insert(work.end(), elements.rbegin(), elements.rend());
                } else if (declaration(next).valid()) out.push_back({next, via});
            }
        };
        for (const BindingId member : resolution_.scopes()[resolution_.binding(id).members.value].bindings) {
            const auto& binding = resolution_.binding(member);
            if (binding.kind == BindingKind::Field && !(binding.item && has_modifier(binding.item->mods, Modifier::Static)))
                hold(types_.binding(member), member);
            else if (binding.kind == BindingKind::EnumCase)
                if (const auto* signature = types_.signature(member))
                    for (const Type field : signature->parameters) hold(field, member);
        }
        return out;
    }

    struct Node {
        Type type;
        BindingId declaration;
        std::vector<Part> parts;
        u32 index = 0, low = 0;
        bool active = false;  // on the component stack
    };
    u32 add(Type type) {
        const u32 id = static_cast<u32>(nodes_.size());
        ids_.emplace(type.detail, id);
        auto parts_of = parts(type);
        nodes_.push_back({type, declaration(type), std::move(parts_of), id, id, true});
        components_.push_back(id);
        ++nesting_[nodes_.back().declaration.value];
        return id;
    }

    void visit(Type start) {
        if (ids_.contains(start.detail)) return;
        std::vector<std::pair<u32, usize>> stack{{add(start), 0}};  // a node and its next part
        while (!stack.empty()) {
            auto& [id, at] = stack.back();
            if (at < nodes_[id].parts.size()) {
                const Part part = nodes_[id].parts[at++];
                if (const auto known = ids_.find(part.type.detail); known != ids_.end()) {
                    if (nodes_[known->second].active) nodes_[id].low = std::min(nodes_[id].low, nodes_[known->second].index);
                    continue;
                }
                if (const BindingId held = declaration(part.type); nesting_[held.value] >= kNesting) {
                    report(held, part.via);
                    continue;
                }
                const u32 next = add(part.type);
                stack.push_back({next, 0});
                continue;
            }
            const u32 done = id;
            stack.pop_back();
            --nesting_[nodes_[done].declaration.value];
            if (!stack.empty()) nodes_[stack.back().first].low = std::min(nodes_[stack.back().first].low, nodes_[done].low);
            if (nodes_[done].low == nodes_[done].index) component(done);
        }
    }

    // Pops the component rooted at `id`. The types of a cycle that are their
    // declaration's own, such as `Node` or `Looped<T>`, are reported; an
    // instance like `Box<Node>` is fine for other arguments.
    void component(u32 id) {
        std::vector<u32> members;
        do {
            members.push_back(components_.back());
            components_.pop_back();
            nodes_[members.back()].active = false;
        } while (members.back() != id);
        std::unordered_set<usize> inside;
        for (const u32 member : members) inside.insert(nodes_[member].type.detail);
        const auto cycle_part = [&](u32 member) {
            for (const Part& part : nodes_[member].parts)
                if (inside.contains(part.type.detail)) return &part;
            return static_cast<const Part*>(nullptr);
        };
        if (members.size() == 1 && !cycle_part(id)) return;
        bool reported = false;
        for (const u32 member : std::views::reverse(members)) {
            const Node& node = nodes_[member];
            if (node.type != types_.declared(node.declaration, generics(node.declaration))) continue;
            report(node.declaration, cycle_part(member)->via);
            reported = true;
        }
        if (!reported) report(nodes_[id].declaration, cycle_part(id)->via);
    }

    void report(BindingId id, BindingId via) {
        if (!reported_.insert(id.value).second) return;
        const auto& binding = resolution_.binding(id);
        const auto& through = resolution_.binding(via);
        const bool is_case = through.kind == BindingKind::EnumCase;
        diag_.error(DiagCode::InfiniteSize, binding.name.span,
                    "'{}' holds itself inline through {} '{}{}', so it has no finite size; hold it in an array or a class instead",
                    names_.str(binding.name.sym), is_case ? "case" : "field", is_case ? "." : "",
                    names_.str(through.name.sym));
    }

    const Resolution& resolution_;
    ScalarTypes& types_;
    const Interner& names_;
    DiagEngine& diag_;
    std::vector<Node> nodes_;
    std::unordered_map<usize, u32> ids_;  // by type
    std::vector<u32> components_;
    std::unordered_map<usize, usize> nesting_;  // by declaration, on the walk's stack
    std::unordered_set<usize> reported_;
};
}  // namespace

void check_sizes(const Resolution& resolution, ScalarTypes& types, const Interner& names, DiagEngine& diag) {
    Sizes(resolution, types, names, diag).check();
}

}  // namespace phs
