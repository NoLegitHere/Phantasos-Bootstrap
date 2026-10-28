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

#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "codegen/elf.hpp"
#include "codegen/x86_64.hpp"
#include "frontend/session.hpp"
#include "mir_interpreter.hpp"

#if defined(__linux__) && defined(__x86_64__)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#define PHS_EXEC 1
#endif

using namespace phs;

namespace {

struct Built {
    FrontendSession session;
    const mir::Program* program = nullptr;
    std::vector<obj::Object> objects;
    std::vector<u8> image;

    explicit Built(std::string_view source) {
        const auto id = session.sources().add("elf.phs", source);
        CHECK(id);
        program = session.lower(*id);
        if (!program) session.diagnostics().render(stderr);
        CHECK(program);
        objects.push_back(x86_64::emit(*program));
        objects.push_back(x86_64::runtime(*program));
        auto linked = elf::link(objects, x86_64::kEntrySymbol);
        if (!linked) std::println(stderr, "{}", linked.error());
        CHECK(linked);
        image = std::move(*linked);
    }
};

u64 read(const std::vector<u8>& bytes, u64 offset, int size) {
    u64 value = 0;
    for (int i = 0; i < size; ++i) value |= u64{bytes[offset + i]} << (8 * i);
    return value;
}

constexpr std::string_view kFib = R"(
fun fib(n: Int): Int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}

fun sumTo(limit: Int): Int {
    var total = 0
    var i = 1
    while i <= limit {
        if i % 3 == 0 || i % 5 == 0 { total += i }
        i += 1
    }
    return total
}

fun main(): Int {
    return fib(10) - sumTo(9) + 10
}
)";

void headers() {
    Built built(kFib);
    const auto& image = built.image;
    CHECK(image.size() > 176);
    CHECK(std::memcmp(image.data(), "\x7F" "ELF\x02\x01\x01", 7) == 0);
    CHECK(read(image, 16, 2) == 2);   // ET_EXEC
    CHECK(read(image, 18, 2) == 62);  // EM_X86_64
    CHECK(read(image, 56, 2) == 2);   // code and read-only data
    CHECK(read(image, 64, 4) == 1);   // PT_LOAD
    CHECK(read(image, 68, 4) == 5);   // R+X
    CHECK(read(image, 72, 8) == 0);
    CHECK(read(image, 80, 8) == elf::kBaseAddress);
    const u64 load_size = read(image, 96, 8);
    CHECK(read(image, 104, 8) == load_size);  // memsz == filesz

    // The entry is _start, which the runtime object places first.
    const u64 runtime_base = (built.objects[0].text.size() + 15) / 16 * 16;
    CHECK(read(image, 24, 8) == elf::kBaseAddress + 176 + runtime_base);
    CHECK(load_size == 176 + runtime_base + built.objects[1].text.size());

    // The runtime's panic messages are in a page-aligned, read-only segment
    // that is not executable.
    const u64 data = 64 + 56;
    CHECK(read(image, data, 4) == 1 && read(image, data + 4, 4) == 4);  // PT_LOAD, R
    const u64 data_offset = read(image, data + 8, 8);
    CHECK(data_offset % 0x1000 == 0 && data_offset >= load_size);
    CHECK(read(image, data + 16, 8) == elf::kBaseAddress + data_offset);
    CHECK(read(image, data + 32, 8) == built.objects[1].rodata.size());
    const std::string_view rodata(reinterpret_cast<const char*>(image.data() + data_offset), built.objects[1].rodata.size());
    CHECK(rodata == "panic: arithmetic overflow\npanic: division by zero\npanic: cannot write to standard output\n"
                    "\ntrue\nfalse\n");

    // .symtab names every defined symbol, locals first.
    const u64 shoff = read(image, 40, 8);
    CHECK(read(image, 60, 2) == 7 && read(image, 62, 2) == 6);
    const u64 rodata_section = shoff + 2 * 64, bss = shoff + 3 * 64, symtab = shoff + 4 * 64, strtab = shoff + 5 * 64;
    CHECK(read(image, rodata_section + 8, 8) == 2 && read(image, rodata_section + 24, 8) == data_offset);
    CHECK(read(image, bss + 4, 4) == 8 && read(image, bss + 32, 8) == 0);  // an empty SHT_NOBITS .bss
    CHECK(read(image, symtab + 4, 4) == 2 && read(image, strtab + 4, 4) == 3);
    CHECK(read(image, symtab + 40, 4) == 5 && read(image, symtab + 44, 4) == 7);  // link, first global
    const u64 symbols = read(image, symtab + 24, 8), strings = read(image, strtab + 24, 8);
    std::vector<std::string> names;
    std::vector<u64> info;
    for (u64 at = symbols + 24; at < symbols + read(image, symtab + 32, 8); at += 24) {
        names.emplace_back(reinterpret_cast<const char*>(image.data() + strings + read(image, at, 4)));
        info.push_back(read(image, at + 4, 1) | read(image, at + 6, 2) << 8);
    }
    for (usize i = 0; i < 6; ++i)  // STB_LOCAL, STT_OBJECT, .rodata
        CHECK(names[i] == std::format(".Lstr{}", i) && info[i] == (0 << 4 | 1 | 2 << 8));
    for (const auto* expected : {"_P3fib", "_P5sumTo", "_P4main", "_start", "__phantasos_trap", "_P$write",
                                 "_P$print_string", "_P$print_int", "_P$print_uint", "_P$print_bool"}) {
        const auto found = std::ranges::find(names, expected);
        CHECK(found != names.end() && info[found - names.begin()] == (1 << 4 | 2 | 1 << 8));
    }
}

