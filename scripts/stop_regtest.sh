#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
regtest_dir="${repo_root}/regtest"

for name in alpha bravo charlie delta; do
    pid_file="${regtest_dir}/${name}/novacoind.pid"
    if [[ -f "${pid_file}" ]]; then
        pid=$(<"${pid_file}")
        if kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}"
        fi
        rm -f "${pid_file}"
    fi
done

echo "stopped regtest runtimes"
