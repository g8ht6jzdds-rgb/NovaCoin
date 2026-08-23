#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: verify_regtest_ci_evidence.sh <build-directory>" >&2
    exit 2
fi

build_directory=$1
ctest_log="${build_directory}/ci-ctest.log"
artifact_directory="${build_directory}/regtest-artifacts"

if [[ ! -f "${ctest_log}" ]] ||
    ! grep -Eq 'nova\.regtest_rpc_process.*Passed' "${ctest_log}"; then
    echo "missing passed Unix multi-process regtest CTest evidence" >&2
    exit 1
fi

mapfile -t result_files < <(find "${artifact_directory}" -type f -name result.txt -print 2>/dev/null)
if [[ ${#result_files[@]} -eq 0 ]]; then
    echo "missing retained multi-process regtest result marker" >&2
    exit 1
fi

for result_file in "${result_files[@]}"; do
    if ! grep -Fxq 'result=passed' "${result_file}"; then
        echo "regtest harness did not complete successfully: ${result_file}" >&2
        exit 1
    fi
done

echo "verified retained Unix multi-process regtest evidence"