// Literals are interned per object: identical bytes share a local symbol.
void literals() {
    obj::Object object;
    const u32 hello = object.literal("hello");
    const u32 world = object.literal("world");
    CHECK(object.literal("hello") == hello && hello != world);
    CHECK(object.literal("") != hello);
    CHECK(object.rodata == std::vector<u8>{'h', 'e', 'l', 'l', 'o', 'w', 'o', 'r', 'l', 'd'});
    const auto& symbol = object.symbols[world];
    CHECK(symbol.defined && symbol.local && symbol.section == obj::Section::Rodata);
    CHECK(symbol.offset == 5 && symbol.size == 5 && symbol.name == ".Lstr1");
}

// Each object's relocations resolve its own local symbols, even when another
// object uses the same local name, and reach the data in the second segment.
void local_relocations() {
    const auto object = [](std::string_view bytes, bool entry) {
        obj::Object o;
        const u32 literal = o.literal(bytes);
        if (entry) o.symbols.push_back({"_start", true, 0, 7});
        o.text = {0x48, 0x8D, 0x35, 0, 0, 0, 0};  // lea rsi, [rip+literal]
        o.relocations.push_back({3, literal, -4});
        return o;
    };
    const std::array objects{object("first", true), object("second", false)};
    CHECK(objects[0].symbols[0].name == objects[1].symbols[0].name);
    const auto image = elf::link(objects, "_start");
    CHECK(image);
    const u64 data_offset = read(*image, 64 + 56 + 8, 8);
    for (const auto [code, data] : {std::pair<u64, u64>{0, 0}, {16, 16}}) {
        const i64 rel = static_cast<i32>(read(*image, 176 + code + 3, 4));
        const u64 target = elf::kBaseAddress + 176 + code + 7 + rel;
        CHECK(target == elf::kBaseAddress + data_offset + data);
    }
    CHECK(std::string_view(reinterpret_cast<const char*>(image->data() + data_offset), 5) == "first");
    CHECK(std::string_view(reinterpret_cast<const char*>(image->data() + data_offset + 16), 6) == "second");

    // Without data there is a single segment and an empty .rodata section.
    obj::Object code;
    code.text = {0xC3};
    code.symbols.push_back({"_start", true, 0, 1});
    const auto bare = elf::link(std::span(&code, 1), "_start");
    CHECK(bare && read(*bare, 56, 2) == 1 && read(*bare, read(*bare, 40, 8) + 2 * 64 + 32, 8) == 0);
}

