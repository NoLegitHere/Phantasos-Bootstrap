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

#include <type_traits>
#include <utility>

namespace phs {

// Temporarily replace a value, restoring it on every exit from the scope.
template <typename T>
    requires (std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>)
class [[nodiscard]] ScopedValue {
public:
    ScopedValue(T& value, T replacement) noexcept
        : value_(value), previous_(std::exchange(value, std::move(replacement))) {}
    ~ScopedValue() { value_ = std::move(previous_); }

    ScopedValue(const ScopedValue&) = delete;
    ScopedValue& operator=(const ScopedValue&) = delete;

private:
    T& value_;
    T previous_;
};

}  // namespace phs
