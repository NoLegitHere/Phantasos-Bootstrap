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

#include <deque>
#include <iterator>
#include <tuple>
#include <utility>
#include <vector>

#include "support/common.hpp"

namespace phs {

// A map keyed by AST node pointers that carry a dense, nonzero `id`
// (ast::Expr and ast::TypeExpr number their nodes per file). It costs one u32
// slot per id up to the largest key, plus one entry per stored value, instead
// of a heap node and bucket per entry. Only keys from one file may be mixed.
//
// Like std::unordered_map, references to values stay valid when other keys
// are inserted. erase() moves the last entry into the erased one's place, so
// it invalidates references to that last entry; iteration order is insertion
// order, apart from that move.
template <typename K, typename V>
class NodeMap {
public:
    using value_type = std::pair<K, V>;
    using iterator = typename std::deque<value_type>::iterator;
    using const_iterator = typename std::deque<value_type>::const_iterator;

    iterator find(K key) {
        const u32 slot = slot_of(key);
        return slot ? entries_.begin() + (slot - 1) : entries_.end();
    }
    const_iterator find(K key) const {
        const u32 slot = slot_of(key);
        return slot ? entries_.begin() + (slot - 1) : entries_.end();
    }
    iterator begin() { return entries_.begin(); }
    const_iterator begin() const { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator end() const { return entries_.end(); }
    bool contains(K key) const { return slot_of(key) != 0; }
    // Sizes the slot table for ids up to max_id, so it never grows by doubling.
    void reserve_ids(u32 max_id) { slots_.reserve(max_id + 1); }
    usize size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    V& at(K key) {
        const auto it = find(key);
        PHS_ASSERT(it != end());
        return it->second;
    }
    const V& at(K key) const {
        const auto it = find(key);
        PHS_ASSERT(it != end());
        return it->second;
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(K key, Args&&... args) {
        if (const auto it = find(key); it != end()) return {it, false};
        const u32 id = key->id;
        PHS_ASSERT(id != 0);
        if (id >= slots_.size()) slots_.resize(id + 1);  // grows capacity geometrically
        entries_.emplace_back(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple(std::forward<Args>(args)...));
        slots_[id] = static_cast<u32>(entries_.size());
        return {std::prev(entries_.end()), true};
    }
    template <typename... Args>
    std::pair<iterator, bool> emplace(K key, Args&&... args) { return try_emplace(key, std::forward<Args>(args)...); }
    std::pair<iterator, bool> insert(K key) { return try_emplace(key); }  // set-style use with an empty V
    V& operator[](K key) { return try_emplace(key).first->second; }

    usize erase(K key) {
        const u32 slot = slot_of(key);
        if (!slot) return 0;
        if (slot != entries_.size()) {
            entries_[slot - 1] = std::move(entries_.back());
            slots_[entries_[slot - 1].first->id] = slot;
        }
        entries_.pop_back();
        slots_[key->id] = 0;
        return 1;
    }

private:
    u32 slot_of(K key) const { return key->id < slots_.size() ? slots_[key->id] : 0; }

    std::vector<u32> slots_;           // id -> index into entries_ + 1, or 0
    std::deque<value_type> entries_;   // stable references on insertion
};

}  // namespace phs
