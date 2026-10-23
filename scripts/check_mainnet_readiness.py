"""Fail-closed readiness inventory; never changes source or grants activation."""
import argparse
import json
from pathlib import Path
import re
import sys

STATES = {"PASS", "FAIL", "NOT TESTED", "HUMAN REVIEW REQUIRED"}


def parse_checklist(text):
    gates = {}
    for line in text.splitlines():
        if not re.match(r"\|\s*\d+\s*\|", line):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) != 3:
            raise ValueError("malformed gate row")
        gate, description, status = cells
        number = int(gate)
        if number in gates or status not in STATES or not description:
            raise ValueError("duplicate gate, empty requirement or invalid status")
        gates[number] = {"gate": number, "requirement": description, "status": status}
    if set(gates) != set(range(1, 15)):
        raise ValueError("exactly gates 1 through 14 required")
    return [gates[number] for number in range(1, 15)]


def check_references(root):
    names = set()
    for path in sorted((root / "tests").glob("*.cpp")):
        for suite, test in re.findall(r"TEST(?:_F)?\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)",
                                     path.read_text(encoding="utf-8")):
            names.add(suite + "." + test)
    rows = 0
    for line in (root / "docs/consensus-coverage.md").read_text(encoding="utf-8").splitlines():
        if not re.match(r"\|\s*C\d+\s", line):
            continue
        rows += 1
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) != 5 or any(not cell for cell in cells):
            raise ValueError("malformed coverage row")
        implementation = cells[1]
        if implementation.startswith("NONE:"):
            if any(not cell.startswith("NONE") for cell in cells[2:]):
                raise ValueError("unimplemented rule claims test coverage")
        else:
            for location in implementation.split(";"):
                location = location.strip()
                if (not re.fullmatch(r"[\w/-]+\.(?:cpp|hpp)", location)
                        or Path(location).anchor
                        or ".." in Path(location).parts
                        or not (root / "src" / location).is_file()):
                    raise ValueError("missing/invalid implementation reference: " + location)
        for reference in re.findall(r"`([A-Za-z]\w*\.\w+)`", line):
            if reference not in names:
                raise ValueError("missing coverage reference: " + reference)
    if rows == 0:
        raise ValueError("coverage inventory empty")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--activation-check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    gates = parse_checklist((root / "MAINNET_CHECKLIST.md").read_text(encoding="utf-8"))
    header = (root / "src/node/network_selection.hpp").read_text(encoding="utf-8")
    if not re.search(r"inline\s+constexpr\s+bool\s+MAINNET_LOCKED\s*=\s*true\s*;", header):
        raise ValueError("required compiled MAINNET lock missing")
    print(json.dumps({"MAINNET_LOCKED": True, "activationAllowed": False,
                      "coverageRows": check_references(root), "gates": gates,
                      "warning": "reference inventory is not human gate acceptance"}, indent=2))
    if args.activation_check:
        print("MAINNET_LOCKED: activation forbidden; inventory never authorizes launch.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(2)
