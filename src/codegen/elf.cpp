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

#include "codegen/elf.hpp"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <limits>
#include <unordered_map>

namespace phs::elf {
namespace {

constexpr u64 kHeaderSize = 64, kProgramHeaderSize = 56, kSectionHeaderSize = 64, kSymbolSize = 24;
constexpr u64 kPage = 0x1000;

constexpr u64 align_up(u64 value, u64 to) { return (value + to - 1) / to * to; }

class Writer {
public:
    std::vector<u8> bytes;

    void u8_(u8 v) { bytes.push_back(v); }
    void u16_(u16 v) { put(v, 2); }
    void u32_(u32 v) { put(v, 4); }
    void u64_(u64 v) { put(v, 8); }
    void align(u64 to, u8 fill = 0) {
        while (bytes.size() % to) bytes.push_back(fill);
    }

private:
    void put(u64 v, int size) {
        for (int i = 0; i < size; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
};

struct Placed {
    const obj::Symbol* symbol;
    u64 address;
};

// Lays each object's section bytes out one after another, each object
// starting 16-byte aligned, and returns where each object's bytes begin.
std::vector<u64> concatenate(std::span<const obj::Object> objects, std::vector<u8> obj::Object::*section,
                             std::vector<u8>& out, u8 fill) {
    std::vector<u64> bases;
    for (const auto& object : objects) {
        while (out.size() % 16) out.push_back(fill);
        bases.push_back(out.size());
        out.insert(out.end(), (object.*section).begin(), (object.*section).end());
    }
    return bases;
}

}  // namespace

std::expected<std::vector<u8>, std::string> link(std::span<const obj::Object> objects, std::string_view entry) {
    // Code is padded with int3. Read-only data starts on its own page so that
    // it can be mapped without execute permission.
    std::vector<u8> text, rodata;
    const auto text_bases = concatenate(objects, &obj::Object::text, text, 0xCC);
    const auto rodata_bases = concatenate(objects, &obj::Object::rodata, rodata, 0);
    // Zeroed, writable data takes no file space; it starts on the page after the rest.
    std::vector<u64> bss_bases;
    u64 bss_size = 0;
    for (const auto& object : objects) {
        bss_size = align_up(bss_size, 16);
        bss_bases.push_back(bss_size);
        bss_size += object.bss;
    }
    const bool has_rodata = !rodata.empty();
    const bool has_bss = bss_size != 0;
    const u16 segments = static_cast<u16>(1 + (has_rodata ? 1 : 0) + (has_bss ? 1 : 0));
    // Code follows the ELF header and the program headers, 16-byte aligned.
    const u64 text_offset = align_up(kHeaderSize + kProgramHeaderSize * segments, 16);
    const u64 text_end = text_offset + text.size();
    const u64 text_address = kBaseAddress + text_offset;
    const u64 rodata_offset = align_up(text_end, kPage);
    const u64 rodata_address = kBaseAddress + rodata_offset;
    const u64 load_end = has_rodata ? rodata_offset + rodata.size() : text_end;
    const u64 bss_offset = align_up(load_end, kPage);
    const u64 bss_address = kBaseAddress + bss_offset;

    // Local symbols resolve only within their object; the rest share one namespace.
    std::vector<Placed> placed;
    std::vector<std::vector<u64>> local_addresses(objects.size());
    std::unordered_map<std::string_view, u64> addresses;
    for (usize o = 0; o < objects.size(); ++o)
        for (const auto& symbol : objects[o].symbols) {
            u64 address = 0;
            if (symbol.defined) {
                address = symbol.section == obj::Section::Text     ? text_address + text_bases[o] + symbol.offset
                        : symbol.section == obj::Section::Rodata ? rodata_address + rodata_bases[o] + symbol.offset
                                                                 : bss_address + bss_bases[o] + symbol.offset;
                if (!symbol.local && !addresses.emplace(symbol.name, address).second)
                    return std::unexpected(std::format("symbol '{}' is defined more than once", symbol.name));
                placed.push_back({&symbol, address});
            }
            local_addresses[o].push_back(address);
        }
    std::ranges::stable_partition(placed, [](const Placed& p) { return p.symbol->local; });

    for (usize o = 0; o < objects.size(); ++o)
        for (const auto& reloc : objects[o].relocations) {
            const auto& symbol = objects[o].symbols[reloc.symbol];
            u64 target = local_addresses[o][reloc.symbol];
            if (!symbol.defined) {
                const auto found = addresses.find(symbol.name);
                if (found == addresses.end()) return std::unexpected(std::format("undefined symbol '{}'", symbol.name));
                target = found->second;
            }
            const u64 field = text_bases[o] + reloc.offset;
            const i64 value = static_cast<i64>(target) + reloc.addend - static_cast<i64>(text_address + field);
            if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max())
                return std::unexpected(std::format("reference to '{}' is out of rel32 range", symbol.name));
            for (int i = 0; i < 4; ++i) text[field + i] = static_cast<u8>(static_cast<u64>(value) >> (8 * i));
        }

    const auto start = addresses.find(entry);
    if (start == addresses.end()) return std::unexpected(std::format("undefined entry symbol '{}'", entry));

