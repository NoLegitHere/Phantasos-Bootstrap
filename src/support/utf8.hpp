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

#include <bit>
#include <optional>
#include <string>
#include <string_view>

#include "support/common.hpp"

// The standard library has no supported UTF-8 codec (std::codecvt was
// deprecated in C++17 and removed in C++26), so stage0 carries this small one.

namespace phs {

struct Utf8Char {
    u32 cp;   // Unicode scalar value
    u32 len;  // encoded length in bytes (1-4)
};

// Decodes the scalar value at the start of `s`. Returns nullopt unless the
// bytes are a valid shortest-form encoding: rejects truncated and overlong
// sequences, surrogates, values above U+10FFFF and stray continuation bytes.
constexpr std::optional<Utf8Char> utf8_decode(std::string_view s) {
    if (s.empty()) return std::nullopt;
    const auto b0 = static_cast<u8>(s[0]);
    if (b0 < 0x80) return Utf8Char{b0, 1};
    // The number of leading 1-bits in the lead byte is the sequence length.
    const auto len = static_cast<u32>(std::countl_one(b0));
    if (len < 2 || len > 4 || s.size() < len) return std::nullopt;
    u32 cp = b0 & (0x7Fu >> len);
    for (u32 i = 1; i < len; ++i) {
        const auto b = static_cast<u8>(s[i]);
        if ((b & 0xC0) != 0x80) return std::nullopt;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    constexpr u32 kMinForLen[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMinForLen[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt;
    return Utf8Char{cp, len};
}

// Bytes to advance past the character at the start of `s`: the length of a
// valid sequence, or 1 for an invalid byte. Each step is one display column.
constexpr u32 utf8_step(std::string_view s) {
    const auto c = utf8_decode(s);
    return c ? c->len : 1;
}

// Appends the UTF-8 encoding of a valid Unicode scalar value.
inline void utf8_encode(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Compile-time checks of the decoder.
static_assert(utf8_decode("A")->cp == 'A');
static_assert(utf8_decode("\xC3\xA9")->cp == 0xE9 && utf8_decode("\xC3\xA9")->len == 2);
static_assert(utf8_decode("\xF0\x9F\x98\x80")->cp == 0x1F600);
static_assert(!utf8_decode("\xC0\xAF"));          // overlong '/'
static_assert(!utf8_decode("\xED\xA0\x80"));      // surrogate U+D800
static_assert(!utf8_decode("\xF4\x90\x80\x80"));  // above U+10FFFF
static_assert(!utf8_decode("\xE2\x82"));          // truncated
static_assert(!utf8_decode("\x80"));              // stray continuation byte
static_assert(utf8_step("\xFF") == 1);

}  // namespace phs
