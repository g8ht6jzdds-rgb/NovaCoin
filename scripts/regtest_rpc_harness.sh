#!/usr/bin/env bash
set -euo pipefail

# Runs four real novacoind processes and drives only their authenticated
# localhost RPC interfaces plus a bounded loopback test peer. It is regtest-only and makes no
# consensus decision itself: assertions query state already selected by nodes.

daemon=${1:?usage: regtest_rpc_harness.sh /path/to/novacoind /path/to/nova-explorer}
explorer=${2:?usage: regtest_rpc_harness.sh /path/to/novacoind /path/to/nova-explorer}
invalid_block_peer=${3:?usage: regtest_rpc_harness.sh daemon explorer invalid-block-peer}
invalid_transaction_peer=${4:?usage: regtest_rpc_harness.sh daemon explorer invalid-block-peer invalid-transaction-peer}
[[ -x "${invalid_transaction_peer}" ]]
if [[ ! -x "${daemon}" ]]; then
    echo "novacoind is not executable: ${daemon}" >&2
    exit 2
fi
if [[ ! -x "${explorer}" ]]; then
    echo "nova-explorer is not executable: ${explorer}" >&2
    exit 2
fi
if [[ ! -x "${invalid_block_peer}" ]]; then
    echo "invalid-block peer is not executable: ${invalid_block_peer}" >&2
    exit 2
fi

# CI can retain the complete process-level evidence (per-node daemon logs,
# explorer log, and state directories) without exposing its RPC or wallet
# secrets. Local runs retain the old automatic cleanup behavior.
artifact_root=${NOVACOIN_REGTEST_ARTIFACT_DIR:-}
if [[ -n "${artifact_root}" ]]; then
    mkdir -p -- "${artifact_root}"
    work_root=$(mktemp -d "${artifact_root}/run.XXXXXX")
else
    work_root=$(mktemp -d "${TMPDIR:-/tmp}/novacoin-rpc-regtest.XXXXXX")
fi
wallet_passphrase="regtest-harness-wallet-passphrase"
rpc_password="regtest-harness-rpc-password"
explorer_password="regtest-harness-explorer-password"
declare -a pids=()
last_started_pid=""
harness_result="failed"

cleanup() {
    local pid
    for pid in "${pids[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
    if [[ -n "${artifact_root}" ]]; then
        printf 'result=%s\n' "${harness_result}" >"${work_root}/result.txt"
    else
        rm -rf -- "${work_root}"
    fi
}
trap cleanup EXIT

rpc_raw() {
    local port=$1
    local method=$2
    local params=${3:-}
    local body
    if [[ -n "${params}" ]]; then
        body="{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}"
    else
        body="{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\"}"
    fi
    curl --silent --show-error --max-time 10 --fail-with-body \
        --user "novacoin:${rpc_password}" --header 'Content-Type: application/json' \
        --data "${body}" "http://127.0.0.1:${port}/rpc"
}

rpc() {
    local response
    response=$(rpc_raw "$@")
    if [[ "${response}" == *'"error"'* ]]; then
        echo "RPC $2 failed: ${response}" >&2
        return 1
    fi
    printf '%s' "${response}"
}

height() {
    local response
    response=$(rpc_raw "$1" getblockchaininfo 2>/dev/null || true)
    sed -n 's/.*"blocks":\([0-9][0-9]*\).*/\1/p' <<<"${response}"
}

mempool_size() {
    local response
    response=$(rpc_raw "$1" getmempoolinfo 2>/dev/null || true)
    sed -n 's/.*"size":\([0-9][0-9]*\).*/\1/p' <<<"${response}"
}