void link_errors() {
    Built built(kFib);
    // The program alone leaves the trap entry undefined.
    const auto alone = elf::link(std::span(built.objects).first(1), x86_64::kEntrySymbol);
    CHECK(!alone && alone.error() == "undefined symbol '__phantasos_trap'");
    const std::array twice{built.objects[0], built.objects[1], built.objects[1]};
    const auto duplicate = elf::link(twice, x86_64::kEntrySymbol);
    CHECK(!duplicate && duplicate.error() == "symbol '_start' is defined more than once");
    const auto no_entry = elf::link(built.objects, "_begin");
    CHECK(!no_entry && no_entry.error() == "undefined entry symbol '_begin'");
}

void runtime_listing() {
    Built built(kFib);
    std::string listing;
    (void)x86_64::runtime(*built.program, &listing);
    CHECK(listing.contains("_start:  ; process entry"));
    CHECK(listing.contains("call _P4main"));
    CHECK(listing.contains("mov edi, eax"));
    CHECK(listing.contains("mov edi, 101"));
    CHECK(listing.contains("lea rsi, [rip+.Lstr1]"));
    CHECK(listing.contains(".rodata\n    0000  .Lstr0:  db \"panic: arithmetic overflow\\n\""));
    CHECK(listing.contains("    001b  .Lstr1:  db \"panic: division by zero\\n\""));
    CHECK(listing.contains("_P$write:  ; writes rsi bytes at rdi to stdout"));
    CHECK(listing.contains("cmp rax, -4"));
    CHECK(listing.contains("mov byte [r8], dl"));
    CHECK(listing.contains("call _P$print_decimal"));

    Built unit("fun main() {}\n");
    (void)x86_64::runtime(*unit.program, &listing);
    CHECK(listing.contains("xor edi, edi") && !listing.contains("mov edi, eax"));
    // Only programs whose closures capture get the allocator.
    CHECK(!listing.contains("_P$alloc") && unit.objects[1].bss == 0);
}

constexpr std::string_view kAdder = R"(
fun makeAdder(n: Int): fun(Int) -> Int = { x -> x + n }
fun main(): Int = makeAdder(40)(2)
)";

// The allocator's state is in a writable .bss segment with no file bytes,
// after the read-only data; the code moves to make room for its header.
void bss() {
    Built built(kAdder);
    const auto& image = built.image;
    CHECK(read(image, 56, 2) == 3);
    const u64 text_offset = 64 + 3 * 56 + 8;
    CHECK(read(image, 64 + 8, 8) == 0 && read(image, 80 + 8, 8) == elf::kBaseAddress);
    const u64 data = 64 + 56, writable = 64 + 2 * 56;
    const u64 data_end = read(image, data + 8, 8) + read(image, data + 32, 8);
    CHECK(read(image, writable, 4) == 1 && read(image, writable + 4, 4) == 6);  // PT_LOAD, R+W
    const u64 bss_offset = read(image, writable + 8, 8);
    CHECK(bss_offset % 0x1000 == 0 && bss_offset >= data_end);
    CHECK(read(image, writable + 16, 8) == elf::kBaseAddress + bss_offset);
    CHECK(read(image, writable + 32, 8) == 0 && read(image, writable + 40, 8) == 16);  // filesz, memsz
    const u64 shoff = read(image, 40, 8);
    const u64 text = shoff + 64, section = shoff + 3 * 64;
    CHECK(read(image, text + 24, 8) == text_offset);
    CHECK(read(image, section + 4, 4) == 8 && read(image, section + 8, 8) == 3);  // SHT_NOBITS, SHF_WRITE | SHF_ALLOC
    CHECK(read(image, section + 16, 8) == elf::kBaseAddress + bss_offset && read(image, section + 32, 8) == 16);
    CHECK(built.objects[0].bss == 0 && built.objects[1].bss == 16);
    std::string listing;
    (void)x86_64::runtime(*built.program, &listing);
    CHECK(listing.contains("_P$alloc:  ; rdi: size; returns a zeroed, 16-byte aligned block in rax"));
    CHECK(listing.contains("mov rax, qword [rip+_P$heap]") && listing.contains("mov qword [rip+_P$heap+8], rcx"));
    CHECK(listing.contains("mov eax, 9") && listing.contains("syscall"));
}

