#!/usr/bin/env python3
# Copyright 2026 The Phantasos Authors
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Mutation fuzzer for the stage0 frontend (syntax, semantics and name resolution).

Mutates the golden test inputs at the byte level (including invalid UTF-8)
and checks that `phantasos-stage0 parse` (or `check`) never crashes, hangs, trips a
sanitizer, or writes output that is not valid UTF-8. Failing inputs are saved
to --out.

Usage: python tests/fuzz_frontend.py --stage0 <exe> [--cases N] [--seed S] [--out DIR]
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "golden"

FRAGMENTS = [
    s.encode("utf-8")
    for s in [
        "(", ")", "{", "}", "[", "]", "<", ">", ">>", ">>=", '"', "\\(", "'", ".", "?.", "\n", ",", ";",
        "match", "if", "else", "fun", "class", "enum", "extern", "->", "=", "@", "|", "is", "as?", "try?",
        "!", "/*", "*/", "0x", "1e", "1e9999", "_", "let", ":", "&", "..<", "\u00e9", "\u2028",
    ]
] + [
    # Invalid UTF-8: stray continuation, overlong, surrogate, truncated, too large, bad lead byte.
    b"\x80", b"\xc0\xaf", b"\xed\xa0\x80", b"\xe2\x82", b"\xf4\x90\x80\x80", b"\xff", b"\xfe",
]

SANITIZER_MARKERS = (b"AddressSanitizer", b"runtime error:", b"LeakSanitizer")


def mutate(rng: random.Random, seeds: list[bytes]) -> bytes:
    # Byte-level mutation, so multi-byte characters can also be split.
    s = bytearray(rng.choice(seeds))
    for _ in range(rng.randint(1, 12)):
        pos = rng.randrange(len(s) + 1)
        op = rng.random()
        if op < 0.4:
            s[pos:pos] = rng.choice(FRAGMENTS)
        elif op < 0.8 and s:
            del s[pos : pos + rng.randint(1, 8)]
        else:
            donor = rng.choice(seeds)
            start = rng.randrange(len(donor))
            s[pos:pos] = donor[start : start + rng.randint(1, 40)]
    return bytes(s)


def is_utf8(data: bytes) -> bool:
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage0", required=True, type=Path)
    ap.add_argument("--cases", type=int, default=500)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--command", choices=("parse", "check", "gate", "mir", "asm", "build"), default="parse")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--out", type=Path, default=None, help="directory for failing inputs")
    args = ap.parse_args(argv)

    exe = args.stage0.resolve()
    if not exe.exists() and exe.with_suffix(".exe").exists():
        exe = exe.with_suffix(".exe")
    seeds = [p.read_bytes() for p in sorted(ROOT.rglob("*.phs"))]
    if not seeds:
        print("error: no seed inputs found", file=sys.stderr)
        return 2

    out_dir = args.out or Path(tempfile.mkdtemp(prefix="phs-fuzz-"))
    out_dir.mkdir(parents=True, exist_ok=True)
    work = out_dir / "case.phs"
    # `build` writes an executable into its own directory; it is never run,
    # since mutated loops need not end.
    build_dir = tempfile.TemporaryDirectory(prefix="phs-fuzz-build-") if args.command == "build" else None
    output = ["-o", str(Path(build_dir.name) / "case")] if build_dir else []
    rng = random.Random(args.seed)
    failures = 0
    for i in range(args.cases):
        text = mutate(rng, seeds)
        work.write_bytes(text)
        reason = None
        try:
            r = subprocess.run([str(exe), args.command, str(work), *output], capture_output=True, timeout=args.timeout)
            if r.returncode not in (0, 1):
                reason = f"exit code {r.returncode}"
            elif any(m in r.stderr for m in SANITIZER_MARKERS):
                reason = "sanitizer report"
            elif not is_utf8(r.stdout) or not is_utf8(r.stderr):
                reason = "output is not valid UTF-8"
        except subprocess.TimeoutExpired:
            reason = "timeout"
        if reason:
            failures += 1
            saved = out_dir / f"failure-{i}.phs"
            saved.write_bytes(text)
            print(f"FAIL case {i}: {reason} -> {saved}")
    work.unlink(missing_ok=True)
    if build_dir:
        build_dir.cleanup()
    print(f"fuzz ({args.command}): {args.cases} cases, {failures} failure(s), seed {args.seed}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
