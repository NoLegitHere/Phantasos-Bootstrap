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

#include "support/diag.hpp"

#include <algorithm>
#include <print>
#include <utility>

#include "support/utf8.hpp"

namespace phs {

namespace {

constexpr std::string_view severity_name(Severity s) {
    switch (s) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Note: return "note";
    }
    unreachable();
}

// Invalid UTF-8 bytes become U+FFFD so the output is always valid UTF-8. Each
// replacement is one column, matching utf8_step() used for caret placement.
std::string sanitize_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    while (!s.empty()) {
        const auto c = utf8_decode(s);
        const u32 len = c ? c->len : 1;
        out += c ? s.substr(0, len) : "\uFFFD";
        s.remove_prefix(len);
    }
    return out;
}

}  // namespace

std::vector<const Diagnostic*> DiagEngine::in_source_order() const {
    std::vector<const Diagnostic*> sorted;
    sorted.reserve(diags_.size());
    for (const Diagnostic& d : diags_) sorted.push_back(&d);
    std::ranges::stable_sort(sorted, {}, [](const Diagnostic* d) { return std::pair{d->span.file, d->span.lo}; });
    return sorted;
}

void DiagEngine::render(std::FILE* out) const {
    for (const Diagnostic* d : in_source_order()) render_one(out, *d);
}

void DiagEngine::render_one(std::FILE* out, const Diagnostic& d) const {
    const LineCol lc = sm_.line_col(d.span.file, d.span.lo);
    std::println(out, "{}:{}:{}: {}[{}]: {}", sm_.file(d.span.file).path, lc.line, lc.col, severity_name(d.severity),
                 diag_code(d.code), d.message);
    render_snippet(out, d.span);
    for (const DiagNote& n : d.notes) {
        const LineCol nlc = sm_.line_col(n.span.file, n.span.lo);
        std::println(out, "{}:{}:{}: note: {}", sm_.file(n.span.file).path, nlc.line, nlc.col, n.message);
        render_snippet(out, n.span);
    }
}

void DiagEngine::render_snippet(std::FILE* out, Span span) const {
    const LineCol lc = sm_.line_col(span.file, span.lo);
    const std::string_view line = sm_.line_text(span.file, lc.line);
    const std::string_view text = sm_.file(span.file).text;
    const auto line_start = static_cast<u32>(line.data() - text.data());
    const auto line_end = line_start + static_cast<u32>(line.size());

    // Keep tabs so the carets line up under the echoed source.
    std::string indent;
    for (u32 k = line_start; k < std::min(span.lo, line_end); k += utf8_step(text.substr(k, line_end - k)))
        indent += text[k] == '\t' ? '\t' : ' ';
    const u32 hi = std::min(span.hi, line_end);
    usize width = 0;
    for (u32 k = span.lo; k < hi; k += utf8_step(text.substr(k, hi - k))) ++width;

    std::println(out, "{:5} | {}", lc.line, sanitize_utf8(line));
    std::println(out, "      | {}{}", indent, std::string(std::max<usize>(width, 1), '^'));
}

}  // namespace phs
