FROM ubuntu:24.04

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        qemu-system-arm \
        gcc-arm-none-eabi \
        binutils-arm-none-eabi \
        make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