#ifdef PHS_EXEC
struct Exit {
    int status = -1;
    std::string out, err;
};

std::string drain(int fd) {
    std::string text;
    char buffer[256];
    for (ssize_t n; (n = ::read(fd, buffer, sizeof buffer)) > 0;) text.append(buffer, static_cast<usize>(n));
    close(fd);
    return text;
}

// Writes the image to a file, runs it and collects its exit status, stdout
// and stderr. With full, stdout is /dev/full, so every write to it fails; a
// nonzero memory limits the process's address space to that many bytes.
Exit run(const std::vector<u8>& image, bool full = false, rlim_t memory = 0) {
    test::TempDirectory dir;
    const auto path = dir.path / "program";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        CHECK(out.good());
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    int out_fds[2], err_fds[2];
    CHECK(pipe(out_fds) == 0 && pipe(err_fds) == 0);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        const rlimit limit{memory, memory};
        if (memory && setrlimit(RLIMIT_AS, &limit) != 0) _exit(125);
        if (full) {
            const int device = open("/dev/full", O_WRONLY);
            if (device < 0) _exit(126);
            dup2(device, 1);
        } else dup2(out_fds[1], 1);
        dup2(err_fds[1], 2);
        for (const int fd : {out_fds[0], out_fds[1], err_fds[0], err_fds[1]}) close(fd);
        const std::string file = path.string();
        char* argv[] = {const_cast<char*>(file.c_str()), nullptr};
        char* envp[] = {nullptr};
        execve(file.c_str(), argv, envp);
        _exit(127);
    }
    close(out_fds[1]);
    close(err_fds[1]);
    // Test programs write little, so neither pipe fills while the other drains.
    Exit result;
    result.out = drain(out_fds[0]);
    result.err = drain(err_fds[0]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    result.status = WEXITSTATUS(status);
    return result;
}

void expect(std::string_view source, int status, std::string_view err = "", std::string_view out = "") {
    Built built(source);
    const auto result = run(built.image);
    if (result.status != status || result.err != err || result.out != out) {
        std::println(stderr, "{}\nexpected exit {} '{}' '{}', got {} '{}' '{}'", source, status, out, err, result.status,
                     result.out, result.err);
        CHECK(false);
    }
}

// The executable's output matches the MIR interpreter's.
void prints(std::string_view source, std::string_view out) {
    Built built(source);
    const auto reference = test::Interpreter(*built.program).run();
    CHECK(!reference.trapped && reference.output == out);
    expect(source, static_cast<int>(reference.value & 0xFF), "", out);
}

void output() {
    prints(R"(
fun main() {
    print("Hello, world!")
    print(value: "\u{1F30D} \"quoted\"\ttab")
    print("")
}
)", "Hello, world!\n\U0001F30D \"quoted\"\ttab\n\n");
    prints(R"(
fun main() {
    print(0)
    print(7)
    print(-7)
    print(10)
    print(-1000000)
    print(9223372036854775807)
    print(-9223372036854775807 - 1)
    let zero: UInt = 0
    let top: UInt = 18446744073709551615
    print(zero)
    print(top)
    print(true)
    print(1 > 2)
}
)", "0\n7\n-7\n10\n-1000000\n9223372036854775807\n-9223372036854775808\n0\n18446744073709551615\ntrue\nfalse\n");
    // Arguments evaluate before the write, left to right with other calls.
    prints(R"(
fun shout(text: own String): String {
    print("evaluating")
    return text
}
fun main(): Int {
    print(shout("done"))
    var i = 0
    while i < 3 {
        print(i * i)
        i += 1
    }
    return 3
}
)", "evaluating\ndone\n0\n1\n4\n");
    // Output written before a panic stays written.
    expect("fun main(): Int {\n    print(\"before\")\n    return 1 / 0\n}\n", 101, "panic: division by zero\n", "before\n");
    // A failed write panics instead of losing output silently.
    Built full("fun main(): Int {\n    print(\"lost\")\n    return 0\n}\n");
    const auto failed = run(full.image, true);
    CHECK(failed.status == 101 && failed.err == "panic: cannot write to standard output\n" && failed.out.empty());
}

