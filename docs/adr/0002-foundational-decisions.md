# ADR 0002: Foundational language and toolchain decisions

- Status: accepted; the Bootstrap row is superseded by [ADR 0003](0003-bootstrap-subset.md)

| Area | Decision |
|---|---|
| Memory | Single ownership with move by default. Opt-in `shared T` (ARC) and `weak T`. Second-class borrows (`borrow`, `inout`) checked at compile time, with no lifetime annotations. Deterministic `deinit`. |
| Subtyping | Single class inheritance. Classes are `final` by default (`open` allows subclassing). `abstract`, `sealed`, multiple `interface`s with default methods, and mandatory `override`. |
| Errors | Typed `throws(E)`, lowered to result returns (no unwinding). `panic` aborts with a backtrace. |
| Concurrency | `async`/`await`, structured concurrency, `actor`, and `Sendable` are part of v1. |
| Syntax | Brace-based, in the Kotlin/Swift family. Source files use `.phs`, and the manifest is `Phantasos.toml`. |
| Targets | Windows x64, Linux x86_64, Linux i386. |
| Toolchain | Built from scratch: our own x86 backends, COFF/ELF writers, PE/ELF linker, and C11 frontend. No LLVM, glibc, musl, or MSVC CRT. |
| Bootstrap | Stage0 in C++23, built with clang++ on Windows and Linux, compiles Phantasos-Core and includes a workspace-aware build driver. Stage1 is the compiler written in Phantasos. The stage2 and stage3 binaries must be byte-identical. |
| Privileged internals | `phantasos.internal` may only be imported by toolchain-shipped, hash-verified packages (`std`, `rt`, `libc-compat`). |
| License | Apache-2.0. Every source file carries the license header, and CI enforces it. |
