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

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codegen/object.hpp"

// A static linker and writer for Linux x86_64 ELF64 executables. It lays the
// objects' code and read-only data out one after another, resolves each
// relocation against the symbols the objects define (local symbols only within
// their own object), and writes an ET_EXEC image with a read-only, executable
// PT_LOAD segment for the code, a page-aligned read-only one for data when
// there is some, and a page-aligned writable one for zeroed data (.bss), with
// no file bytes, when there is some of that. Section headers and a symbol table are included so
// that objdump, readelf and debuggers can name the code; the loader needs only
// the program headers.

namespace phs::elf {

inline constexpr u64 kBaseAddress = 0x400000;

// Fails with a message naming the symbol when one is undefined, defined twice,
// or out of rel32 reach, or when entry is not defined.
[[nodiscard]] std::expected<std::vector<u8>, std::string> link(std::span<const obj::Object> objects,
                                                               std::string_view entry);

}  // namespace phs::elf
