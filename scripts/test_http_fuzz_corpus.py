#!/usr/bin/env python3
"""Verify the bounded HTTP-request fuzz seed corpus and its SHA-256 manifest."""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path


MAX_SEED_BYTES = 8_448
MANIFEST_LINE = re.compile(r"([0-9a-f]{64})  ([A-Za-z0-9._-]+)\Z")


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        return fail("usage: test_http_fuzz_corpus.py CORPUS_DIR MANIFEST")

    corpus_dir = Path(sys.argv[1])
    manifest_path = Path(sys.argv[2])
    try:
        manifest_bytes = manifest_path.read_bytes()
        manifest_text = manifest_bytes.decode("ascii")
    except (OSError, UnicodeDecodeError) as error:
        return fail(f"cannot read ASCII manifest: {error}")
    if b"\r" in manifest_bytes or not manifest_text.endswith("\n"):
        return fail("manifest must use LF line endings and end with LF")

    expected: dict[str, str] = {}
    names: list[str] = []
    for line_number, line in enumerate(manifest_text.splitlines(), start=1):
        match = MANIFEST_LINE.fullmatch(line)
        if match is None:
            return fail(f"manifest line {line_number} has invalid syntax")
        digest, name = match.groups()
        if name in expected:
            return fail(f"duplicate manifest entry: {name}")
        expected[name] = digest
        names.append(name)
    if names != sorted(names):
        return fail("manifest entries are not in ordinal filename order")

    try:
        entries = list(corpus_dir.iterdir())
    except OSError as error:
        return fail(f"cannot read corpus directory: {error}")
    actual: dict[str, Path] = {}
    for entry in entries:
        if entry.is_symlink() or not entry.is_file():
            return fail(f"corpus entries must be regular files: {entry.name}")
        if entry.stat().st_size > MAX_SEED_BYTES:
            return fail(f"seed exceeds {MAX_SEED_BYTES} bytes: {entry.name}")
        actual[entry.name] = entry
    if set(actual) != set(expected):
        return fail(
            f"manifest membership mismatch: actual={sorted(actual)}, expected={sorted(expected)}"
        )

    for name in names:
        digest = hashlib.sha256(actual[name].read_bytes()).hexdigest()
        if digest != expected[name]:
            return fail(f"SHA-256 mismatch: {name}")
    print(f"verified {len(expected)} HTTP parser seed files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