wait_height() {
    local port=$1
    local expected=$2
    local actual
    for _ in $(seq 1 240); do
        actual=$(height "${port}")
        if [[ "${actual}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.05
    done
    echo "node on RPC ${port} did not reach height ${expected} (last ${actual:-unavailable})" >&2
    return 1
}

wait_mempool() {
    local port=$1
    local expected=$2
    local actual
    for _ in $(seq 1 240); do
        actual=$(mempool_size "${port}")
        if [[ "${actual}" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.05
    done
    echo "node on RPC ${port} did not reach mempool size ${expected} (last ${actual:-unavailable})" >&2
    return 1
}

start_node() {
    local name=$1
    local p2p_port=$2
    local rpc_port=$3
    local connect_port=${4:-}
    local node_root="${work_root}/${name}"
    mkdir -p "${node_root}/data" "${node_root}/logs"
    local -a connect_args=()
    if [[ -n "${connect_port}" ]]; then
        connect_args=(--connect "127.0.0.1:${connect_port}")
    fi
    NOVACOIN_WALLET_PASSPHRASE="${wallet_passphrase}" \
        NOVACOIN_RPC_PASSWORD="${rpc_password}" \
        NOVACOIN_EXPLORER_RPC_PASSWORD="${rpc_password}" \
        "${daemon}" --regtest --name "${name}" --datadir "${node_root}/data" \
        --p2pport "${p2p_port}" --rpcport "${rpc_port}" \
        --logfile "${node_root}/logs/novacoind.log" "${connect_args[@]}" \
        >"${node_root}/logs/stdout.log" 2>&1 &
    pids+=("$!")
    last_started_pid=$!
}

start_explorer() {
    local rpc_port=$1
    local http_port=$2
    local log_path="${work_root}/explorer.log"
    NOVACOIN_EXPLORER_RPC_PASSWORD="${rpc_password}" \
        NOVACOIN_EXPLORER_PASSWORD="${explorer_password}" \
        "${explorer}" --regtest --rpcport "${rpc_port}" --httpport "${http_port}" \
        >"${log_path}" 2>&1 &
    pids+=("$!")
}

explorer_summary() {
    local port=$1
    curl --silent --show-error --max-time 10 --fail-with-body \
        --user "explorer:${explorer_password}" \
        "http://127.0.0.1:${port}/api/v1/summary"
}

wait_explorer_height() {
    local port=$1
    local expected=$2
    local response
    for _ in $(seq 1 240); do
        response=$(explorer_summary "${port}" 2>/dev/null || true)
        if [[ "${response}" == *"\"height\":${expected}"* ]]; then
            return 0
        fi
        sleep 0.05
    done
    echo "explorer on HTTP ${port} did not index height ${expected}" >&2
    return 1
}

address() {
    local response
    response=$(rpc "$1" getnewaddress)
    sed -n 's/.*"result":"\([1-9A-HJ-NP-Za-km-z][1-9A-HJ-NP-Za-km-z]*\)".*/\1/p' <<<"${response}"
}

# Two initially separate TCP-connected pairs form deterministic partitions.
start_node alpha 29644 29643
wait_height 29643 0
start_node bravo 29645 29642 29644
wait_height 29642 0
start_node charlie 29646 29641
wait_height 29641 0
start_node delta 29647 29640 29646
wait_height 29640 0

# Mining and transfer propagation use the daemon callbacks into RegtestNode,
# not fabricated blocks or direct store edits.
rpc 29643 generateregtestblock >/dev/null
wait_height 29642 1
rpc 29643 generateregtestblock >/dev/null
wait_height 29642 2
bravo_address=$(address 29642)
[[ "${bravo_address}" =~ ^[1-9A-HJ-NP-Za-km-z]{26,64}$ ]]
rpc 29643 sendtoaddress "{\"address\":\"${bravo_address}\",\"amount\":100000000}" >/dev/null
wait_mempool 29643 1
wait_mempool 29642 1
rpc 29643 generateregtestblock >/dev/null
wait_height 29642 3
wait_mempool 29643 0
wait_mempool 29642 0

# A canonical block with invalid PoW must reach the ordinary validator. A
# disconnect alone would also pass for a framing error, so require the node's
# specific rejection counter to advance and all active-chain state to stay put.
stable_state() {
    local response
    response=$(rpc "$1" getnodehealth)
    sed -n 's/.*"height":\([0-9]*\),"bestblockhash":"\([0-9a-f]*\)","chainwork":"\([0-9a-f]*\)","mempool_bytes":\([0-9]*\),"peers":[0-9]*,"mempool":\([0-9]*\).*/\1 \2 \3 \4 \5/p' <<<"${response}"
}
rejected_blocks() {
    rpc "$1" getnodemetrics | sed -n 's/.*"blocks_rejected":\([0-9]*\).*/\1/p'
}
declare -A invalid_before=()
for port in 29643 29642 29641 29640; do
    invalid_before[${port}]=$(stable_state "${port}")
    [[ -n "${invalid_before[${port}]}" ]]
    printf '%s %s\n' "${port}" "${invalid_before[${port}]}" >>"${work_root}/invalid-block-before.txt"
done
rejected_before=$(rejected_blocks 29643)
[[ "${rejected_before}" =~ ^[0-9]+$ ]]
control_hash=$(rpc 29643 getblockchaininfo | sed -n 's/.*"bestblockhash":"\([0-9a-f]*\)".*/\1/p')
[[ "${control_hash}" =~ ^[0-9a-f]{64}$ ]]
set +e
"${invalid_block_peer}" 29644 "${control_hash}" >"${work_root}/invalid-block-peer.log" 2>&1
peer_exit=$?
set -e
printf 'exit=%s\n' "${peer_exit}" >>"${work_root}/invalid-block-peer.log"
[[ "${peer_exit}" == 0 ]]
rejected_after=$(rejected_blocks 29643)
[[ "${rejected_after}" =~ ^[0-9]+$ ]]
[[ "${rejected_after}" -eq $((rejected_before + 1)) ]]
for port in 29643 29642 29641 29640; do
    invalid_after=$(stable_state "${port}")
    printf '%s %s\n' "${port}" "${invalid_after}" >>"${work_root}/invalid-block-after.txt"
    [[ "${invalid_after}" == "${invalid_before[${port}]}" ]]
done
printf 'rejected_before=%s\nrejected_after=%s\n' "${rejected_before}" "${rejected_after}" >"${work_root}/invalid-block-counters.txt"
# Subsequent ordinary mining/propagation and SIGTERM restart below remain
# positive controls; successful completion is required before the result marker.

# Join all four nodes before a signed, canonical overspend is submitted. Bravo
# has exactly the confirmed payment output from block 3, not a coinbase input.
rpc 29641 connectpeer '{"host":"127.0.0.1","port":29644}' >/dev/null
wait_height 29641 3
wait_height 29640 3
alpha_address=$(address 29643)
transaction_hex() {
    sed -n 's/.*"hex":"\([0-9a-f]*\)".*/\1/p'
}
unsigned_hex=$(rpc 29642 createtransaction "{\"address\":\"${alpha_address}\",\"amount\":10000000}" | transaction_hex)
[[ -n "${unsigned_hex}" ]]
printf '%s\n' "${unsigned_hex}" >"${work_root}/valid-unsigned.hex"
"${invalid_transaction_peer}" prepare 29644 "${control_hash}" "${work_root}/valid-unsigned.hex" \
    >"${work_root}/overspend-unsigned.hex" 2>"${work_root}/invalid-transaction-prepare.log"
overspend_hex=$(cat "${work_root}/overspend-unsigned.hex")
signed_hex=$(rpc 29642 signtransaction "{\"hex\":\"${overspend_hex}\"}" | transaction_hex)
[[ -n "${signed_hex}" ]]
printf '%s\n' "${signed_hex}" >"${work_root}/invalid-transaction.hex"
input_txid=$(sed -n 's/^input_txid=//p' "${work_root}/invalid-transaction-prepare.log")
input_vout=$(sed -n 's/^input_vout=//p' "${work_root}/invalid-transaction-prepare.log")
input_value=$(sed -n 's/^input_value=//p' "${work_root}/invalid-transaction-prepare.log")
[[ "${input_txid}" =~ ^[0-9a-f]{64}$ && "${input_vout}" =~ ^[0-9]+$ && "${input_value}" == 100000000 ]]
utxo_params="{\"txid\":\"${input_txid}\",\"vout\":${input_vout}}"
transaction_counter() {
    rpc "$1" getnodemetrics | sed -n "s/.*\"transactions_$2\":\([0-9]*\).*/\1/p"
}
declare -A transaction_before=() utxo_before=() wallet_before=() peers_before=()
for port in 29643 29642 29641 29640; do
    wait_mempool "${port}" 0
    transaction_before[${port}]=$(stable_state "${port}")
    utxo_before[${port}]=$(rpc "${port}" getutxo "${utxo_params}")
    wallet_before[${port}]=$(rpc "${port}" listunspent)
    peers_before[${port}]=$(rpc "${port}" getpeerinfo)
    [[ -n "${transaction_before[${port}]}" ]]
    printf '%s %s %s %s\n' "${port}" "${transaction_before[${port}]}" "${utxo_before[${port}]}" "${wallet_before[${port}]}" >>"${work_root}/invalid-transaction-before.txt"
done
tx_rejected_before=$(transaction_counter 29643 rejected)
tx_accepted_before=$(transaction_counter 29643 accepted)
[[ "${tx_rejected_before}" =~ ^[0-9]+$ && "${tx_accepted_before}" =~ ^[0-9]+$ ]]
set +e
"${invalid_transaction_peer}" submit 29644 "${control_hash}" "${work_root}/invalid-transaction.hex" \
    >"${work_root}/invalid-transaction-peer.log" 2>&1
transaction_exit=$?
set -e
printf 'exit=%s\n' "${transaction_exit}" >>"${work_root}/invalid-transaction-peer.log"
[[ "${transaction_exit}" == 0 ]]
# Allow relay/side effects to surface, with no mining or concurrent submissions.
sleep 0.25
tx_rejected_after=$(transaction_counter 29643 rejected)
tx_accepted_after=$(transaction_counter 29643 accepted)
[[ "${tx_rejected_after}" -eq $((tx_rejected_before + 1)) ]]
[[ "${tx_accepted_after}" == "${tx_accepted_before}" ]]
for port in 29643 29642 29641 29640; do
    state=$(stable_state "${port}")
    utxo=$(rpc "${port}" getutxo "${utxo_params}")
    wallet=$(rpc "${port}" listunspent)
    printf '%s %s %s %s\n' "${port}" "${state}" "${utxo}" "${wallet}" >>"${work_root}/invalid-transaction-after.txt"
    [[ "${state}" == "${transaction_before[${port}]}" ]]
    [[ "${utxo}" == "${utxo_before[${port}]}" && "${wallet}" == "${wallet_before[${port}]}" ]]
    [[ "$(rpc "${port}" getpeerinfo)" == "${peers_before[${port}]}" ]]
done
printf 'rejected_before=%s\nrejected_after=%s\naccepted_before=%s\naccepted_after=%s\n' \
    "${tx_rejected_before}" "${tx_rejected_after}" "${tx_accepted_before}" "${tx_accepted_after}" >"${work_root}/invalid-transaction-counters.txt"
# Sign and submit the ORIGINAL transaction: exactly the same input remains
# spendable. Require propagation and confirmation on all nodes before restart.
valid_hex=$(rpc 29642 signtransaction "{\"hex\":\"${unsigned_hex}\"}" | transaction_hex)
[[ -n "${valid_hex}" ]]
printf '%s\n' "${valid_hex}" >"${work_root}/valid-transaction.hex"
"${invalid_transaction_peer}" control 29644 "${control_hash}" "${work_root}/valid-transaction.hex" \
    >"${work_root}/valid-transaction-peer.log" 2>&1
[[ "$(sed -n 's/^input_txid=//p' "${work_root}/valid-transaction-peer.log")" == "${input_txid}" ]]
[[ "$(sed -n 's/^input_vout=//p' "${work_root}/valid-transaction-peer.log")" == "${input_vout}" ]]
for port in 29643 29642 29641 29640; do
    wait_mempool "${port}" 1
done
rpc 29643 generateregtestblock >/dev/null
for port in 29643 29642 29641 29640; do
    wait_height "${port}" 4
    wait_mempool "${port}" 0
    spent=$(rpc_raw "${port}" getutxo "${utxo_params}")
    [[ "${spent}" == *'"error"'* ]]
done
control_wallet=$(rpc 29643 listunspent)
printf 'valid_propagation=passed\nvalid_confirmation=passed\n' >"${work_root}/invalid-transaction-controls.txt"

# The independent explorer process obtains copied snapshots through the
# authenticated node RPC endpoint, then exposes only its own authenticated
# read-only frontend. It has no node-state or consensus-state reference.
start_explorer 29643 29639
wait_explorer_height 29639 4

# Restart alpha from its journal and encrypted wallet; its active tip must be
# restored before it reconnects to bravo.
alpha_pid=${pids[0]}
kill "${alpha_pid}"
wait "${alpha_pid}" || true
start_node alpha 29644 29643 29645
wait_height 29643 4
[[ "$(rpc 29643 listunspent)" == "${control_wallet}" ]]
[[ "$(rpc_raw 29643 getutxo "${utxo_params}")" == *'"error"'* ]]
printf 'restart_persistence=passed\n' >>"${work_root}/invalid-transaction-controls.txt"

# The second partition already shares the parent. Sever all live sockets.
# Each partition now mines a competing branch from height 4.
wait_height 29641 4
wait_height 29640 4
for port in 29643 29642 29641 29640; do
    rpc "${port}" disconnectpeers >/dev/null
done
sleep 1
# Re-form only the winning partition's internal link after severing all prior
# sockets; alpha remains isolated on the competing branch.
rpc 29641 connectpeer '{"host":"127.0.0.1","port":29647}' >/dev/null
wait_height 29640 4

# The transaction is confirmed only on alpha's lower-work side branch.
bravo_address=$(address 29642)
rpc 29643 sendtoaddress "{\"address\":\"${bravo_address}\",\"amount\":100000000}" >/dev/null
wait_mempool 29643 1
rpc 29643 generateregtestblock >/dev/null
wait_height 29643 5
wait_mempool 29643 0
rpc 29641 generateregtestblock >/dev/null
rpc 29641 generateregtestblock >/dev/null
wait_height 29640 6

# Reconnect the branches. Greater verified chainwork on charlie's branch must
# win; alpha disconnects its payment block and restores that transaction.
rpc 29643 connectpeer '{"host":"127.0.0.1","port":29646}' >/dev/null
wait_height 29643 6
wait_mempool 29643 1
wait_explorer_height 29639 6

# Negative control: malformed raw transaction input remains rejected by RPC
# and cannot alter a process's mempool.
before=$(mempool_size 29643)
invalid=$(rpc_raw 29643 sendwallettransaction '{"hex":"00"}')
[[ "${invalid}" == *'"error"'* ]]
[[ "$(mempool_size 29643)" == "${before}" ]]

# SIGTERM must take the daemon through its ordinary shutdown path: listeners
# close and the encrypted wallet is atomically persisted before a zero exit.
# Restarting from the same data directory must recover the active chain.
alpha_pid=${last_started_pid}
kill -TERM "${alpha_pid}"
wait "${alpha_pid}"
start_node alpha 29644 29643
wait_height 29643 6

harness_result="passed"
echo "RPC-driven multi-process regtest harness passed"
