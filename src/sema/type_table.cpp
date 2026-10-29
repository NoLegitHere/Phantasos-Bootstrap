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
#include <string_view>
#include <variant>

#include "support/flat_map.hpp"

namespace phs {
usize TypeDescriptorHash::operator()(const TypeDescriptor& d) const {
    usize hash = static_cast<usize>(d.kind);
    const auto add = [&](usize n) { hash ^= n + 0x9e3779b9U + (hash << 6) + (hash >> 2); };
    const auto type = [&](ScalarType t) { add(t.kind); add(t.optional_depth); add(t.detail); };
    type(d.element); type(d.result); type(d.error);
    add(d.declaration.value); add(d.mutable_pointer); add(d.throws);
    for (auto param : d.parameters) type(param);
    for (auto arg : d.arguments) type(arg);
    return hash;
}

ScalarType TypeTable::intern(TypeDescriptor descriptor) {
    // Each descriptor is stored once, in descriptors_; slots_ indexes them by
    // hash with linear probing, so a lookup touches one slot line per probe.
    const u64 hash = mix_key(TypeDescriptorHash{}(descriptor));
    if ((descriptors_.size() + 1) * 4 > slots_.size() * 3) grow();
    const usize mask = slots_.size() - 1;
    usize index = hash & mask;
    for (; slots_[index].id; index = (index + 1) & mask) {
        const usize id = slots_[index].id - 1;
        if (slots_[index].hash == static_cast<u32>(hash) && hashes_[id] == hash && descriptors_[id] == descriptor)
            return {descriptor.kind, 0, id};
    }
    const usize id = descriptors_.size();
    PHS_ASSERT(id < std::numeric_limits<u32>::max());
    slots_[index] = {static_cast<u32>(id + 1), static_cast<u32>(hash)};
    hashes_.push_back(hash);
    descriptors_.push_back(std::move(descriptor));
    return {descriptors_[id].kind, 0, id};
}

ScalarType ScalarTypes::substitute(ScalarType type, std::span<const TypeArgument> arguments) {
    if (arguments.empty() || !types_.descriptor(type)) return type;
    // Types share their parts, so each distinct descriptor is substituted
    // once, without its optional wrappers, parts first and without
    // recursion, since aliases can nest types deeply.
    FlatMap<usize, ScalarType> done;
    std::vector<std::pair<usize, bool>> work{{type.detail, false}};  // a descriptor, and whether its parts are done
    const auto at = [](usize detail) { return ScalarType{ScalarType::Deferred, 0, detail}; };
    while (!work.empty()) {
        const auto [detail, parts_done] = work.back();
        work.pop_back();
        if (done.find(detail) != done.end()) continue;
        // Interning a part can move the descriptor, so a copy is changed.
        TypeDescriptor next = *types_.descriptor(at(detail));
        ScalarType result{next.kind, 0, detail};
        if (next.kind == ScalarType::TypeParameter) {
            for (const auto& [parameter, argument] : arguments)
                if (parameter == next.declaration) result = argument;
            done.emplace(detail, result);
            continue;
        }
        std::vector<ScalarType*> parts{&next.element, &next.result, &next.error};
        for (auto& parameter : next.parameters) parts.push_back(&parameter);
        for (auto& argument : next.arguments) parts.push_back(&argument);
        if (!parts_done) {
            work.push_back({detail, true});
            for (const ScalarType* part : parts)
                if (types_.descriptor(*part) && done.find(part->detail) == done.end()) work.push_back({part->detail, false});
            continue;
        }
        bool changed = false;
        for (ScalarType* part : parts) {
            if (!types_.descriptor(*part)) continue;
            ScalarType replaced = done.find(part->detail)->second;
            replaced.optional_depth += part->optional_depth;
            changed |= replaced != *part;
            *part = replaced;
        }
        if (changed) result = types_.intern(std::move(next));
        done.emplace(detail, result);
    }
    auto result = done.find(type.detail)->second;
    result.optional_depth += type.optional_depth;
    return result;
}

ScalarType ScalarTypes::declared(BindingId declaration, std::span<const BindingId> parameters) {
    std::vector<ScalarType> arguments;
    for (const BindingId parameter : parameters)
        arguments.push_back(types_.intern({.kind = ScalarType::TypeParameter, .declaration = parameter}));
    return types_.intern({.kind = ScalarType::Nominal, .declaration = declaration, .arguments = std::move(arguments)});
}

ScalarType ScalarTypes::parameter(BindingId declaration) {
    return types_.intern({.kind = ScalarType::TypeParameter, .declaration = declaration});
}

void TypeTable::grow() {
    // Rebuilds from the cached hashes; descriptors are never rehashed.
    std::vector<Slot> slots(slots_.empty() ? 64 : slots_.size() * 2);
    const usize mask = slots.size() - 1;
    for (usize id = 0; id < hashes_.size(); ++id) {
        usize index = hashes_[id] & mask;
        while (slots[index].id) index = (index + 1) & mask;
        slots[index] = {static_cast<u32>(id + 1), static_cast<u32>(hashes_[id])};
    }
    slots_ = std::move(slots);
}

std::string TypeTable::name(ScalarType root, const Resolution& resolution, const Interner& names, usize limit) const {
    struct Repeat { usize count; char character; };
    using Work = std::variant<ScalarType, std::string_view, Repeat>;
    std::vector<Work> work{root};
    std::string result;
    const usize work_limit = std::max<usize>(1, limit);
    usize steps = 0;
    bool elided = false;
    const auto append = [&](std::string_view text) {
        const usize available = limit - result.size();
        result.append(text.substr(0, available));
        elided |= text.size() > available;
    };
    const auto push = [&](Work next) {
        if (work.size() >= work_limit) elided = true;
        else work.push_back(next);
    };
    while (!work.empty() && !elided) {
        if (steps++ == work_limit) { elided = true; break; }
        Work next = std::move(work.back()); work.pop_back();
        if (const auto* text = std::get_if<std::string_view>(&next)) { append(*text); continue; }
        if (const auto* repeat = std::get_if<Repeat>(&next)) {
            const usize available = limit - result.size();
            result.append(std::min(repeat->count, available), repeat->character);
            elided |= repeat->count > available;
            continue;
        }
        auto type = std::get<ScalarType>(next);
        if (type.optional_depth) {
            const bool group = type.kind == ScalarType::Function || type.kind == ScalarType::Pointer ||
                               type.kind == ScalarType::Shared || type.kind == ScalarType::Weak;
            if (group) append("(");
            push(Repeat{type.optional_depth, '?'});
            if (group) push(")");
            type.optional_depth = 0;
        }
        if (elided) break;
        const auto* d = descriptor(type);
        if (!d) { append(scalar_type_name(type)); continue; }
        switch (d->kind) {
            case ScalarType::TypeParameter:
                if (resolution.binding(d->declaration).kind == BindingKind::Type) append("Self");
                else append(names.str(resolution.binding(d->declaration).name.sym));
                break;
            case ScalarType::Projection:
                push(names.str(resolution.binding(d->declaration).name.sym)); push("."); push(d->element); break;
            case ScalarType::Nominal: {
                std::vector<std::string_view> parts{names.str(resolution.binding(d->declaration).name.sym)};
                auto scope = resolution.binding(d->declaration).scope;
                while (scope.valid()) {
                    if (steps++ == work_limit) { elided = true; break; }
                    if (resolution.scopes()[scope.value].kind == ScopeKind::Members) {
                        for (const auto& binding : resolution.bindings())
                            if (binding.members == scope) { parts.push_back(names.str(binding.name.sym)); break; }
                    }
                    scope = resolution.scopes()[scope.value].parent;
                }
                for (auto it = parts.rbegin(); it != parts.rend() && !elided; ++it) {
                    if (it != parts.rbegin()) append(".");
                    if (!elided) append(*it);
                }
                if (!d->arguments.empty()) {
                    append("<"); push(">");
                    for (usize i = d->arguments.size(); i > 0 && !elided; --i) {
                        push(d->arguments[i - 1]);
                        if (i > 1) push(", ");
                    }
                }
                break;
            }
            case ScalarType::Array:
                append("["); push("]"); push(d->element); break;
            case ScalarType::Pointer:
                append(d->mutable_pointer ? "*mut " : "*"); push(d->element); break;
            case ScalarType::Shared: case ScalarType::Weak:
                append(d->kind == ScalarType::Shared ? "shared " : "weak "); push(d->element); break;
            case ScalarType::Tuple:
                append("("); push(")");
                for (usize i = d->parameters.size(); i > 0 && !elided; --i) {
                    push(d->parameters[i - 1]);
                    if (i > 1) push(", ");
                }
                break;
            case ScalarType::Function:
                append("fun(");
                if (d->throws) {
                    push(")"); push(d->error); push(" throws(");
                }
                push(d->result); push(") -> ");
                for (usize i = d->parameters.size(); i > 0 && !elided; --i) {
                    push(d->parameters[i - 1]);
                    if (i > 1) push(", ");
                }
                break;
            default: unreachable();
        }
    }
    if (elided) {
        const auto marker = std::string_view("...").substr(0, std::min<usize>(3, limit));
        result.resize(std::min(result.size(), limit - marker.size()));
        result += marker;
    }
    return result;
}
}  // namespace phs
