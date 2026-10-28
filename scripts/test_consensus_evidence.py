#!/usr/bin/env python3
"""Check clarification evidence integrity; never infer consensus completeness."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DOCUMENT = ROOT / "docs" / "consensus-evidence-bindings.json"
IDS = {"CFD01", "CFD02", "CFD03", "CFD04", "CFD05", "CFD06",
       "CFD10", "CFD15", "CFP01"}
ROLES = {"positive", "negative", "boundary"}
CLASSIFICATIONS = {"MAPPED_AND_TESTED", "MAPPED_BUT_TRIAD_INCOMPLETE",
                   "AMBIGUOUS", "UNMATCHED", "IMPLEMENTATION_DEPENDENT",
                   "HUMAN_PROCESS"}


def exact_fields(value, fields):
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ValueError("missing or unknown evidence fields")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON field")
        result[key] = value
    return result


def canonical_bytes(path):
    # Git's text checkout may use CRLF. This hash binds LF text, not raw disk
    # bytes, binaries, arbitrary encodings or independent reproduction.
    with path.open("rb") as stream:
        raw = stream.read(16 * 1024 * 1024 + 1)
    if len(raw) > 16 * 1024 * 1024:
        raise ValueError("evidence input exceeds checker bound")
    return raw.replace(b"\r\n", b"\n")


def checked_path(root, relative):
    if not isinstance(relative, str) or "\\" in relative:
        raise ValueError("expected repository-relative POSIX path")
    path = PurePosixPath(relative)
    if path.is_absolute() or not path.parts or any(
            part in {".", ".."} or ":" in part for part in path.parts):
        raise ValueError("unsafe evidence path")
    target = (root / relative).resolve()
    try:
        target.relative_to(root.resolve())
    except ValueError as error:
        raise ValueError("escaping evidence path") from error
    if not target.is_file():
        raise ValueError("missing or escaping evidence path")
    return target


def verify_binding(root, value):
    exact_fields(value, {"path", "line", "token", "sha256"})
    raw = canonical_bytes(checked_path(root, value["path"]))
    if hashlib.sha256(raw).hexdigest() != value["sha256"]:
        raise ValueError("bound source text changed; review before rebinding")
    lines = raw.decode("utf-8").splitlines()
    line = value["line"]
    if type(line) is not int or not 1 <= line <= len(lines):
        raise ValueError("invalid one-based evidence line")
    if not isinstance(value["token"], str) or not value["token"]:
        raise ValueError("empty evidence token")
    if value["token"] not in lines[line - 1]:
        raise ValueError("evidence token does not match bound line")


def verify(root, data):
    exact_fields(data, {"schema", "baseline", "records"})
    if type(data["schema"]) is not int or data["schema"] != 1:
        raise ValueError("unknown evidence schema")
    if data["baseline"] != "1b83e00f0e49a8703d0a6fef1291ccc538ca732e":
        raise ValueError("unexpected historical baseline")
    ledger = (root / "docs/consensus-clause-ledger.md").read_text()
    ledger_rows = re.findall(r"^\| (NCV0-\d{3}) \| [^\n]+", ledger, re.MULTILINE)
    if (len(ledger_rows) != 40 or
            set(ledger_rows) != {f"NCV0-{i:03}" for i in range(1, 41)}):
        raise ValueError("legacy clause inventory changed")
    for line in ledger.splitlines():
        if line.startswith("| NCV0-") and line.split("|")[3].strip() not in CLASSIFICATIONS:
            raise ValueError("unknown ledger classification")
    records = data["records"]
    if not isinstance(records, list) or len(records) != len(IDS):
        raise ValueError("unexpected clause inventory")
    seen = set()
    for row in records:
        exact_fields(row, {"id", "section", "protocol", "classification",
                           "sources", "cases", "gaps", "vectors"})
        if row["id"] not in IDS or row["id"] in seen:
            raise ValueError("duplicate or unknown clause ID")
        seen.add(row["id"])
        if row["classification"] != "MAPPED_BUT_TRIAD_INCOMPLETE":
            raise ValueError("this scoped checker cannot certify completeness")
        if not row["gaps"] or any(not isinstance(g, str) or not g.strip()
                                  for g in row["gaps"]):
            raise ValueError("missing explicit remaining gap")
        verify_binding(root, row["protocol"])
        if row["protocol"]["path"] != "docs/protocol.md":
            raise ValueError("protocol binding must name normative document")
        protocol = canonical_bytes(root / "docs/protocol.md").decode().splitlines()
        headings = [line for line in protocol[:row["protocol"]["line"]]
                    if line.startswith("## ")]
        if not headings or not headings[-1].startswith("## " + row["section"] + ". "):
            raise ValueError("protocol section mismatch")
        if not row["sources"]:
            raise ValueError("missing implementation binding")
        for source in row["sources"]:
            if not source["path"].startswith("src/"):
                raise ValueError("implementation binding must name src")
            verify_binding(root, source)
        exact_fields(row["cases"], ROLES)
        for case in row["cases"].values():
            exact_fields(case, {"description", "assertion", "binding"})
            if not case["description"].strip():
                raise ValueError("missing case description")
            if re.fullmatch(r"\w+\.\w+", case["assertion"]) is None:
                raise ValueError("expected exact named assertion")
            suite, name = case["assertion"].split(".")
            if (not case["binding"]["path"].startswith("tests/") or
                    case["binding"]["token"] != f"TEST({suite}, {name})"):
                raise ValueError("assertion declaration mismatch")
            verify_binding(root, case["binding"])
        for vector in row["vectors"]:
            exact_fields(vector, {"path", "sha256"})
            if (not vector["path"].startswith("test/data/protocol/") or
                    not vector["path"].endswith(".ncpv")):
                raise ValueError("expected existing protocol vector")
            raw = canonical_bytes(checked_path(root, vector["path"]))
            if hashlib.sha256(raw).hexdigest() != vector["sha256"]:
                raise ValueError("vector provenance changed")


class ConsensusEvidenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = json.loads(canonical_bytes(DOCUMENT).decode("utf-8"),
                              object_pairs_hook=unique_object)

    def test_current_bindings(self):
        verify(ROOT, self.data)

    def test_unknown_and_duplicate_fields(self):
        with self.assertRaises(ValueError):
            json.loads('{"schema":1,"schema":1}', object_pairs_hook=unique_object)
        bad = copy.deepcopy(self.data)
        bad["unexpected"] = True
        with self.assertRaises(ValueError):
            verify(ROOT, bad)

    def test_inventory_duplicate_and_missing(self):
        for change in ("duplicate", "missing"):
            bad = copy.deepcopy(self.data)
            if change == "duplicate":
                bad["records"][-1] = bad["records"][0]
            else:
                bad["records"].pop()
            with self.assertRaises(ValueError):
                verify(ROOT, bad)

    def test_missing_role_and_unsupported_completion(self):
        for change in ("role", "complete", "gap"):
            bad = copy.deepcopy(self.data)
            row = bad["records"][0]
            if change == "role":
                row["cases"].pop("boundary")
            elif change == "complete":
                row["classification"] = "MAPPED_AND_TESTED"
            else:
                row["gaps"] = []
            with self.assertRaises(ValueError):
                verify(ROOT, bad)

    def test_line_boundaries_and_drift(self):
        for line in (0, -1, True, 10**9):
            bad = copy.deepcopy(self.data)
            bad["records"][0]["protocol"]["line"] = line
            with self.assertRaises(ValueError):
                verify(ROOT, bad)
        bad = copy.deepcopy(self.data)
        bad["records"][0]["protocol"]["sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            verify(ROOT, bad)

    def test_valid_first_and_last_nonempty_line(self):
        binding = copy.deepcopy(self.data["records"][0]["protocol"])
        lines = canonical_bytes(ROOT / binding["path"]).decode().splitlines()
        last = max(i for i, line in enumerate(lines, 1) if line)
        for number in (1, last):
            binding["line"] = number
            binding["token"] = lines[number - 1]
            verify_binding(ROOT, binding)

    def test_escaping_and_missing_paths(self):
        for path in ("../AGENTS.md", "/etc/passwd", "C:/secret", "src/missing.cpp"):
            with self.assertRaises(ValueError):
                checked_path(ROOT, path)

    def test_section_and_assertion_drift(self):
        for change in ("section", "assertion"):
            bad = copy.deepcopy(self.data)
            if change == "section":
                bad["records"][0]["section"] = "30"
            else:
                bad["records"][0]["cases"]["positive"]["assertion"] = "Missing.Test"
            with self.assertRaises(ValueError):
                verify(ROOT, bad)

    def test_vector_provenance_drift(self):
        bad = copy.deepcopy(self.data)
        bad["records"][0]["vectors"][0]["sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            verify(ROOT, bad)


if __name__ == "__main__":
    unittest.main()
