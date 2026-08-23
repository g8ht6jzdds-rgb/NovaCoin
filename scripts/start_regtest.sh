#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${NOVA_BUILD_DIR:-"${repo_root}/build/debug"}
daemon="${build_dir}/src/daemon/novacoind"
regtest_dir="${repo_root}/regtest"

if [[ -z "${NOVACOIN_WALLET_PASSPHRASE:-}" ]]; then
    echo "set NOVACOIN_WALLET_PASSPHRASE before launching persistent regtest wallets" >&2
    exit 1
fi
if [[ -z "${NOVACOIN_RPC_PASSWORD:-}" ]]; then
    echo "set NOVACOIN_RPC_PASSWORD before launching the authenticated RPC listener" >&2
    exit 1
fi

if [[ ! -x "${daemon}" ]]; then
    echo "novacoind not found: configure and build the debug preset first" >&2
    exit 1
fi

mkdir -p "${regtest_dir}"
nodes=(alpha bravo charlie delta)
p2p_ports=(18444 18445 18446 18447)
rpc_ports=(18443 18442 18441 18440)

for index in "${!nodes[@]}"; do
    name=${nodes[${index}]}
    node_dir="${regtest_dir}/${name}"
    pid_file="${node_dir}/novacoind.pid"
    if [[ -f "${pid_file}" ]] && kill -0 "$(<"${pid_file}")" 2>/dev/null; then
        echo "${name} is already running" >&2
        exit 1
    fi
    mkdir -p "${node_dir}/data" "${node_dir}/logs"
    connect_args=()
    if (( index > 0 )); then
        sleep 1
        previous=$((index - 1))
        connect_args=(--connect "127.0.0.1:${p2p_ports[${previous}]}")
    fi
    NOVACOIN_WALLET_PASSPHRASE="${NOVACOIN_WALLET_PASSPHRASE}" NOVACOIN_RPC_PASSWORD="${NOVACOIN_RPC_PASSWORD}" \
        "${daemon}" --regtest --name "${name}" --datadir "${node_dir}/data" \
        --p2pport "${p2p_ports[${index}]}" --rpcport "${rpc_ports[${index}]}" \
        --logfile "${node_dir}/logs/novacoind.log" "${connect_args[@]}" \
        >"${node_dir}/logs/stdout.log" 2>&1 &
    echo $! >"${pid_file}"
done

echo "started four TCP-connected regtest runtimes under ${regtest_dir}"
