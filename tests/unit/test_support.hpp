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

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <source_location>
#include <string_view>
#include <utility>

namespace phs::test {
inline void check(bool condition, std::string_view expression,
                  std::source_location location = std::source_location::current()) {
    if (condition) return;
    std::println(stderr, "{}:{}: check failed: {}", location.file_name(), location.line(), expression);
    std::exit(EXIT_FAILURE);
}
}  // namespace phs::test

#define CHECK(...) ::phs::test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)

namespace phs::test {
// Each invocation owns a fresh directory, including concurrent CTest runs.
struct TempDirectory {
    std::filesystem::path path;

    TempDirectory() {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        CHECK(!ec);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = base / std::format("phantasos-test-{}-{}", stamp, attempt);
            if (std::filesystem::create_directory(candidate, ec)) {
                path = std::move(candidate);
                return;
            }
            CHECK(!ec || ec == std::errc::file_exists);
        }
        CHECK(false);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace phs::test
