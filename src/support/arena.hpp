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

#include <concepts>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "support/common.hpp"

namespace phs {

// Bump allocator for AST nodes, interned strings and other data that lives as
// long as the compilation. A thin typed facade over the standard monotonic
// resource. Objects are never destroyed individually, so only trivially
// destructible types may be placed here.
class Arena {
public:
    Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    template <typename T, typename... Args>
        requires (std::is_trivially_destructible_v<T> && std::constructible_from<T, Args...> &&
                  !std::is_const_v<T> && !std::is_volatile_v<T> && !std::is_array_v<T>)
    [[nodiscard]] T* make(Args&&... args) {
        return alloc_.new_object<T>(std::forward<Args>(args)...);
    }

    // Copies a contiguous range (e.g. a std::vector being built by the parser)
    // into arena storage.
    template <typename R>
        requires (std::ranges::contiguous_range<const R> && std::ranges::sized_range<const R> &&
                  std::is_trivially_destructible_v<std::ranges::range_value_t<const R>> &&
                  std::constructible_from<std::ranges::range_value_t<const R>,
                                          std::ranges::range_reference_t<const R>>)
    [[nodiscard]] std::span<std::ranges::range_value_t<const R>> copy(const R& range) {
        using T = std::ranges::range_value_t<const R>;
        const auto n = std::ranges::size(range);
        if (n == 0) return {};
        std::span<T> out{alloc_.allocate_object<T>(n), n};
        std::ranges::uninitialized_copy(range, out);
        return out;
    }

    [[nodiscard]] std::string_view copy_str(std::string_view s) {
        if (s.empty()) return {};
        return {copy(s).data(), s.size()};
    }

private:
    std::pmr::monotonic_buffer_resource resource_;
    std::pmr::polymorphic_allocator<> alloc_{&resource_};
};

}  // namespace phs
