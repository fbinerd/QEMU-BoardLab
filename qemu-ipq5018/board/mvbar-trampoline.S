/* Source for the Monitor-mode SMC trampoline hand-loaded into guest RAM
 * by mr80x_populate_ram() in mr80x.c (MR80X_MVBAR_BASE + MR80X_MVBAR_SMC_OFF).
 * Not built as part of the QEMU Docker image - this is a one-off input to
 * produce the `trampoline[]` byte array committed directly in mr80x.c, kept
 * here only so that array is reproducible/auditable instead of hand-derived.
 *
 * Reassemble with the same cross-toolchain this project's own appsbl build
 * already uses:
 *
 *   docker run --rm -v "$PWD:/w" -w /w mr80x-appsbl-builder:openwrt-gcc5.2-binutils2.24 bash -c '
 *     /opt/openwrt-toolchain/bin/arm-openwrt-linux-muslgnueabi-as -mcpu=cortex-a7 \
 *         board/mvbar-trampoline.S -o /tmp/t.o &&
 *     /opt/openwrt-toolchain/bin/arm-openwrt-linux-muslgnueabi-objcopy -O binary /tmp/t.o /tmp/t.bin &&
 *     xxd -i /tmp/t.bin
 *   '
 *
 * MR80X_HANDOFF_TRIGGER_BASE (0x0A000000) is baked in directly below - if
 * that #define in mr80x.c ever changes, update the movw/movt pair here to
 * match and re-generate the byte array.
 */
        .arm
        .text
        .global _start
_start:
        /* is_scm_armv8() probe: fn_id 0x82000601 -> "yes, armv8 TZ"
         * (r0=0, r1=1) - see arch/arm/cpu/armv7/qca/common/scm.c's
         * is_scm_armv8(): with a real, no-monitor-firmware trampoline
         * this probe always failed, caching scm_version=SCM_LEGACY for
         * the rest of the boot, which makes jump_kernel64() hang()
         * before ever issuing the real SMC below. */
        movw    ip, #0x0601
        movt    ip, #0x8200
        cmp     r0, ip
        bne     1f
        mov     r0, #0
        mov     r1, #1
        movs    pc, lr

1:      /* jump_kernel64() SMC: fn_id 0x0210010f = QCA_SCM_FNID(
         * SCM_ARCH64_SWITCH_ID=1, SCM_EL1SWITCH_CMD_ID=0xf,
         * SCM_OWNR_SIP=2). r2 holds the physical address of scm.c's
         * on-stack `kernel_params` struct (reg_x0=fdt addr at offset
         * 0, kernel_start at offset 72) - hand it to
         * mr80x_handoff_trigger_write() via a plain STR to a
         * dedicated MMIO register. */
        movw    ip, #0x010f
        movt    ip, #0x0210
        cmp     r0, ip
        bne     2f
        movw    ip, #0x0000      /* MR80X_HANDOFF_TRIGGER_BASE & 0xffff */
        movt    ip, #0x0A00      /* MR80X_HANDOFF_TRIGGER_BASE >> 16 */
        str     r2, [ip]

2:      /* Shared default: no real secure firmware present. Also the
         * fallthrough after the STR above - matches jump_kernel64()'s
         * own "SMC never returns on real hardware" expectation:
         * appsbl just spins in hang() until our MMIO write's
         * qemu_system_reset_request() actually lands. */
        mvn     r0, #3
        movs    pc, lr
