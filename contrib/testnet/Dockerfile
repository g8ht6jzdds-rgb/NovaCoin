# Pre-approval TESTNET deployment image. Build from an immutable source commit:
#   docker build -f contrib/testnet/Dockerfile -t novacoin-testnet:<revision> .
# This image does not enable TESTNET; novacoind will fail closed until the
# reviewed immutable parameter gate is opened.
FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ make ninja-build libgtest-dev libssl-dev libsecp256k1-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /source
COPY . .
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --target novacoind

FROM debian:bookworm-slim

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libgcc-s1 libstdc++6 libssl3 libsecp256k1-1 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system novacoin \
    && useradd --system --gid novacoin --home-dir /var/lib/novacoin --no-create-home novacoin \
    && mkdir -p /var/lib/novacoin/testnet /var/log/novacoin \
    && chown -R novacoin:novacoin /var/lib/novacoin /var/log/novacoin

COPY --from=builder /build/src/daemon/novacoind /usr/local/bin/novacoind

USER novacoin:novacoin
VOLUME ["/var/lib/novacoin/testnet"]
ENTRYPOINT ["/usr/local/bin/novacoind"]
