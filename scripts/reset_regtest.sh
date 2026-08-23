#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
regtest_dir="${repo_root}/regtest"
resolved_root=$(realpath -m "${repo_root}")
resolved_regtest=$(realpath -m "${regtest_dir}")

"${repo_root}/scripts/stop_regtest.sh"
if [[ "${resolved_regtest}" != "${resolved_root}/regtest" ]]; then
    echo "refusing unexpected regtest path" >&2
    exit 1
fi
rm -rf -- "${resolved_regtest}"
mkdir -p "${regtest_dir}"
echo "reset ${regtest_dir}"