void executables() {
    expect(kFib, 42);
    expect("fun main() {}\n", 0);
    expect("fun main(): Int = 0\n", 0);
    // The kernel keeps the low 8 bits of the status.
    expect("fun main(): Int = 300\n", 44);
    expect("fun main(): Int = -1\n", 255);
    expect(R"(
fun add(a: Int, b: Int, c: Int, d: Int, e: Int, f: Int, g: Int, h: Int): Int = a + b + c + d + e + f + g * h
fun main(): Int = add(1, 2, 3, 4, 5, 6, 7, 8)
)", 77);
    expect(R"(
fun depth(n: Int): Int {
    if n == 0 { return 0 }
    return depth(n - 1) + 1
}
fun main(): Int = depth(10000) - 9990
)", 10);
    expect(R"(
fun main(): Int {
    var x: Int8 = 120
    var steps = 0
    while true {
        x = x &+ 3
        steps += 1
        if x < 0 { break }
    }
    return steps
}
)", 3);
    expect("fun zero(): Int = 0\nfun main(): Int = 7 / zero()\n", 101, "panic: division by zero\n");
    expect("fun zero(): UInt8 = 0\nfun main(): Int = Int(UInt8(7) % zero())\n", 101, "panic: division by zero\n");
    expect("fun big(): Int = 9223372036854775807\nfun main(): Int = big() + 1\n", 101, "panic: arithmetic overflow\n");
    expect("fun minus(): Int = -1\nfun main(): Int = (-9223372036854775807 - 1) / minus()\n", 101,
           "panic: arithmetic overflow\n");
    expect("fun main() {\n    let x: UInt = 0\n    let y = x - 1\n}\n", 101, "panic: arithmetic overflow\n");
    // A trap inside a Unit main still panics; short-circuiting skips one.
    expect("fun boom(): Bool = 1 / 0 == 1\nfun main(): Int {\n    let a = false && boom()\n    return 5\n}\n", 5);
}

void closures() {
    expect(kAdder, 42);
    // Environments outlive their frames, and allocation spans several chunks.
    prints(R"(
struct Big(a: Int, b: Int, c: Int)
fun keep(b: Big): fun() -> Int = { -> b.a + b.b + b.c }
fun main(): Int {
    var total = 0
    var i = 0
    while i < 100000 {
        total += keep(Big(a: i, b: 1, c: 2))()
        i += 1
    }
    print(total)
    let name = "closure"
    let twice = { x: Int ->
        let copy = name.clone()
        { -> print(copy); x * 2 }
    }
    return twice(21)()
}
)", "5000250000\nclosure\n");
    // Without memory to map, allocation panics.
    Built hungry(R"(
struct Big(a: Int, b: Int, c: Int)
fun main(): Int {
    var i = 0
    loop {
        let b = Big(a: i, b: i, c: i)
        let f = { -> b.a }
        i += f() - i + 1
    }
}
)");
    const auto starved = run(hungry.image, false, rlim_t{16} << 20);
    CHECK(starved.status == 101 && starved.err == "panic: out of memory\n" && starved.out.empty());
}
#endif

}  // namespace

int main() {
    headers();
    link_errors();
    literals();
    local_relocations();
    runtime_listing();
    bss();
#ifdef PHS_EXEC
    executables();
    output();
    closures();
#endif
    std::println("elf tests passed");
}
