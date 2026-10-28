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

// Compile AND link the facilities used by stage0, with its real build flags.
#include <array>
#include <charconv>
#include <expected>
#include <format>
#include <memory_resource>
#include <print>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <version>

#if !defined(__cpp_lib_print) || __cpp_lib_print < 202207L
#error stage0 requires C++23 std::print
#endif
#if !defined(__cpp_lib_ranges_to_container) || __cpp_lib_ranges_to_container < 202202L
#error stage0 requires C++23 std::ranges::to
#endif
#if !defined(__cpp_lib_ranges_join_with) || __cpp_lib_ranges_join_with < 202202L
#error stage0 requires C++23 std::views::join_with
#endif
#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error stage0 requires C++23 std::expected
#endif

int main() {
    using namespace std::literals;
    const std::array words{"one"sv, "two"sv};
    auto text = words | std::views::transform([](std::string_view word) { return std::format("{}", word); }) |
                std::views::join_with(","sv) | std::ranges::to<std::string>();
    std::expected<std::string, std::string> result{std::move(text)};
    constexpr auto digits = "1.25"sv;
    double number = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::polymorphic_allocator<> allocator{&resource};
    auto* value = allocator.new_object<int>(42);
    const std::span<int> values(value, 1);
    enum class Mode { Probe };
    std::println("{} {} {} {} {}", *result, number, values.front(), std::to_underlying(Mode::Probe),
                 std::source_location::current().line());
    return parsed.ec == std::errc{} ? 0 : 1;
}
