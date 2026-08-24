FROM ubuntu:24.04 AS toolchain

RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        build-essential ninja-build python3 python3-pip pkg-config \
        libglib2.0-dev libpixman-1-dev libslirp-dev flex bison git \
        gcc-aarch64-linux-gnu bc device-tree-compiler libssl-dev \
    && rm -rf /var/lib/apt/lists/*

FROM toolchain AS qemu-build

WORKDIR /build
COPY vendor/qemu-9.1.0.tar.xz /build/
RUN tar xf qemu-9.1.0.tar.xz && rm qemu-9.1.0.tar.xz

COPY board/gxl_p211.c /build/qemu-9.1.0/hw/arm/gxl_p211.c
COPY board/Kconfig.fragment /build/kconfig.fragment
RUN cat /build/kconfig.fragment >> /build/qemu-9.1.0/hw/arm/Kconfig && \
    sed -i "/arm_ss.add(when: 'CONFIG_HIGHBANK'/a arm_ss.add(when: 'CONFIG_GXL_P211', if_true: files('gxl_p211.c'))" \
        /build/qemu-9.1.0/hw/arm/meson.build

WORKDIR /build/qemu-9.1.0
RUN ./configure --target-list=aarch64-softmmu --disable-docs --disable-tools \
        --disable-guest-agent --without-default-features --enable-tcg \
    && ninja -C build qemu-system-aarch64 -j"$(nproc)"

FROM toolchain AS uboot-build

# The factory BL33 is inside an encrypted/signed Amlogic FIP. Build an
# auditable GXL BL33 instead and enter it at the same architectural handoff.
WORKDIR /build
RUN git clone --depth 1 --branch v2024.01 \
        https://source.denx.de/u-boot/u-boot.git u-boot
WORKDIR /build/u-boot
COPY uboot/0001-arm64-allow-explicit-aarch32-boot.patch /build/
RUN git apply /build/0001-arm64-allow-explicit-aarch32-boot.patch && \
    make CROSS_COMPILE=aarch64-linux-gnu- p212_defconfig && \
    scripts/config --enable ANDROID_BOOT_IMAGE && \
    scripts/config --enable ARMV8_SWITCH_TO_EL1 && \
    scripts/config --set-str BOOTCOMMAND \
      'mmc dev 2; mmc read 0x08000000 0x2ae000 0x8000; setenv boot_aarch32 yes; setenv fdt_high 0x10000000; setenv initrd_high 0x10000000; setenv bootargs console=ttyS0,115200 no_console_suspend selinux=1 androidboot.selinux=permissive androidboot.dtbo_idx=0 buildvariant=userdebug; bootm 0x08000000 0x08000000 0x30000000' && \
    make CROSS_COMPILE=aarch64-linux-gnu- olddefconfig && \
    make CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" u-boot.bin

FROM toolchain

COPY --from=qemu-build /build/qemu-9.1.0/build/qemu-system-aarch64 \
    /build/qemu-9.1.0/build/qemu-system-aarch64
COPY --from=uboot-build /build/u-boot/u-boot.bin /build/u-boot/u-boot.bin

WORKDIR /src
