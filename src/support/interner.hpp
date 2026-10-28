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

#include <string_view>
#include <unordered_map>
#include <vector>

#include "support/arena.hpp"

namespace phs {

// Interned string handle. Id 0 is the empty string.
struct Symbol {
    u32 id = 0;
    bool empty() const { return id == 0; }
    friend bool operator==(Symbol, Symbol) = default;
};

class Interner {
public:
    Symbol intern(std::string_view s) {
        if (s.empty()) return {};
        if (auto it = ids_.find(s); it != ids_.end()) return {it->second};
        const auto id = static_cast<u32>(strings_.size());
        const std::string_view stored = arena_.copy_str(s);  // stable storage for the map key
        strings_.push_back(stored);
        ids_.emplace(stored, id);
        return {id};
    }

    std::string_view str(Symbol s) const { return strings_[s.id]; }
    usize size() const { return strings_.size(); }

private:
    Arena arena_;
    std::vector<std::string_view> strings_{std::string_view{}};  // id 0 = ""
    std::unordered_map<std::string_view, u32> ids_;
};

}  // namespace phs
