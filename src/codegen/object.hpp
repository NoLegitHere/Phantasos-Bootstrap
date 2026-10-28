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

#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "support/common.hpp"

// A minimal relocatable object: a code section, a read-only data section, a
// zero-initialized writable data section, their symbols and the code's
// relocations. Object and executable writers
// consume it; keeping relocations symbolic lets the same code be linked
// statically or loaded in memory.

namespace phs::obj {

enum class Section : u8 { Text, Rodata, Bss };

struct Symbol {
    std::string name;
    bool defined = false;  // false: resolved by the linker (runtime entry points)
    u64 offset = 0;        // within its section when defined
    u64 size = 0;
    Section section = Section::Text;
    bool local = false;  // visible only to this object's relocations
};

// A 32-bit PC-relative reference from .text: the linker writes S + A - P at
// offset, where P is the address of the field (ELF R_X86_64_PLT32 and PC32).
struct Relocation {
    u64 offset = 0;
    u32 symbol = 0;
    i64 addend = -4;
};

struct Object {
    std::vector<u8> text;
    std::vector<u8> rodata;
    std::vector<Symbol> symbols;
    std::vector<Relocation> relocations;
    u64 bss = 0;  // bytes of .bss, which occupies no space in the object

    // Places bytes in .rodata and returns a local symbol for them. Identical
    // bytes share one symbol.
    u32 literal(std::string_view bytes) {
        const auto [found, added] = literals_.try_emplace(std::string(bytes), static_cast<u32>(symbols.size()));
        if (!added) return found->second;
        symbols.push_back({std::format(".Lstr{}", literals_.size() - 1), true, rodata.size(), bytes.size(),
                           Section::Rodata, true});
        rodata.insert(rodata.end(), bytes.begin(), bytes.end());
        return found->second;
    }

    // Reserves size zeroed bytes in .bss, 16-byte aligned, and returns a local
    // symbol for them.
    u32 reserve(std::string name, u64 size) {
        bss = (bss + 15) / 16 * 16;
        symbols.push_back({std::move(name), true, bss, size, Section::Bss, true});
        bss += size;
        return static_cast<u32>(symbols.size() - 1);
    }

private:
    std::unordered_map<std::string, u32> literals_;
};

}  // namespace phs::obj
