#!/usr/bin/env bash
# Disposable, local REGTEST staging exercise. It never selects TESTNET or
# MAINNET and leaves a redacted factual evidence record for operator review.
set -euo pipefail

daemon=${1:?usage: regtest_staging_recovery_exercise.sh /path/to/novacoind [artifact-directory]}
if [[ ! -x "${daemon}" ]]; then
    echo "novacoind is not executable: ${daemon}" >&2
    exit 2
fi
for command in curl python3 sha256sum; do
    command -v "${command}" >/dev/null || {
        echo "required command is unavailable: ${command}" >&2
        exit 2
    }
done

artifact_root=${2:-}
if [[ -z "${artifact_root}" ]]; then
    artifact_root=$(mktemp -d "${TMPDIR:-/tmp}/novacoin-regtest-staging.XXXXXX")
else
    mkdir -p -- "${artifact_root}"
    artifact_root=$(mktemp -d "${artifact_root%/}/run.XXXXXX")
fi

readonly node_root="${artifact_root}/node"
readonly data_dir="${node_root}/data"
readonly log_dir="${node_root}/logs"
readonly daemon_log="${log_dir}/novacoind.log"
readonly evidence="${artifact_root}/exercise-record.txt"
readonly rpc_port=29743
readonly p2p_port=29744
readonly wallet_passphrase="disposable-regtest-staging-wallet-passphrase"
readonly rpc_password="disposable-regtest-staging-rpc-password"
pid=""

mkdir -p -- "${data_dir}" "${log_dir}"

record() {
    printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "${evidence}"
}

stop_node() {
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill -TERM "${pid}" 2>/dev/null || true
        wait "${pid}" || true
    fi
    pid=""
}
trap stop_node EXIT

start_node() {
    NOVACOIN_WALLET_PASSPHRASE="${wallet_passphrase}" \
        NOVACOIN_RPC_PASSWORD="${rpc_password}" \
        "${daemon}" --regtest --name staging-recovery --datadir "${data_dir}" \
        --p2pport "${p2p_port}" --rpcport "${rpc_port}" --logfile "${daemon_log}" \
        >>"${node_root}/stdout.log" 2>&1 &
    pid=$!
    for _ in $(seq 1 200); do
        if curl --silent --show-error --max-time 1 --fail-with-body \
            --user "novacoin:${rpc_password}" --header 'Content-Type: application/json' \
            --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo"}' \
            "http://127.0.0.1:${rpc_port}/rpc" >/dev/null 2>&1; then
            record "daemon_started pid=${pid}"
            return 0
        fi
        sleep 0.05
    done
    record "FAIL daemon did not become ready"
    return 1
}

rpc() {
    curl --silent --show-error --max-time 5 --fail-with-body \
        --user "novacoin:${rpc_password}" --header 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\"}" \
        "http://127.0.0.1:${rpc_port}/rpc"
}

chain_tuple() {
    { rpc getblockchaininfo; printf "\\n"; rpc getnodehealth; printf "\\n"; } | python3 -c '
import json, sys
blockchain, health = (json.loads(line)["result"] for line in sys.stdin)
print("height={};tip={};chainwork={}".format(
    blockchain["blocks"], blockchain["bestblockhash"], health["chainwork"]))
'
}

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_revision=$(git -C "${repo_root}" rev-parse HEAD 2>/dev/null || echo unavailable)
if git -C "${repo_root}" diff --quiet && git -C "${repo_root}" diff --cached --quiet; then
    tree_state=clean
else
    tree_state=dirty
fi
record "exercise=disposable-local-regtest source_revision=${source_revision} source_tree=${tree_state}"
record "binary_sha256=$(sha256sum "${daemon}" | awk '{print $1}') binary_path=${daemon}"
record "command=${BASH_SOURCE[0]} ${daemon} ${artifact_root}"

start_node
initial=$(chain_tuple)
record "initial_${initial}"
for _ in 1 2 3; do
    rpc generateregtestblock >/dev/null
done
expected=$(chain_tuple)
record "expected_${expected}"
journal_digest_before=$(sha256sum "${data_dir}/blocks.dat" | awk '{print $1}')
wallet_digest_before=$(sha256sum "${data_dir}/wallet.dat" | awk '{print $1}')
record "journal_sha256_before=${journal_digest_before} wallet_sha256_before=${wallet_digest_before}"

record "action=SIGTERM pid=${pid}"
kill -TERM "${pid}"
if wait "${pid}"; then
    record "clean_shutdown_exit=0"
else
    record "FAIL clean_shutdown_exit=nonzero"
    exit 1
fi
pid=""
grep -F "novacoind shutdown complete staging-recovery" "${node_root}/stdout.log" >/dev/null
test -s "${data_dir}/wallet.dat"
record "clean_shutdown_log=present wallet_persistence=file-present"

start_node
recovered=$(chain_tuple)
record "recovered_${recovered}"
if [[ "${recovered}" != "${expected}" ]]; then
    record "FAIL recovered chain state differs from expected"
    exit 1
fi
record "restart_recovery=PASS"
stop_node

# A single trailing prepare-record tag is a deliberately incomplete final
# journal record. BlockJournal::Load must retain the durably committed prefix.
printf '\x01' >>"${data_dir}/blocks.dat"
record "incomplete_journal_injected=single-prepare-tag journal_sha256_after_injection=$(sha256sum "${data_dir}/blocks.dat" | awk '{print $1}')"
start_node
prefix_recovered=$(chain_tuple)
record "prefix_recovered_${prefix_recovered}"
if [[ "${prefix_recovered}" != "${expected}" ]]; then
    record "FAIL incomplete-record recovery did not retain committed prefix"
    exit 1
fi
record "incomplete_journal_recovery=PASS"
stop_node

record "RESULT=PASS unsigned_local_exercise_only"
printf 'Evidence written to %s\n' "${evidence}"
