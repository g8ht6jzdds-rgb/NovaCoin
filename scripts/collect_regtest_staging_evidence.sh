#!/usr/bin/env bash
# Collect redacted, factual evidence from an actual REGTEST staging host.
# It never starts, stops, creates, or reconfigures a service; operators perform
# lifecycle actions under their approved change process between --phase before
# and --phase after. TESTNET and MAINNET are intentionally out of scope.
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
usage:
  collect_regtest_staging_evidence.sh --mode docker|systemd --candidate <40-hex> \
    --binary <path> --data-dir <absolute-path> --log-dir <absolute-path> \
    --output <new-record.txt> --phase before|after [options]

docker options:
  --compose <docker-compose.regtest.yml> --env-file <redacted-secret-file-path>
  [--container <container-id>]
systemd options:
  [--service novacoind-regtest-staging.service]
common options:
  [--baseline <before-record.txt>] [--external-probe <redacted-probe-output>]
  [--sign-key <full-openpgp-fingerprint>]
EOF
    exit 2
}

mode=""
candidate=""
binary=""
data_dir=""
log_dir=""
output=""
phase=""
compose=""
env_file=""
container=""
service="novacoind-regtest-staging.service"
baseline=""
external_probe=""
sign_key=""

while (($# > 0)); do
    case "$1" in
    --mode) mode=${2:-}; shift 2 ;;
    --candidate) candidate=${2:-}; shift 2 ;;
    --binary) binary=${2:-}; shift 2 ;;
    --data-dir) data_dir=${2:-}; shift 2 ;;
    --log-dir) log_dir=${2:-}; shift 2 ;;
    --output) output=${2:-}; shift 2 ;;
    --phase) phase=${2:-}; shift 2 ;;
    --compose) compose=${2:-}; shift 2 ;;
    --env-file) env_file=${2:-}; shift 2 ;;
    --container) container=${2:-}; shift 2 ;;
    --service) service=${2:-}; shift 2 ;;
    --baseline) baseline=${2:-}; shift 2 ;;
    --external-probe) external_probe=${2:-}; shift 2 ;;
    --sign-key) sign_key=${2:-}; shift 2 ;;
    *) usage ;;
    esac
done

