# Guitar Tuner Module Build Environment
# Targets: Ableton Move (aarch64 Linux)
# Single .c file with libm — small, C11 is fine.

FROM debian:bookworm

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    make \
    file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV CC=aarch64-linux-gnu-gcc
