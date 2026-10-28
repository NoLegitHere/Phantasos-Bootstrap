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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <source_location>
#include <string_view>

namespace phs {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using usize = std::size_t;

// Internal compiler error: report and abort (stage0 is built without exceptions).
[[noreturn]] inline void fatal(std::string_view msg, std::source_location loc = std::source_location::current()) {
    std::println(stderr, "stage0 internal error: {} ({}:{})", msg, loc.file_name(), loc.line());
    std::abort();
}

// Unlike std::unreachable(), reaching this is a diagnosed abort, not undefined
// behavior: a compiler bug should fail loudly, never miscompile.
[[noreturn]] inline void unreachable(std::source_location loc = std::source_location::current()) {
    fatal("unreachable code reached", loc);
}

}  // namespace phs

// A macro only because C++ cannot stringify the asserted expression otherwise.
#define PHS_ASSERT(cond)                                        \
    do {                                                        \
        if (!(cond)) ::phs::fatal("assertion failed: " #cond);  \
    } while (0)
