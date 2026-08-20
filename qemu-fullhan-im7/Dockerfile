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

COPY board/im7cam.c /build/qemu-9.1.0/hw/arm/im7cam.c
COPY board/Kconfig.fragment /build/kconfig.fragment
RUN cat /build/kconfig.fragment >> /build/qemu-9.1.0/hw/arm/Kconfig && \
    sed -i "/arm_ss.add(when: 'CONFIG_HIGHBANK'/a arm_ss.add(when: 'CONFIG_IM7CAM', if_true: files('im7cam.c'))" \
        /build/qemu-9.1.0/hw/arm/meson.build

WORKDIR /build/qemu-9.1.0
# arm-softmmu, not aarch64-softmmu: unlike MR80X, this device is confirmed
# 32-bit ARM only (Linux zImage, U-Boot built AArch32 - see
# BRINGUP-NOTES.md section 3). Also keeps this board's build completely
# separate from qemu-ipq5018's own aarch64-softmmu build - different
# target binary, different container image, no shared state.
RUN ./configure --target-list=arm-softmmu --disable-docs --disable-tools \
        --disable-guest-agent --without-default-features \
        --enable-tcg --enable-slirp \
    && ninja -C build qemu-system-arm -j"$(nproc)"

RUN DEBIAN_FRONTEND=noninteractive apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gdb-multiarch \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
