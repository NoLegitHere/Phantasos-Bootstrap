# 4. Packages, modules, and workspaces

## 4.1 Packages [Core]
A **package** is a directory that contains a `Phantasos.toml` manifest:

```toml
[package]
name = "geometry"          # [a-z][a-z0-9_]*
version = "0.1.0"
kind = "lib"               # bin | lib | staticlib | dynlib
edition = "2026"

[dependencies]
mathx = { path = "../mathx" }
util = { workspace = true }      # a sibling member of the enclosing workspace
```

`phantasos init [--bin|--lib] [name]` creates the manifest, `src/main.phs` (for
`bin`) or `src/lib.phs` (for `lib`), and `tests/`. Every generated source file
includes the license header configured in `[package] license-header`, which
defaults to the Apache-2.0 header.

## 4.2 Modules and auto-discovery [Core]
- **Every `.phs` file under `src/` belongs to the package.** Nothing needs to
  be listed by hand.
- A file's **module path** is its directory path relative to `src/`, with
  `/` replaced by `.`. All files in the same directory form one module and
  share its namespace.
  - `src/shapes/circle.phs` is module `geometry.shapes`.
  - `src/lib.phs` and other files directly in `src/` form the root module
    `geometry`.
- Being in the same module does **not** widen visibility (3.8). A `file`
  declaration is visible only in its own source file, and a `private` one
  only in its enclosing type, even to other files of the same module.
  `internal` declarations (the default) are visible to every module of the
  package.
- `import geometry.shapes` imports a module, and `import geometry.shapes as s`
  imports it under an alias.
- The entry point of a `bin` package is `fun main()` in the root module. It
  may be declared `async` and/or `throws`.
- `csrc/**/*.c` is compiled by the built-in C frontend and linked into the
  package (full language). `tests/**/*.phs` is discovered by
  `phantasos test`.

## 4.3 Workspaces [Core]
```toml
[workspace]
members = ["compiler/*", "tools/*"]   # optional; default: every subdirectory with a Phantasos.toml
exclude = ["examples/old"]
```
- A workspace root is a `Phantasos.toml` that contains a `[workspace]`
  table. It may also contain a `[package]` table.
- Members are discovered automatically: by default, every directory below
  the root that contains a `Phantasos.toml`, excluding `build/` and
  `.phantasos/`. Nested workspaces are an error.
- The build driver resolves dependencies into a DAG of packages. A cycle is
  an error, and the diagnostic shows the full cycle. Packages are built in
  topological order.
- `phantasos build` builds the whole workspace. `phantasos build -p name`, or
  running it from inside a member directory, builds that member and its
  dependencies.
- **Linking.**
  - A `lib` produces a `.phsi` interface file (exported declarations plus
    the bodies of inlinable and generic functions) and an object archive.
  - Packages that depend on the lib read only its `.phsi`, never its sources.
  - A `bin` links the objects of all its transitive `lib` dependencies.

## 4.4 Output layout
```
build/<target>/<profile>/<package>/        objects, .phsi
build/<target>/<profile>/bin/<name>[.exe]  executables
```
`<target>` is one of `windows-x64`, `linux-x86_64`, `linux-i386`.
`<profile>` is `debug` or `release`.

## 4.5 Stage0 support
**Current status:** stage0 provides `lex`, syntax-only `parse`, and `check`,
`gate` and `build` on single files. Imports receive `E0304` from `check`
until package loading is implemented. None of the package or workspace
features in this chapter work yet.

**Planned scope** (before the compiler is ported to Phantasos, see
[ADR 0003](../adr/0003-bootstrap-subset.md)): a directory of `.phs` files
forms one package, with module paths and `import` as in 4.2 and the
visibility rules of 3.8. Stage0 reads no manifest and builds no libraries;
the whole program is compiled at once.

Manifests, workspaces, the package DAG, `.phsi` interface files, libraries,
the incremental cache, the lockfile, git dependencies, parallel builds and
cross-compilation belong to the self-hosted build tool.
