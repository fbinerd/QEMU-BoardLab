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
 *
 * Two SMC *calling conventions* coexist in this codebase's scm.c, and this
 * trampoline must answer each correctly:
 *
 *  - "legacy" (scm_call() -> __scm_call() -> smc()): r0 is *always* 1 on
 *    entry (a fixed trap value, not a function ID - the real command lives
 *    in a memory-pointed cmd_addr, per this SoC family's original SCM ABI).
 *    __scm_call() passes whatever this trampoline returns in r0 through
 *    scm_remap_error() itself, which maps the *raw* SCM_EOPNOTSUPP (-4)
 *    into the C errno -EOPNOTSUPP (-95) for its caller.
 *  - "armv8" (scm_call_64() -> __scm_call_64() -> __qca_scm_call_armv8_32()):
 *    r0 is the actual QCA_SCM_FNID-encoded function ID (always a large,
 *    specific value - never 1). __scm_call_64() does *not* call
 *    scm_remap_error() - whatever this trampoline returns in r0 is used
 *    directly, raw, by the caller.
 *
 * Getting this wrong is exactly why an earlier version of this trampoline
 * silently broke do_bootipq(): it always returned raw -4 for anything
 * unrecognized. do_bootipq()'s QFPROM check (SCM_SVC_FUSE,
 * QFPROM_IS_AUTHENTICATE_CMD) goes through the armv8 convention once
 * is_scm_armv8() answers true (needed at all for jump_kernel64() to ever
 * fire - see the comment above mr80x_handoff_trigger_write() in mr80x.c),
 * so its `ret` never gets remapped - comparing raw -4 against `ret ==
 * -EOPNOTSUPP` (i.e. against -95) is always false, `ret == 0` is also
 * false, so neither the do_boot_signedimg() nor do_boot_unsignedimg()
 * branch in do_bootipq() ever runs, silently, no error printed at all.
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

1:      /* jump_kernel64() SMC: fn_id 0x0200010f = QCA_SCM_FNID(
         * SCM_ARCH64_SWITCH_ID=1, SCM_EL1SWITCH_CMD_ID=0xf,
         * SCM_OWNR_SIP=2) = ((1<<8)|0xf) | (2<<24) - confirmed against
         * a real boot (an earlier "0x0210010f" here was a hex-digit
         * transcription slip: 0x10f | 0x02000000 is 0x0200010f, not
         * 0x0210010f - caught via a temporary debug trampoline variant
         * that logged the real fn_id for any unmatched armv8-convention
         * SMC). r2 holds the physical address of scm.c's
         * on-stack `kernel_params` struct (reg_x0=fdt addr at offset
         * 0, kernel_start at offset 72) - hand it to
         * mr80x_handoff_trigger_write() via a plain STR to a
         * dedicated MMIO register, then fall through to the armv8
         * default return below (this SMC's own return value doesn't
         * matter on a real handoff - jump_kernel64() never returns on
         * real hardware either - but keeping it correct costs
         * nothing). */
        movw    ip, #0x010f
        movt    ip, #0x0200
        cmp     r0, ip
        bne     2f
        movw    ip, #0x0000      /* MR80X_HANDOFF_TRIGGER_BASE & 0xffff */
        movt    ip, #0x0A00      /* MR80X_HANDOFF_TRIGGER_BASE >> 16 */
        str     r2, [ip]
        b       3f

2:      /* Dispatch by calling convention - see the file header comment. */
        cmp     r0, #1
        beq     4f

3:      /* armv8 convention default: return -EOPNOTSUPP (-95) *already
         * remapped*, since __scm_call_64() passes it straight through. */
        mvn     r0, #94
        movs    pc, lr

4:      /* legacy convention default: return raw SCM_EOPNOTSUPP (-4) -
         * __scm_call() remaps this itself via scm_remap_error(). */
        mvn     r0, #3
        movs    pc, lr
