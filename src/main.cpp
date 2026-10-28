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

// phantasos-stage0: the C++23 bootstrap compiler for Phantasos-Core.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codegen/elf.hpp"
#include "codegen/x86_64.hpp"
#include "frontend/session.hpp"
#include "syntax/ast_dump.hpp"
#include "syntax/lexer.hpp"
#include "syntax/token.hpp"

namespace {

constexpr std::string_view kUsage =
    "usage: phantasos-stage0 <command> [args]\n"
    "\n"
    "commands:\n"
    "  lex <file.phs>     print the token stream\n"
    "  parse <file.phs>   print the syntax tree\n"
    "  check <file.phs>   check syntax, semantics, names, types and flow\n"
    "  gate <file.phs>    check, then check that stage0 can generate code for it\n"
    "  mir <file.phs>     print the MIR of a file that passes gate\n"
    "  asm <file.phs>     print its x86_64 code as an assembly listing\n"
    "  build <file.phs> [-o <out>]\n"
    "                     compile it to a static Linux x86_64 executable\n"
    "                     (default output: the file name without .phs)\n"
    "  version            print the version\n"
    "  help               print this message\n";

int run_frontend(std::string_view cmd, std::string_view path) {
    phs::FrontendSession session;
    const auto file = session.sources().load(path);
    if (!file) {
        std::println(stderr, "error: cannot read '{}': {}", path, file.error().message());
        return 2;
    }
    if (cmd == "lex") {
        const auto tokens = phs::lex_file(session.sources(), *file, session.names(), session.diagnostics());
        std::print("{}", phs::dump_tokens(tokens, session.names(), session.sources()));
    } else if (cmd == "parse")
        std::print("{}", phs::dump_ast(*session.parse(*file), session.names()));
    else if (cmd == "check")
        (void)session.check(*file);
    else if (cmd == "gate")
        (void)session.check_codegen(*file);
    else if (const auto* program = session.lower(*file)) {
        if (cmd == "mir") std::print("{}", phs::mir::dump(*program));
        else {
            std::string listing;
            (void)phs::x86_64::emit(*program, &listing);
            std::print("{}", listing);
        }
    }
    std::fflush(stdout);
    session.diagnostics().render(stderr);
    return session.diagnostics().has_errors(*file) ? 1 : 0;
}

int build(std::string_view path, std::filesystem::path out) {
    phs::FrontendSession session;
    const auto file = session.sources().load(path);
    if (!file) {
        std::println(stderr, "error: cannot read '{}': {}", path, file.error().message());
        return 2;
    }
    const auto* program = session.lower(*file);
    session.diagnostics().render(stderr);
    if (!program) return 1;
    const std::vector objects{phs::x86_64::emit(*program), phs::x86_64::runtime(*program)};
    const auto image = phs::elf::link(objects, phs::x86_64::kEntrySymbol);
    if (!image) {
        std::println(stderr, "error: cannot link '{}': {}", path, image.error());
        return 1;
    }
    if (out.empty()) out = std::filesystem::path(path).stem();
    // Never truncate the source, including through a link to it.
    std::error_code same_ec;
    if (std::filesystem::equivalent(out, path, same_ec) && !same_ec) {
        std::println(stderr, "error: output '{}' is the source file", out.string());
        return 2;
    }
    {
        std::ofstream stream(out, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(image->data()), static_cast<std::streamsize>(image->size()));
        stream.close();  // a failed flush only shows up here
        if (stream.fail()) {
            std::println(stderr, "error: cannot write '{}'", out.string());
            return 2;
        }
    }
    using std::filesystem::perms;
    std::error_code ec;
    std::filesystem::permissions(out, perms::owner_exec | perms::group_exec | perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
    if (ec) {
        std::println(stderr, "error: cannot make '{}' executable: {}", out.string(), ec.message());
        return 2;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() < 2) {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    const std::string_view cmd = args[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        std::print("{}", kUsage);
        return 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::println("phantasos-stage0 {}", PHS_VERSION);
        return 0;
    }
    if (cmd == "lex" || cmd == "parse" || cmd == "check" || cmd == "gate" || cmd == "mir" || cmd == "asm") {
        if (args.size() != 3) {
            std::print(stderr, "{}", kUsage);
            return 2;
        }
        return run_frontend(cmd, args[2]);
    }
    if (cmd == "build") {
        if (args.size() == 3) return build(args[2], {});
        if (args.size() == 5 && std::string_view(args[3]) == "-o") return build(args[2], args[4]);
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    std::print(stderr, "error: unknown command '{}'\n\n{}", cmd, kUsage);
    return 2;
}