    // .strtab and .shstrtab
    std::string strtab(1, '\0');
    std::vector<u32> names;
    for (const auto& p : placed) {
        names.push_back(static_cast<u32>(strtab.size()));
        strtab += p.symbol->name;
        strtab += '\0';
    }
    const std::string shstrtab = std::string("\0.text\0.rodata\0.bss\0.symtab\0.strtab\0.shstrtab\0", 46);
    constexpr u32 kTextName = 1, kRodataName = 7, kBssName = 15, kSymtabName = 20, kStrtabName = 28, kShstrtabName = 36;
    const auto locals = static_cast<u32>(std::ranges::count_if(placed, [](const Placed& p) { return p.symbol->local; }));

    const u64 symtab_offset = align_up(load_end, 8);
    const u64 symtab_size = kSymbolSize * (placed.size() + 1);
    const u64 strtab_offset = symtab_offset + symtab_size;
    const u64 shstrtab_offset = strtab_offset + strtab.size();
    const u64 section_offset = align_up(shstrtab_offset + shstrtab.size(), 8);

    Writer w;
    // ELF header: 64-bit, little-endian, System V ABI, executable, x86_64.
    for (const u8 b : std::initializer_list<u8>{0x7F, 'E', 'L', 'F', 2, 1, 1, 0}) w.u8_(b);
    w.u64_(0);
    w.u16_(2);   // ET_EXEC
    w.u16_(62);  // EM_X86_64
    w.u32_(1);
    w.u64_(start->second);
    w.u64_(kHeaderSize);
    w.u64_(section_offset);
    w.u32_(0);
    w.u16_(kHeaderSize);
    w.u16_(kProgramHeaderSize);
    w.u16_(segments);
    w.u16_(kSectionHeaderSize);
    w.u16_(7);
    w.u16_(6);
    const auto segment = [&](u32 flags, u64 offset, u64 size, u64 memory) {
        w.u32_(1);  // PT_LOAD
        w.u32_(flags);
        w.u64_(offset);
        w.u64_(kBaseAddress + offset);
        w.u64_(kBaseAddress + offset);
        w.u64_(size);
        w.u64_(memory);
        w.u64_(kPage);
    };
    segment(5, 0, text_end, text_end);  // the headers and .text: PF_R | PF_X
    if (has_rodata) segment(4, rodata_offset, rodata.size(), rodata.size());  // .rodata: PF_R
    if (has_bss) segment(6, bss_offset, 0, bss_size);  // .bss: PF_R | PF_W, no file bytes
    w.bytes.resize(text_offset);
    w.bytes.insert(w.bytes.end(), text.begin(), text.end());
    if (has_rodata) {
        w.bytes.resize(rodata_offset);
        w.bytes.insert(w.bytes.end(), rodata.begin(), rodata.end());
    }

    // .symtab: the null symbol, the local symbols, then the global ones.
    w.align(8);
    PHS_ASSERT(w.bytes.size() == symtab_offset);
    for (int i = 0; i < 3; ++i) w.u64_(0);
    for (usize i = 0; i < placed.size(); ++i) {
        const auto& symbol = *placed[i].symbol;
        const bool code = symbol.section == obj::Section::Text;
        w.u32_(names[i]);
        w.u8_(static_cast<u8>((symbol.local ? 0 : 1) << 4 | (code ? 2 : 1)));  // STB_LOCAL/GLOBAL, STT_FUNC/OBJECT
        w.u8_(0);
        w.u16_(static_cast<u16>(symbol.section) + 1);  // .text, .rodata or .bss
        w.u64_(placed[i].address);
        w.u64_(symbol.size);
    }
    for (const char c : strtab) w.u8_(static_cast<u8>(c));
    for (const char c : shstrtab) w.u8_(static_cast<u8>(c));
    w.align(8);
    PHS_ASSERT(w.bytes.size() == section_offset);

    const auto section = [&](u32 name, u32 type, u64 flags, u64 address, u64 offset, u64 size, u32 link, u32 info,
                             u64 align, u64 entsize) {
        w.u32_(name);
        w.u32_(type);
        w.u64_(flags);
        w.u64_(address);
        w.u64_(offset);
        w.u64_(size);
        w.u32_(link);
        w.u32_(info);
        w.u64_(align);
        w.u64_(entsize);
    };
    section(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    section(kTextName, 1, 6, text_address, text_offset, text.size(), 0, 0, 16, 0);  // SHF_ALLOC | SHF_EXECINSTR
    section(kRodataName, 1, 2, rodata_address, has_rodata ? rodata_offset : text_end, rodata.size(), 0, 0, 16, 0);  // SHF_ALLOC
    section(kBssName, 8, 3, bss_address, has_bss ? bss_offset : load_end, bss_size, 0, 0, 16, 0);  // SHT_NOBITS, SHF_WRITE | SHF_ALLOC
    section(kSymtabName, 2, 0, 0, symtab_offset, symtab_size, 5, 1 + locals, 8, kSymbolSize);
    section(kStrtabName, 3, 0, 0, strtab_offset, strtab.size(), 0, 0, 1, 0);
    section(kShstrtabName, 3, 0, 0, shstrtab_offset, shstrtab.size(), 0, 0, 1, 0);
    return std::move(w.bytes);
}

}  // namespace phs::elf
