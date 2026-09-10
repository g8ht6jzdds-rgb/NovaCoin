#!/usr/bin/env bash
# Deterministic parser/guard checks for the staging-evidence collector.
# This does not exercise Docker, systemd, or a real host.
set -euo pipefail

collector=${1:?usage: test_collect_regtest_staging_evidence.sh <collector-path>}
candidate=6af776bb65d72f60cbee36547e456e64431f6683
temporary_directory=$(mktemp -d)
trap 'rm -rf -- "$temporary_directory"' EXIT

expect_exit() {
    local expected=$1
    shift
    set +e
    "$@" >/dev/null 2>&1
    local actual=$?
    set -e
    if [[ "$actual" -ne "$expected" ]]; then
        echo "expected exit ${expected}, got ${actual}: $*" >&2
        exit 1
    fi
}

expect_exit 2 "$collector"

expect_exit 2 "$collector" --mode systemd --candidate "$candidate" --binary /usr/bin/true \
    --data-dir /tmp --log-dir /tmp --output "$temporary_directory/missing-probe.txt" --phase before

touch "$temporary_directory/external-probe.txt"
expect_exit 2 "$collector" --mode systemd --candidate "$candidate" --binary /usr/bin/true \
    --data-dir /tmp --log-dir /tmp --output "$temporary_directory/missing-baseline.txt" --phase after \
    --external-probe "$temporary_directory/external-probe.txt"

touch "$temporary_directory/existing.txt"
expect_exit 2 "$collector" --mode systemd --candidate "$candidate" --binary /usr/bin/true \
    --data-dir /tmp --log-dir /tmp --output "$temporary_directory/existing.txt" --phase before \
    --external-probe "$temporary_directory/external-probe.txt"

echo "staging evidence collector input guards passed"
