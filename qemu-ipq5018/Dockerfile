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
    sed -i "/system_ss.add(when: 'CONFIG_COLLIE'/a system_ss.add(when: 'CONFIG_MR80X', if_true: files('mr80x.c'))" \
        /build/qemu-9.1.0/hw/arm/meson.build

WORKDIR /build/qemu-9.1.0
RUN ./configure --target-list=arm-softmmu --disable-docs --disable-tools \
        --disable-guest-agent --without-default-features \
        --enable-tcg --enable-slirp \
    && ninja -C build arm-softmmu/qemu-system-arm -j"$(nproc)"

WORKDIR /src
