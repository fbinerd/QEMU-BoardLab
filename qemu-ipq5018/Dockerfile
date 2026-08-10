FROM ubuntu:24.04

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        build-essential ninja-build python3 python3-pip pkg-config \
        libglib2.0-dev libpixman-1-dev libslirp-dev \
        flex bison git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY vendor/qemu-9.1.0.tar.xz /build/
RUN tar xf qemu-9.1.0.tar.xz && rm qemu-9.1.0.tar.xz

COPY board/mr80x.c /build/qemu-9.1.0/hw/arm/mr80x.c
COPY board/Kconfig.fragment /build/kconfig.fragment
RUN cat /build/kconfig.fragment >> /build/qemu-9.1.0/hw/arm/Kconfig && \
    sed -i "/arm_ss.add(when: 'CONFIG_HIGHBANK'/a arm_ss.add(when: 'CONFIG_MR80X', if_true: files('mr80x.c'))" \
        /build/qemu-9.1.0/hw/arm/meson.build

WORKDIR /build/qemu-9.1.0
# aarch64-softmmu instead of arm-softmmu: it's a superset (includes every
# 32-bit ARM CPU model - cortex-a7 etc, this machine's existing default -
# *plus* AArch64-capable ones like cortex-a53) built from the exact same
# hw/arm/mr80x.c via the same shared `arm_ss` source set every other ARM
# board file uses regardless of target. Needed once real hardware turned
# out to be AArch64-capable (BRINGUP-NOTES.md section 28) - qemu-system-arm
# genuinely cannot instantiate a cortex-a53 CPU at all, "unable to find CPU
# model" is a hard target-list limitation, not a machine/board issue.
RUN ./configure --target-list=aarch64-softmmu --disable-docs --disable-tools \
        --disable-guest-agent --without-default-features \
        --enable-tcg --enable-slirp \
    && ninja -C build qemu-system-aarch64 -j"$(nproc)"

RUN DEBIAN_FRONTEND=noninteractive apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gdb-multiarch \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
