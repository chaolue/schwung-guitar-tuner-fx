# Guitar Tuner FX build environment
# Targets: Ableton Move (aarch64 Linux)
# One .c file with libm, plus python3 to generate the served contract.

FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu \
        libc6-dev-arm64-cross \
        python3 \
        file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV CC=aarch64-linux-gnu-gcc
