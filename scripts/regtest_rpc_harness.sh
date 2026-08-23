#!/usr/bin/env bash
set -euo pipefail

# Runs four real novacoind processes and drives only their authenticated
# localhost RPC interfaces.  It is intentionally regtest-only and makes no
# consensus decision itself: assertions query state already selected by nodes.

daemon=${1:?usage: regtest_rpc_harness.sh /path/to/novacoind /path/to/nova-explorer}
explorer=${2:?usage: regtest_rpc_harness.sh /path/to/novacoind /path/to/nova-explorer}
if [[ ! -x "${daemon}" ]]; then
    echo "novacoind is not executable: ${daemon}" >&2
    exit 2
fi
if [[ ! -x "${explorer}" ]]; then
    echo "nova-explorer is not executable: ${explorer}" >&2
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
        "${daemon}" --regtest --name "${name}" --datadir "${node_root}/data" \
        --p2pport "${p2p_port}" --rpcport "${rpc_port}" \
        --logfile "${node_root}/logs/novacoind.log" "${connect_args[@]}" \
        >"${node_root}/logs/stdout.log" 2>&1 &
    pids+=("$!")
}

start_explorer() {
    local rpc_port=$1
    local http_port=$2
    local log_path="${work_root}/explorer.log"
    NOVACOIN_RPC_PASSWORD="${rpc_password}" \
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
    sed -n 's/.*"result":"\([0-9a-f][0-9a-f]*\)".*/\1/p' <<<"${response}"
}

# Two initially separate TCP-connected pairs form deterministic partitions.
start_node alpha 29644 29643
start_node bravo 29645 29642 29644
start_node charlie 29646 29641
start_node delta 29647 29640 29646
wait_height 29643 0
wait_height 29642 0
wait_height 29641 0
wait_height 29640 0

# Mining and transfer propagation use the daemon callbacks into RegtestNode,
# not fabricated blocks or direct store edits.
rpc 29643 generateregtestblock >/dev/null
wait_height 29642 1
rpc 29643 generateregtestblock >/dev/null
wait_height 29642 2
bravo_address=$(address 29642)
[[ "${bravo_address}" =~ ^[0-9a-f]{40}$ ]]
rpc 29643 sendtoaddress "{\"address\":\"${bravo_address}\",\"amount\":100000000}" >/dev/null
wait_mempool 29643 1
wait_mempool 29642 1
rpc 29643 generateregtestblock >/dev/null
wait_height 29642 3
wait_mempool 29643 0

# The independent explorer process obtains copied snapshots through the
# authenticated node RPC endpoint, then exposes only its own authenticated
# read-only frontend. It has no node-state or consensus-state reference.
start_explorer 29643 29639
wait_explorer_height 29639 3

# Restart alpha from its journal and encrypted wallet; its active tip must be
# restored before it reconnects to bravo.
alpha_pid=${pids[0]}
kill "${alpha_pid}"
wait "${alpha_pid}" || true
start_node alpha 29644 29643 29645
wait_height 29643 3

# Let the second partition synchronize to the shared parent, then sever all
# live sockets. Each partition now mines a competing branch from height 3.
rpc 29641 connectpeer '{"host":"127.0.0.1","port":29644}' >/dev/null
wait_height 29641 3
wait_height 29640 3
for port in 29643 29642 29641 29640; do
    rpc "${port}" disconnectpeers >/dev/null
done
sleep 1
# Re-form only the winning partition's internal link after severing all prior
# sockets; alpha remains isolated on the competing branch.
rpc 29641 connectpeer '{"host":"127.0.0.1","port":29647}' >/dev/null
wait_height 29640 3

# The transaction is confirmed only on alpha's lower-work side branch.
bravo_address=$(address 29642)
rpc 29643 sendtoaddress "{\"address\":\"${bravo_address}\",\"amount\":100000000}" >/dev/null
wait_mempool 29643 1
rpc 29643 generateregtestblock >/dev/null
wait_height 29643 4
wait_mempool 29643 0
rpc 29641 generateregtestblock >/dev/null
rpc 29641 generateregtestblock >/dev/null
wait_height 29640 5

# Reconnect the branches. Greater verified chainwork on charlie's branch must
# win; alpha disconnects its payment block and restores that transaction.
rpc 29643 connectpeer '{"host":"127.0.0.1","port":29646}' >/dev/null
wait_height 29643 5
wait_mempool 29643 1
wait_explorer_height 29639 5

# Negative control: malformed raw transaction input remains rejected by RPC
# and cannot alter a process's mempool.
before=$(mempool_size 29643)
invalid=$(rpc_raw 29643 sendwallettransaction '{"hex":"00"}')
[[ "${invalid}" == *'"error"'* ]]
[[ "$(mempool_size 29643)" == "${before}" ]]

harness_result="passed"
echo "RPC-driven multi-process regtest harness passed"