[[ "$mode" == docker || "$mode" == systemd ]] || usage
[[ "$candidate" =~ ^[0-9a-f]{40}$ ]] || usage
[[ -x "$binary" && "$data_dir" == /* && "$log_dir" == /* && -n "$output" ]] || usage
[[ "$phase" == before || "$phase" == after ]] || usage
[[ ! -e "$output" && ! -e "${output}.asc" && ! -e "${output}.firewall.txt" ]] || {
    echo "refusing to overwrite evidence: $output" >&2
    exit 2
}
if [[ "$mode" == docker ]]; then
    [[ -f "$compose" && -f "$env_file" ]] || usage
fi
if [[ "$phase" == after ]]; then
    [[ -f "$baseline" ]] || { echo "--baseline is required after recreation" >&2; exit 2; }
fi
[[ -n "$external_probe" && -f "$external_probe" ]] || {
    echo "an independently collected external probe file is required" >&2
    exit 2
}
if [[ -n "$sign_key" && "${EUID}" -eq 0 ]]; then
    echo "refusing to sign evidence as root; collect first, then sign as the named operator" >&2
    exit 2
fi

for command in sha256sum date uname awk grep sed ss; do
    command -v "$command" >/dev/null || { echo "missing command: $command" >&2; exit 2; }
done

output_directory=$(dirname -- "$output")
mkdir -p -- "$output_directory"
umask 077
record_file=$(mktemp "${output_directory}/.novacoin-staging-evidence.XXXXXX")
firewall_file="${record_file}.firewall.txt"
trap 'rm -f -- "$record_file" "$firewall_file"' EXIT

record() {
    # Callers supply single-line, redacted values only.
    printf '%s=%s\n' "$1" "$2" >>"$record_file"
}

digest_or_missing() {
    local path=$1
    if [[ -f "$path" ]]; then
        sha256sum -- "$path" | awk '{print $1}'
    else
        printf 'MISSING'
    fi
}

baseline_value() {
    local key=$1
    grep -E "^${key}=" -- "$baseline" | tail -n 1 | sed "s/^${key}=//"
}

capture_firewall() {
    if command -v firewall-cmd >/dev/null; then
        firewall-cmd --list-all >"$firewall_file" 2>&1 || true
        record firewall_collector firewall-cmd
    elif command -v ufw >/dev/null; then
        ufw status numbered >"$firewall_file" 2>&1 || true
        record firewall_collector ufw
    elif command -v nft >/dev/null; then
        nft list ruleset >"$firewall_file" 2>&1 || true
        record firewall_collector nft
    elif command -v iptables-save >/dev/null; then
        iptables-save >"$firewall_file" 2>&1 || true
        record firewall_collector iptables-save
    else
        printf 'No supported local firewall collector found.\n' >"$firewall_file"
        record firewall_collector unavailable
    fi
    chmod 0600 -- "$firewall_file"
    record firewall_record_sha256 "$(digest_or_missing "$firewall_file")"
}

record schema novacoin-regtest-staging-evidence-v1
record status PENDING_OPERATOR_REVIEW
record collected_utc "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
record network REGTEST
record mode "$mode"
record lifecycle_phase "$phase"
record candidate_commit "$candidate"
record host_kernel "$(uname -srmo | tr ' ' '_')"
record binary_path "$binary"
record binary_sha256 "$(digest_or_missing "$binary")"
record data_dir "$data_dir"
record log_dir "$log_dir"
network_identity_sha256=$(digest_or_missing "$data_dir/network.identity")
wallet_sha256=$(digest_or_missing "$data_dir/wallet.dat")
journal_sha256=$(digest_or_missing "$data_dir/blocks.dat")
[[ "$network_identity_sha256" != MISSING ]] || {
    echo "network identity marker is missing" >&2
    exit 1
}
[[ "$wallet_sha256" != MISSING ]] || { echo "wallet file is missing" >&2; exit 1; }
[[ "$journal_sha256" != MISSING ]] || { echo "block journal is missing" >&2; exit 1; }
record network_identity_sha256 "$network_identity_sha256"
record wallet_file_present yes
record wallet_sha256 "$wallet_sha256"
record journal_sha256 "$journal_sha256"

if [[ "$phase" == after ]]; then
    expected_identity=$(baseline_value network_identity_sha256)
    actual_identity=$(digest_or_missing "$data_dir/network.identity")
    [[ "$expected_identity" != MISSING && "$actual_identity" == "$expected_identity" ]] || {
        echo "persistent network identity did not survive recreation" >&2
        exit 1
    }
    record persistent_volume_identity PASS
    [[ -s "$data_dir/wallet.dat" ]] || { echo "wallet file is missing after recreation" >&2; exit 1; }
    record persistent_wallet_file PASS
fi

if [[ "$mode" == docker ]]; then
    command -v docker >/dev/null || { echo "missing command: docker" >&2; exit 2; }
    # Validate compose syntax but intentionally do not capture its output: it
    # could contain interpolated secrets from the environment file.
    docker compose --env-file "$env_file" -f "$compose" config --quiet
    if [[ -z "$container" ]]; then
        container=$(docker compose --env-file "$env_file" -f "$compose" ps -q novacoind)
    fi
    [[ -n "$container" ]] || { echo "no novacoind container found" >&2; exit 1; }
    image_id=$(docker inspect --format '{{.Image}}' "$container")
    image_revision=$(docker image inspect --format '{{ index .Config.Labels "org.opencontainers.image.revision" }}' "$image_id")
    base_image=$(docker image inspect --format '{{ index .Config.Labels "org.opencontainers.image.base.name" }}' "$image_id")
    configured_user=$(docker inspect --format '{{.Config.User}}' "$container")
    container_pid=$(docker inspect --format '{{.State.Pid}}' "$container")
    mounts=$(docker inspect --format '{{range .Mounts}}{{.Destination}}={{.Source}};{{end}}' "$container")
    ports=$(docker inspect --format '{{json .NetworkSettings.Ports}}' "$container")
    runtime_uid=$(ps -o uid= -p "$container_pid" 2>/dev/null | tr -d '[:space:]' || true)
    container_binary_sha256=$(docker exec "$container" sha256sum /usr/local/bin/novacoind \
        | awk '{print $1}')
    [[ "$configured_user" == novacoin:novacoin && -n "$runtime_uid" && "$runtime_uid" != 0 ]] || {
        echo "container is not running as novacoin" >&2
        exit 1
    }
    [[ "$image_revision" == "$candidate" ]] || {
        echo "image OCI revision label does not match candidate" >&2
        exit 1
    }
    [[ "$base_image" == *@sha256:* ]] || {
        echo "image base label is not digest pinned" >&2
        exit 1
    }
    [[ "$mounts" == *"/var/lib/novacoin/regtest=${data_dir}"* &&
       "$mounts" == *"/var/log/novacoin=${log_dir}"* ]] || {
        echo "expected persistent host bind mounts are absent" >&2
        exit 1
    }
    [[ "$ports" != *HostPort* ]] || {
        echo "REGTEST container must not publish a host port" >&2
        exit 1
    }
    record docker_container "$container"
    record docker_image_id "$image_id"
    record docker_oci_revision "$image_revision"
    record docker_base_image "$base_image"
    record docker_config_user "$configured_user"
    record docker_runtime_uid "$runtime_uid"
    record docker_binary_sha256 "$container_binary_sha256"
    record docker_mounts "$mounts"
    record docker_published_ports "$ports"
else
    command -v systemctl >/dev/null || { echo "missing command: systemctl" >&2; exit 2; }
    service_state=$(systemctl is-active "$service" 2>/dev/null || true)
    properties=$(systemctl show "$service" --property=User --property=Group --property=ReadWritePaths \
        --property=ProtectSystem --property=NoNewPrivileges --property=MainPID --no-pager)
    [[ "$service_state" == active ]] || { echo "systemd service is not active" >&2; exit 1; }
    grep -qx 'User=novacoin' <<<"$properties" || { echo "systemd service user is not novacoin" >&2; exit 1; }
    grep -qx 'Group=novacoin' <<<"$properties" || { echo "systemd service group is not novacoin" >&2; exit 1; }
    grep -qx 'ProtectSystem=strict' <<<"$properties" || { echo "systemd protection is incomplete" >&2; exit 1; }
    grep -qx 'NoNewPrivileges=yes' <<<"$properties" || { echo "systemd privilege control is incomplete" >&2; exit 1; }
    read_write_paths=$(grep '^ReadWritePaths=' <<<"$properties" || true)
    [[ "$read_write_paths" == *"/var/lib/novacoin/regtest"* &&
       "$read_write_paths" == *"/var/log/novacoin"* ]] || {
        echo "systemd writable-state paths are incomplete" >&2
        exit 1
    }
    systemd_pid=$(grep '^MainPID=' <<<"$properties" | sed 's/^MainPID=//')
    systemd_uid=$(ps -o uid= -p "$systemd_pid" 2>/dev/null | tr -d '[:space:]' || true)
    [[ -n "$systemd_uid" && "$systemd_uid" != 0 ]] || {
        echo "systemd service is running as root or has no process" >&2
        exit 1
    }
    record systemd_service "$service"
    record systemd_active "$service_state"
    while IFS= read -r line; do
        record "systemd_${line%%=*}" "${line#*=}"
    done <<<"$properties"
    record systemd_runtime_uid "$systemd_uid"
fi

capture_firewall
record external_probe_sha256 "$(digest_or_missing "$external_probe")"
record external_probe_reference "$external_probe"
listeners=$(ss -H -ltn '( sport = :18443 or sport = :18444 )' 2>/dev/null | tr '\n' ';' || true)
[[ "$listeners" != *"0.0.0.0:"* && "$listeners" != *"[::]:"* ]] || {
    echo "REGTEST listener is bound publicly" >&2
    exit 1
}
record local_listeners "$listeners"
if [[ "$phase" == after ]]; then
    grep -Fq 'novacoind shutdown complete' "$log_dir/regtest.log" || {
        echo "clean shutdown was not found in the persistent daemon log" >&2
        exit 1
    }
    record clean_shutdown_log present
fi
record result PASS_PENDING_OPERATOR_SIGNATURE
if [[ -n "$sign_key" ]]; then
    record detached_signature "${output}.asc"
fi

chmod 0600 -- "$record_file" "$firewall_file"
if [[ -n "$sign_key" ]]; then
    command -v gpg >/dev/null || { echo "missing command: gpg" >&2; exit 2; }
    gpg --batch --yes --armor --local-user "$sign_key" --detach-sign \
        --output "${record_file}.asc" "$record_file"
fi
mv -- "$record_file" "$output"
mv -- "$firewall_file" "${output}.firewall.txt"
if [[ -n "$sign_key" ]]; then
    mv -- "${record_file}.asc" "${output}.asc"
fi

chmod 0600 -- "$output"
printf 'Wrote staging evidence: %s\n' "$output"
