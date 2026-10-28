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

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <span>
#include <type_traits>

#include "support/common.hpp"

namespace phs {

// A vector of trivially copyable values that stores up to N of them inline,
// so the short candidate lists the checker builds on every lookup do not
// touch the heap. Supports the subset of std::vector the checker uses.
template <typename T, usize N>
class SmallVec {
    static_assert(std::is_trivially_copyable_v<T> && N > 0);
public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    SmallVec() = default;
    SmallVec(std::initializer_list<T> values) { append(values.begin(), values.end()); }
    explicit SmallVec(std::span<const T> values) { append(values.begin(), values.end()); }
    template <std::input_iterator It>
    SmallVec(It first, It last) { append(first, last); }
    SmallVec(const SmallVec& other) { append(other.begin(), other.end()); }
    SmallVec(SmallVec&& other) noexcept { steal(other); }
    SmallVec& operator=(const SmallVec& other) {
        if (this != &other) { size_ = 0; append(other.begin(), other.end()); }
        return *this;
    }
    SmallVec& operator=(SmallVec&& other) noexcept {
        if (this != &other) { release(); steal(other); }
        return *this;
    }
    ~SmallVec() { release(); }

    T* data() { return heap_ ? heap_ : inline_; }
    const T* data() const { return heap_ ? heap_ : inline_; }
    iterator begin() { return data(); }
    iterator end() { return data() + size_; }
    const_iterator begin() const { return data(); }
    const_iterator end() const { return data() + size_; }
    std::reverse_iterator<iterator> rbegin() { return std::reverse_iterator(end()); }
    std::reverse_iterator<iterator> rend() { return std::reverse_iterator(begin()); }
    std::reverse_iterator<const_iterator> rbegin() const { return std::reverse_iterator(end()); }
    std::reverse_iterator<const_iterator> rend() const { return std::reverse_iterator(begin()); }
    usize size() const { return size_; }
    bool empty() const { return size_ == 0; }
    T& operator[](usize i) { return data()[i]; }
    const T& operator[](usize i) const { return data()[i]; }
    T& front() { PHS_ASSERT(size_); return data()[0]; }
    const T& front() const { PHS_ASSERT(size_); return data()[0]; }
    T& back() { PHS_ASSERT(size_); return data()[size_ - 1]; }
    const T& back() const { PHS_ASSERT(size_); return data()[size_ - 1]; }
    operator std::span<const T>() const { return {data(), size_}; }

    void clear() { size_ = 0; }
    void reserve(usize count) { if (count > capacity_) grow(count); }
    void push_back(T value) {
        if (size_ == capacity_) grow(capacity_ * 2);
        data()[size_++] = value;
    }
    template <typename... Args>
    T& emplace_back(Args&&... args) { push_back(T(std::forward<Args>(args)...)); return back(); }
    void pop_back() { PHS_ASSERT(size_); --size_; }
    template <std::input_iterator It>
    void assign(It first, It last) { size_ = 0; append(first, last); }
    template <std::input_iterator It>
    iterator insert(const_iterator at, It first, It last) {
        const usize offset = at - begin(), tail = size_ - offset;
        SmallVec moved(begin() + offset, end());
        size_ = offset;
        append(first, last);
        append(moved.begin(), moved.begin() + tail);
        return begin() + offset;
    }
    iterator erase(const_iterator first, const_iterator last) {
        const usize from = first - begin(), to = last - begin();
        std::copy(begin() + to, end(), begin() + from);
        size_ -= to - from;
        return begin() + from;
    }
    iterator erase(const_iterator at) { return erase(at, at + 1); }

    friend bool operator==(const SmallVec& a, const SmallVec& b) { return std::ranges::equal(a, b); }

private:
    template <typename It>
    void append(It first, It last) {
        if constexpr (std::forward_iterator<It>) reserve(size_ + static_cast<usize>(std::distance(first, last)));
        for (; first != last; ++first) push_back(*first);
    }
    void grow(usize count) {
        count = std::max(count, capacity_ * 2);
        T* heap = std::allocator<T>{}.allocate(count);
        std::copy(begin(), end(), heap);
        release();
        heap_ = heap;
        capacity_ = count;
    }
    void release() {
        if (heap_) std::allocator<T>{}.deallocate(heap_, capacity_);
        heap_ = nullptr;
        capacity_ = N;
    }
    void steal(SmallVec& other) {
        size_ = other.size_;
        if (other.heap_) {
            heap_ = other.heap_; capacity_ = other.capacity_;
            other.heap_ = nullptr; other.capacity_ = N;
        } else std::copy(other.inline_, other.inline_ + other.size_, inline_);
        other.size_ = 0;
    }

    T* heap_ = nullptr;
    usize size_ = 0;
    usize capacity_ = N;
    T inline_[N];
};

// Removes the values matching pred, like std::erase_if.
template <typename T, usize N, typename Pred>
usize erase_if(SmallVec<T, N>& values, Pred pred) {
    const auto removed = std::ranges::remove_if(values, pred);
    const usize count = removed.size();
    values.erase(removed.begin(), removed.end());
    return count;
}

}  // namespace phs
