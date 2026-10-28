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
#include <compare>
#include <deque>
#include <expected>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "support/common.hpp"

namespace phs {

struct FileId {
    u32 value = 0;
    friend auto operator<=>(FileId, FileId) = default;
};

struct Span {
    FileId file;
    u32 lo = 0;
    u32 hi = 0;

    static Span join(Span a, Span b) {
        PHS_ASSERT(a.file == b.file && a.lo <= a.hi && b.lo <= b.hi);
        return {a.file, std::min(a.lo, b.lo), std::max(a.hi, b.hi)};
    }
};

struct LineCol {
    u32 line;  // 1-based
    u32 col;   // 1-based, counted in Unicode scalar values
};

struct SourceFile {
    std::string path;
    std::string text;
    std::vector<u32> line_starts;
};

enum class SourceErrorKind : u8 { Metadata, NotRegularFile, Open, Read, ChangedDuringRead, TooLarge, TooManyFiles };

struct SourceError {
    SourceErrorKind kind;
    std::error_code cause;
    u32 limit = 0;  // bytes for TooLarge, file count for TooManyFiles

    std::string message() const;
};

struct SourceLimits {
    // Leave room for the EOF position and 1-based line/column numbers.
    static constexpr u32 kMaxFileBytes = std::numeric_limits<u32>::max() - 1;
    u32 max_file_bytes = 64 * 1024 * 1024;
    u32 max_files = std::numeric_limits<u32>::max();
};

class SourceManager {
public:
    explicit SourceManager(SourceLimits limits = {}) : limits_(limits) {
        PHS_ASSERT(limits.max_file_bytes <= SourceLimits::kMaxFileBytes);
    }

    // Limits apply to raw bytes before BOM/line-ending normalization. Failure
    // leaves the manager unchanged; IDs are assigned only on success.
    [[nodiscard]] std::expected<FileId, SourceError> load(const std::filesystem::path& path);
    [[nodiscard]] std::expected<FileId, SourceError> add(std::string path, std::string_view text);

    // References and text views survive subsequent load/add calls.
    const SourceFile& file(FileId id) const {
        PHS_ASSERT(id.value < files_.size());
        return files_[id.value];
    }
    LineCol line_col(FileId id, u32 offset) const;
    std::string_view line_text(FileId id, u32 line) const;

private:
    std::expected<void, SourceError> check_limits(std::uintmax_t bytes) const;

    SourceLimits limits_;
    std::deque<SourceFile> files_;
};

}  // namespace phs
