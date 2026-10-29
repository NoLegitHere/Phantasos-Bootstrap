# ADR 0003: The bootstrap subset

- Status: accepted
- Supersedes: the Bootstrap row of [ADR 0002](0002-foundational-decisions.md)

## Context
ADR 0002 has stage0 compile all of Phantasos-Core, with Windows and Linux
executables and a workspace-aware build driver, before the compiler is ported
to Phantasos. That means classes, values of interface type, built-in traits,
typed errors, a PE/COFF backend, a linker and manifests would be built twice:
once in C++ and again in Phantasos. Stage0's code generation gate (spec 3.25)
already reports every construct it cannot compile with `E0600`.

## Decision
Stage1 is written in the **bootstrap subset**: the programs that stage0's
`gate` accepts. Stage0 grows only until that subset is enough to write a
compiler:

- shifts (`<<`, `>>`) for hashing, bit sets and instruction encoding;
- enums with payloads and `match`;
- `String` values that are always valid UTF-8, with comparison, byte
  access, slicing on character boundaries and building from bytes;
- reading and writing files, command-line arguments, exit codes and stderr;
- programs made of several files: a directory of modules with `import`,
  without a manifest.

Trees and graphs in stage1 are stored in arrays and linked by index, so
stage0 needs no reference counting. Everything else in Core, Windows
executables, object files, the linker, manifests and workspaces is built
only in stage1. Stage0 keeps type checking what it checks today.

## Consequences
- Stage0 generates code only for Linux x86_64, so stage1 runs on Linux, or
  under WSL on Windows, until stage1 writes PE executables itself.
- Stage1's source is checked against the subset on every build by running
  `gate`. Using anything outside it is a build error, not a review comment.
- Generated code stays naive and memory is never freed, so stage1 built by
  stage0 is slow. Stage1 compiled by itself (stage2) is not.
- The byte-identical stage2 and stage3 rule from ADR 0002 is unchanged.
