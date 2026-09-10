#!/usr/bin/env bash
# Deterministic parser/guard and simulated systemd lifecycle checks.
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

# PATH fixtures simulate commands, not real host/deployment evidence. Real
# files below are disposable and contain no wallet or credential material.
mkdir -p "$temporary_directory/bin" "$temporary_directory/data" "$temporary_directory/logs"
for name in network.identity wallet.dat blocks.dat; do
    printf 'fixture only\n' >"$temporary_directory/data/$name"
done
cat >"$temporary_directory/bin/fixture" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "${0##*/}" in
systemctl)
    if [[ "$1" == is-active ]]; then echo active
    elif [[ "$*" == *--value* ]]; then echo "${FIXTURE_RACE:-$FIXTURE_INVOCATION}"
    else
        printf 'User=novacoin\nGroup=novacoin\nProtectSystem=strict\nNoNewPrivileges=yes\nMainPID=123\nReadWritePaths=/var/lib/novacoin/regtest /var/log/novacoin\nInvocationID=%s\n' "$FIXTURE_INVOCATION"
    fi ;;
ps) echo 997 ;;
ss)
    [[ "${FIXTURE_SS_FAIL:-0}" == 0 ]] || exit 9
    printf '%s' "$FIXTURE_LISTENERS" ;;
firewall-cmd) echo 'SIMULATED firewall record, not deployment evidence' ;;
journalctl)
    [[ "${FIXTURE_JOURNAL_FAIL:-0}" == 0 ]] || exit 9
    [[ "$*" == *--unit=novacoind-regtest-staging.service* ]] || exit 8
    if [[ "$*" == *--show-cursor* ]]; then
        [[ "${FIXTURE_NO_CURSOR:-0}" == 0 ]] || exit 0
        printf 'earlier message\n-- cursor: fixture100\n'
    else
        cursor=""; invocation=""
        for arg in "$@"; do
            case "$arg" in
                --after-cursor=*) cursor=${arg#*=} ;;
                _SYSTEMD_INVOCATION_ID=*) invocation=${arg#*=} ;;
            esac
        done
        [[ "$cursor" == fixture100 && -n "$invocation" ]] || exit 8
        awk -F '|' -v inv="$invocation" '$1 > 100 && $2 == inv && $3 ~ /^novacoind shutdown complete .+/ {print $3}' "$FIXTURE_JOURNAL"
    fi ;;
*) exit 8 ;;
esac
EOF
chmod +x "$temporary_directory/bin/fixture"
for name in systemctl ps ss firewall-cmd journalctl; do
    ln -s fixture "$temporary_directory/bin/$name"
done
export PATH="$temporary_directory/bin:$PATH"
export FIXTURE_INVOCATION=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
export FIXTURE_JOURNAL="$temporary_directory/journal-fixture"
export FIXTURE_LISTENERS=$'LISTEN 0 32 127.0.0.1:18443 0.0.0.0:*\nLISTEN 0 4096 [::1]:18444 [::]:*\n'
good_listeners=$FIXTURE_LISTENERS
common=(--mode systemd --candidate "$candidate" --binary /usr/bin/true
    --data-dir "$temporary_directory/data" --log-dir "$temporary_directory/logs"
    --external-probe "$temporary_directory/external-probe.txt")
before="$temporary_directory/before.txt"
expect_exit 0 "$collector" "${common[@]}" --output "$before" --phase before
grep -Fxq 'systemd_journal_cursor=fixture100' "$before"
grep -Fxq "systemd_InvocationID=$FIXTURE_INVOCATION" "$before"
test ! -e "$temporary_directory/logs/regtest.log"

number=0
for local_address in '0.0.0.0:18443' '[::]:18443' '*:18443' '10.0.0.14:18443'; do
    export FIXTURE_LISTENERS="LISTEN 0 32 $local_address 0.0.0.0:*"
    number=$((number+1))
    expect_exit 1 "$collector" "${common[@]}" --phase before --output "$temporary_directory/public-$number.txt"
    test ! -e "$temporary_directory/public-$number.txt"
done
for bad in '' 'unparseable output' 'LISTEN 0 32 127.0.0.1:18443 0.0.0.0:*'; do
    export FIXTURE_LISTENERS=$bad
    number=$((number+1))
    expect_exit 1 "$collector" "${common[@]}" --phase before --output "$temporary_directory/missing-$number.txt"
done
export FIXTURE_LISTENERS=$good_listeners FIXTURE_SS_FAIL=1
expect_exit 1 "$collector" "${common[@]}" --phase before --output "$temporary_directory/ss-failure.txt"
export FIXTURE_SS_FAIL=0 FIXTURE_NO_CURSOR=1
expect_exit 1 "$collector" "${common[@]}" --phase before --output "$temporary_directory/no-cursor.txt"
export FIXTURE_NO_CURSOR=0

# A new invocation alone is insufficient: messages before the baseline cursor
# and messages from other invocations must not count.
export FIXTURE_INVOCATION=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
printf '99|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|novacoind shutdown complete stale\n110|cccccccccccccccccccccccccccccccc|novacoind shutdown complete unrelated\n' >"$FIXTURE_JOURNAL"
expect_exit 1 "$collector" "${common[@]}" --phase after --baseline "$before" --output "$temporary_directory/stale.txt"
test ! -e "$temporary_directory/stale.txt"
printf '111|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|novacoind shutdown complete fixture-node\n' >>"$FIXTURE_JOURNAL"
expect_exit 0 "$collector" "${common[@]}" --phase after --baseline "$before" --output "$temporary_directory/after.txt"
grep -Fxq 'clean_shutdown_log=journald' "$temporary_directory/after.txt"
grep -Fxq 'systemd_shutdown_message=novacoind shutdown complete fixture-node' "$temporary_directory/after.txt"

export FIXTURE_INVOCATION=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
expect_exit 1 "$collector" "${common[@]}" --phase after --baseline "$before" --output "$temporary_directory/no-restart.txt"
export FIXTURE_INVOCATION=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb FIXTURE_JOURNAL_FAIL=1
expect_exit 1 "$collector" "${common[@]}" --phase after --baseline "$before" --output "$temporary_directory/journal-failure.txt"
export FIXTURE_JOURNAL_FAIL=0 FIXTURE_RACE=cccccccccccccccccccccccccccccccc
expect_exit 1 "$collector" "${common[@]}" --phase before --output "$temporary_directory/race.txt"
unset FIXTURE_RACE
sed '/^systemd_journal_cursor=/d' "$before" >"$temporary_directory/legacy.txt"
expect_exit 1 "$collector" "${common[@]}" --phase after --baseline "$temporary_directory/legacy.txt" --output "$temporary_directory/legacy-after.txt"
sed 's/^candidate_commit=.*/candidate_commit=ffffffffffffffffffffffffffffffffffffffff/' "$before" >"$temporary_directory/wrong-candidate.txt"
expect_exit 1 "$collector" "${common[@]}" --phase after --baseline "$temporary_directory/wrong-candidate.txt" --output "$temporary_directory/wrong-after.txt"
cp "$before" "$temporary_directory/duplicate.txt"
printf 'systemd_journal_cursor=fixture100\n' >>"$temporary_directory/duplicate.txt"
expect_exit 1 "$collector" "${common[@]}" --phase after --baseline "$temporary_directory/duplicate.txt" --output "$temporary_directory/duplicate-after.txt"

echo "staging collector listener and lifecycle regression fixtures passed (not real-host evidence)"
