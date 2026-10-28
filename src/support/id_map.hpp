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

#include <utility>
#include <vector>

#include "support/common.hpp"

namespace phs {

// A map keyed by small dense ids (such as binding ids) whose values are too
// large to keep in a hash table's slots. It costs one u32 per id up to the
// largest key, and a lookup reads that slot and then the entry, whether or
// not the key is present. Entries are kept in insertion order.
//
// Inserting may move entries, so references are invalidated by insertion.
template <typename V>
class IdMap {
public:
    using value_type = std::pair<usize, V>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }
    usize size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    void clear() { slots_.clear(); entries_.clear(); }

    iterator find(usize key) {
        const u32 slot = slot_of(key);
        return slot ? entries_.begin() + (slot - 1) : entries_.end();
    }
    const_iterator find(usize key) const {
        const u32 slot = slot_of(key);
        return slot ? entries_.begin() + (slot - 1) : entries_.end();
    }
    bool contains(usize key) const { return slot_of(key) != 0; }
    V& at(usize key) {
        const u32 slot = slot_of(key);
        PHS_ASSERT(slot);
        return entries_[slot - 1].second;
    }
    const V& at(usize key) const {
        const u32 slot = slot_of(key);
        PHS_ASSERT(slot);
        return entries_[slot - 1].second;
    }
    V& operator[](usize key) {
        if (const u32 slot = slot_of(key)) return entries_[slot - 1].second;
        if (key >= slots_.size()) slots_.resize(key + 1);  // grows geometrically
        entries_.emplace_back(key, V{});
        PHS_ASSERT(entries_.size() < (usize{1} << 32));
        slots_[key] = static_cast<u32>(entries_.size());
        return entries_.back().second;
    }

private:
    u32 slot_of(usize key) const { return key < slots_.size() ? slots_[key] : 0; }

    std::vector<u32> slots_;  // key -> index into entries_ + 1, or 0
    std::vector<value_type> entries_;
};

}  // namespace phs
