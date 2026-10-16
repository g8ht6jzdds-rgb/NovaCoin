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
    run_directory=$(dirname "${result_file}")
    for evidence in header-phase-peer.log header-before.txt header-held-1.txt header-held-2.txt header-held-3.txt header-after.txt header-restart.txt header-controls.txt; do
        [[ -s "${run_directory}/${evidence}" ]]
    done
    for marker in source_connection_closed=true canonical_fixture=passed target_verack=true target_getheaders=genesis_locator helper_headers_queued=1 target_getdata=exact_block body_withheld=true release_observed=true helper_block_queued=1 body_flush_pump=passed target_confirmation_observed=true exit=0; do
        grep -Fxq "${marker}" "${run_directory}/header-phase-peer.log"
    done
    # Preserve protocol order, not merely an unordered set of success markers.
    previous=0
    for marker in target_getheaders=genesis_locator helper_headers_queued=1 target_getdata=exact_block body_withheld=true release_observed=true helper_block_queued=1 body_flush_pump=passed target_confirmation_observed=true; do
        line=$(grep -Fnx "${marker}" "${run_directory}/header-phase-peer.log" | cut -d: -f1)
        [[ "${line}" =~ ^[0-9]+$ && "${line}" -gt "${previous}" ]]
        previous=${line}
    done
    for observation in 1 2 3; do
        cmp "${run_directory}/header-before.txt" "${run_directory}/header-held-${observation}.txt"
    done
    for marker in held_state=unchanged released_body=validated restart_persistence=passed; do
        grep -Fxq "${marker}" "${run_directory}/header-controls.txt"
    done
    for evidence in invalid-block-peer.log invalid-block-before.txt invalid-block-after.txt invalid-block-counters.txt; do
        if [[ ! -s "${run_directory}/${evidence}" ]]; then
            echo "missing invalid-block evidence: ${evidence}" >&2
            exit 1
        fi
    done
    grep -Fxq 'structure_roundtrip=passed' "${run_directory}/invalid-block-peer.log"
    grep -Fxq 'pow=invalid' "${run_directory}/invalid-block-peer.log"
    grep -Fxq 'exit=0' "${run_directory}/invalid-block-peer.log"
    cmp "${run_directory}/invalid-block-before.txt" "${run_directory}/invalid-block-after.txt"
    for evidence in invalid-transaction.hex valid-transaction.hex invalid-transaction-prepare.log invalid-transaction-peer.log invalid-transaction-before.txt invalid-transaction-after.txt invalid-transaction-counters.txt invalid-transaction-controls.txt valid-transaction-peer.log; do
        [[ -s "${run_directory}/${evidence}" ]]
    done
    for marker in canonical_roundtrip=passed signature_authorization=passed preflight=kInputValueTooLow peer_closed_after_submission=true exit=0; do
        grep -Fxq "${marker}" "${run_directory}/invalid-transaction-peer.log"
    done
    cmp "${run_directory}/invalid-transaction-before.txt" "${run_directory}/invalid-transaction-after.txt"
    for marker in valid_propagation=passed valid_confirmation=passed restart_persistence=passed; do
        grep -Fxq "${marker}" "${run_directory}/invalid-transaction-controls.txt"
    done
    grep -Fxq 'preflight=kNone' "${run_directory}/valid-transaction-peer.log"
    grep -Fxq 'valid_control_flushed=true' "${run_directory}/valid-transaction-peer.log"
    rejected_before=$(sed -n 's/^rejected_before=//p' "${run_directory}/invalid-transaction-counters.txt")
    rejected_after=$(sed -n 's/^rejected_after=//p' "${run_directory}/invalid-transaction-counters.txt")
    accepted_before=$(sed -n 's/^accepted_before=//p' "${run_directory}/invalid-transaction-counters.txt")
    accepted_after=$(sed -n 's/^accepted_after=//p' "${run_directory}/invalid-transaction-counters.txt")
    [[ "${rejected_before}" =~ ^[0-9]+$ && "${rejected_after}" =~ ^[0-9]+$ ]]
    [[ "${accepted_before}" =~ ^[0-9]+$ && "${accepted_after}" == "${accepted_before}" ]]
    [[ "${rejected_after}" -eq $((rejected_before + 1)) ]]
done

echo "verified retained Unix multi-process regtest evidence"
