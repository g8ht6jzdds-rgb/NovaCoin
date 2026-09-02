# NovaCoin REGTEST staging Docker and systemd validation

**Status:** **PENDING — NO STAGING HOST EVIDENCE RECORDED**

This procedure validates deployment mechanics with REGTEST only. It must not pass `--testnet`, alter `TestnetNetworkParams`, or publish RPC. Success is staging evidence, not TESTNET or MAINNET authorization.

## Host preparation

Run only on an approved staging host under its change process.

```bash
sudo useradd --system --user-group --home-dir /var/lib/novacoin --no-create-home novacoin
sudo install -d -o novacoin -g novacoin -m 0750 /var/lib/novacoin/regtest /var/log/novacoin
sudo install -d -o root -g novacoin -m 0750 /etc/novacoin
sudo install -o root -g novacoin -m 0600 contrib/staging/regtest-staging.env.example /etc/novacoin/regtest-staging.env
sudo install -d -o novacoin -g novacoin -m 0750 /srv/novacoin/regtest/data /srv/novacoin/regtest/log
```

Replace placeholders in the mode-`0600` file or use a secret manager. Never commit a populated secret file, token, wallet file, or command output. Record the exact source commit, signed tag, image digest, base-image digest, build command, and UTC build time in protected host evidence; a mutable tag is insufficient.

For Docker, create `/etc/novacoin/regtest-docker.env` from the example, make
it `root:novacoin` mode `0600`, and use it explicitly as Compose's
interpolation file. The two host directories above are the example bind-mount
locations; if the values in
`NOVACOIN_REGTEST_DATA_DIR` or `NOVACOIN_REGTEST_LOG_DIR` differ, create and
own those exact directories instead. Confirm the host ownership maps to the
container's `novacoin` UID/GID before starting it. Do not use `chmod 777` to
work around an ownership mismatch.

## Docker REGTEST staging checks

Use [docker-compose.regtest.yml](../contrib/staging/docker-compose.regtest.yml), not the TESTNET template:

```bash
sudo install -o root -g novacoin -m 0600 contrib/staging/regtest-staging.env.example /etc/novacoin/regtest-docker.env
# Populate /etc/novacoin/regtest-docker.env through the approved secret-manager workflow.
sudo docker compose --env-file /etc/novacoin/regtest-docker.env -f contrib/staging/docker-compose.regtest.yml config
sudo docker compose --env-file /etc/novacoin/regtest-docker.env -f contrib/staging/docker-compose.regtest.yml up -d --build
sudo docker compose --env-file /etc/novacoin/regtest-docker.env -f contrib/staging/docker-compose.regtest.yml ps
container_id=$(sudo docker compose --env-file /etc/novacoin/regtest-docker.env -f contrib/staging/docker-compose.regtest.yml ps -q novacoind)
sudo docker inspect "$container_id"
sudo ss -ltnp
sudo docker image inspect "$(sudo docker inspect --format '{{.Image}}' "$container_id")"
```

Verify and record: process UID/GID is non-root; the REGTEST compose profile publishes no ports; RPC `18443` is not published and remains loopback-only; the firewall exposes no RPC port; data/log binds are outside the container; image digest and source commit match the release record. TESTNET `28333` public-P2P exposure is a separate, gated-host validation after approval.

```bash
sudo docker compose --env-file /etc/novacoin/regtest-docker.env -f contrib/staging/docker-compose.regtest.yml stop
sudo docker compose --env-file /etc/novacoin/regtest-docker.env -f contrib/staging/docker-compose.regtest.yml rm -f
sudo sh -c '. /etc/novacoin/regtest-docker.env; test -f "$NOVACOIN_REGTEST_DATA_DIR/network.identity"; test -f "$NOVACOIN_REGTEST_DATA_DIR/wallet.dat"'
sudo docker compose --env-file /etc/novacoin/regtest-docker.env -f contrib/staging/docker-compose.regtest.yml up -d
sudo docker compose --env-file /etc/novacoin/regtest-docker.env -f contrib/staging/docker-compose.regtest.yml logs --tail=200 novacoind
```

After recreation, query authenticated local RPC through a local execution context or private tunnel. Retain only redacted `getnodehealth`, `getblockchaininfo`, and `getnodemetrics` fields. Compare recovered height/tip, journal result, and encrypted-wallet persistence result with pre-restart data.

For a reproducible disposable local exercise (not a substitute for real-host
evidence), run:

```bash
bash scripts/regtest_staging_recovery_exercise.sh \
  /path/to/novacoind /secure/redacted/artifact-root
```

It starts a REGTEST node, mines three blocks, records height/tip/chainwork and
file digests, performs a `SIGTERM` shutdown, verifies encrypted-wallet file
persistence, restarts, appends one deliberately incomplete final journal tag
while stopped, and verifies that restart retains the committed prefix. Its
record is deliberately unsigned: a real staging operator must review and sign
the redacted record before it can satisfy this gate.

## systemd REGTEST staging checks

Install [novacoind-regtest-staging.service](../contrib/staging/novacoind-regtest-staging.service) only after reviewing the binary and environment file:

```bash
sudo install -o root -g root -m 0644 contrib/staging/novacoind-regtest-staging.service /etc/systemd/system/novacoind-regtest-staging.service
sudo systemctl daemon-reload
sudo systemctl enable --now novacoind-regtest-staging
sudo systemctl status novacoind-regtest-staging
sudo journalctl -u novacoind-regtest-staging --since "30 minutes ago"
sudo systemctl stop novacoind-regtest-staging
sudo systemctl start novacoind-regtest-staging
```

Record non-root execution, start/stop exit results, `novacoind shutdown complete` evidence, post-restart height/tip, journal replay outcome, and encrypted-wallet load/save result. Any journal or wallet persistence failure is a release blocker: preserve data, stop automatic recovery attempts, and follow the durability/rollback runbook.

## Deferred TESTNET port validation

Do not execute this section until a separately reviewed activation commit makes
TESTNET available. Then, on each approved TESTNET host, run the requested
checks against the testnet profile and retain the redacted output:

```bash
docker compose -f contrib/testnet/docker-compose.testnet.yml config
docker compose -f contrib/testnet/docker-compose.testnet.yml ps
container_id=$(docker compose -f contrib/testnet/docker-compose.testnet.yml ps -q novacoind)
docker inspect "$container_id"
ss -ltnp
```

The acceptance criteria are: `28333` is the only publicly reachable node port;
`28332` has no Docker published-port mapping and the RPC listener is limited to
loopback; the process UID is `novacoin`, not root; host data and log volumes
survive recreation; and image/base digests plus the candidate commit match the
signed release record. A failed criterion blocks deployment rather than being
remediated by loosening a firewall or exposing RPC.

## Required redacted evidence record

| Field | Required redacted value |
| --- | --- |
| Host/provider account alias | Pending |
| Host region and firewall review UTC date | Pending |
| Source commit / signed tag | Pending |
| Image and base-image digests | Pending |
| Docker config/inspect/port evidence reference | Pending |
| Non-root UID/GID evidence | Pending |
| Persistent-volume recreation result | Pending |
| systemd start/stop/clean-shutdown result | Pending |
| Recovered height, tip, journal, and wallet result | Pending |
| Signer and detached signature reference | Pending |
| Status | Pending |

Do not mark this record complete until real host outputs are independently reviewed.
