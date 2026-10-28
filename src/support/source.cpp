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

#include "support/source.hpp"

#include <format>
#include <fstream>
#include <system_error>

#include "support/utf8.hpp"

namespace phs {

std::string SourceError::message() const {
    switch (kind) {
        case SourceErrorKind::Metadata: return cause.message();
        case SourceErrorKind::NotRegularFile: return "source is not a regular file";
        case SourceErrorKind::Open: return "could not open source file";
        case SourceErrorKind::Read: return "could not read complete source file";
        case SourceErrorKind::ChangedDuringRead: return "source file grew while being read; retry the command";
        case SourceErrorKind::TooLarge: return std::format("source file exceeds input limit of {} bytes", limit);
        case SourceErrorKind::TooManyFiles: return std::format("source file count exceeds limit of {}", limit);
    }
    unreachable();
}

std::expected<void, SourceError> SourceManager::check_limits(std::uintmax_t bytes) const {
    if (files_.size() >= limits_.max_files)
        return std::unexpected(SourceError{SourceErrorKind::TooManyFiles, {}, limits_.max_files});
    if (bytes > limits_.max_file_bytes)
        return std::unexpected(SourceError{SourceErrorKind::TooLarge, {}, limits_.max_file_bytes});
    return {};
}

std::expected<FileId, SourceError> SourceManager::load(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) return std::unexpected(SourceError{SourceErrorKind::Metadata, ec});
    if (!std::filesystem::is_regular_file(status))
        return std::unexpected(SourceError{SourceErrorKind::NotRegularFile});
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return std::unexpected(SourceError{SourceErrorKind::Metadata, ec});
    if (auto checked = check_limits(size); !checked) return std::unexpected(checked.error());
    // Guard the signed stream count even on hosts with a narrower streamsize.
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        return std::unexpected(SourceError{SourceErrorKind::TooLarge, {},
                                          static_cast<u32>(std::numeric_limits<std::streamsize>::max())});
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::unexpected(SourceError{SourceErrorKind::Open});
    std::string text(static_cast<usize>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(size)))
        return std::unexpected(SourceError{SourceErrorKind::Read});
    if (in.peek() != std::ifstream::traits_type::eof())
        return std::unexpected(SourceError{SourceErrorKind::ChangedDuringRead});
    if (in.bad()) return std::unexpected(SourceError{SourceErrorKind::Read});
    return add(path.string(), text);
}

std::expected<FileId, SourceError> SourceManager::add(std::string path, std::string_view text) {
    if (auto checked = check_limits(text.size()); !checked) return std::unexpected(checked.error());
    // Normalize line endings (CRLF and lone CR become LF) and drop a UTF-8 BOM
    // so every later stage sees a single canonical form.
    std::string_view in = text;
    if (in.starts_with("\xEF\xBB\xBF")) in.remove_prefix(3);
    std::string norm;
    norm.reserve(in.size());
    for (usize i = 0; i < in.size(); ++i) {
        if (in[i] != '\r') {
            norm.push_back(in[i]);
            continue;
        }
        norm.push_back('\n');
        if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
    }

    SourceFile sf{.path = std::move(path), .text = std::move(norm), .line_starts = {0}};
    for (u32 k = 0; k < sf.text.size(); ++k)
        if (sf.text[k] == '\n') sf.line_starts.push_back(k + 1);
    files_.push_back(std::move(sf));
    return FileId{static_cast<u32>(files_.size() - 1)};
}

LineCol SourceManager::line_col(FileId id, u32 offset) const {
    const SourceFile& f = file(id);
    const auto line = static_cast<u32>(
        std::ranges::distance(f.line_starts.begin(), std::ranges::upper_bound(f.line_starts, offset)));
    const u32 end = std::min(offset, static_cast<u32>(f.text.size()));
    u32 col = 1;
    for (u32 k = f.line_starts[line - 1]; k < end; k += utf8_step(std::string_view(f.text).substr(k))) ++col;
    return {line, col};
}

std::string_view SourceManager::line_text(FileId id, u32 line) const {
    const SourceFile& f = file(id);
    if (line == 0 || line > f.line_starts.size()) return {};
    const u32 start = f.line_starts[line - 1];
    const u32 end = line < f.line_starts.size() ? f.line_starts[line] - 1 : static_cast<u32>(f.text.size());
    return std::string_view(f.text).substr(start, end - start);
}

}  // namespace phs
