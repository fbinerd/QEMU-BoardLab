/*
 * QCA IPQ5018 / Mercusys MR80X v5 research machine model
 *
 * Deliberately minimal and incomplete: enough to boot the REAL appsbl
 * (u-boot 2016.01) ELF from the full-flash NAND image (or, as a development
 * override, from the sibling `appsbl` clean-room project's -kernel file) far
 * enough to get an interactive UART console and, eventually, working
 * Ethernet and NAND - not a faithful IPQ5018 SoC model. No PCI/USB/BT/
 * switch chip. The CPU starts execution directly at appsbl's entry point
 * (0x4A920000) rather than executing the real, proprietary, undocumented
 * sbl1/qsee stages that run before it on real hardware - see
 * BRINGUP-NOTES.md in the parent directory for the full reasoning, the
 * exact register maps this file is built from, and the real 16-partition
 * flash layout this backs the NAND model with.
 *
 * Every address/offset below is transcribed from the appsbl project's own
 * vendor GPL source (board/qca/arm/ipq5018/, drivers/serial/qca_uart.c,
 * arch/arm/include/asm/arch-qca-common/uart.h) or from disassembling its
 * own build output - not guessed. Re-derive from there if anything here
 * turns out wrong.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "sysemu/sysemu.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "exec/tb-flush.h"
#include "chardev/char-fe.h"
#include "cpu.h"
#include "qom/object.h"
#include "elf.h"
#include "net/net.h"
#include "hw/qdev-properties.h"
#include "hw/intc/arm_gic.h"
#include "target/arm/gtimer.h"

/* ---- memory map (appsbl/CLEAN_ROOM_STATUS.md, ipq5018.h) ---- */

#define MR80X_RAM_BASE      0x40000000
#define MR80X_RAM_SIZE      (512 * MiB)
#define MR80X_APPSBL_ENTRY  0x4A920000
#define MR80X_APPSBL_FLASH_OFFSET 0x00380000
#define MR80X_APPSBL_FLASH_SIZE   0x00140000

#define MR80X_UART_BASE     0x078AF000
#define MR80X_UART_SIZE     0x1000

#define MR80X_GCC_BASE      0x01800000
/* Sized to cover every GCC_* register seen in ipq5018.h from
 * 0x01800000 up through the PCIe clock block at 0x01876050 (GEPHY
 * clock/reset at 0x01856000+, the whole GMAC clock block at
 * 0x01868000+, SDCC1 at 0x01842004, QPIC_IO_MACRO at 0x01857010, USB
 * at 0x0183E0xx) - all plain read-modify-write or CMD_RCGR-poll
 * registers, no special per-register modeling needed beyond the
 * generic latch-and-readback + always-clear-bit0 behavior below. */
#define MR80X_GCC_SIZE      (1 * MiB)

/* ---- SMEM (arch/arm/cpu/armv7/qca/common/smem.c) ----
 * Real hardware has SBL populate this before appsbl ever runs; since we
 * don't execute sbl1 (see BRINGUP-NOTES.md section 7b), fdtdec_setup()'s
 * call to smem_get_board_platform_type() would otherwise read zeroed RAM
 * and hang() (confirmed empirically: first boot attempt hit exactly this
 * hang, at the fdtdec_setup->parse_combined_fdt->hang() call chain).
 * CONFIG_SMEM_VERSION_C is NOT set for this board (checked
 * build/u-boot-2016/.config), so smem_read_alloc_entry() uses the older,
 * simple `struct smem { proc_comm[4]; version_info[32]; heap_info;
 * alloc_info[SMEM_MAX_SIZE]; }` layout directly at CONFIG_QCA_SMEM_BASE,
 * not the newer partition-table format - only one alloc_info entry
 * needs to be valid: SMEM_MACHID_INFO_LOCATION (=425 in
 * board/qca/arm/ipq5018/ipq5018.h's smem_mem_type_t), pointing at an
 * 8-byte {format,machid} struct smem_machid_info (format is never
 * validated by smem_get_board_platform_type(), only machid is used). */
#define MR80X_SMEM_BASE          0x4AB00000
#define MR80X_SMEM_ALLOC_INFO_OFF (4 * 16 + 32 * 4 + 4 * 4) /* 0x1B60 */
#define MR80X_SMEM_MACHID_TYPE    425
#define MR80X_SMEM_MACHID_DATA_OFF 0x4000 /* clear of alloc_info[506] end */
/* Targets ipq5018-emulation.dts's machid - Qualcomm's own reduced
 * bring-up profile, deliberately chosen over hunting down MR80X v5's
 * real machid among ~18 near-identical board DTS files (BRINGUP-NOTES.md
 * section 7). */
#define MR80X_TARGET_MACHID       0x0F040000

/* SMEM_BOOT_FLASH_TYPE and friends (board/qca/arm/ipq5018/ipq5018.h's
 * smem_mem_type_t) - board_f.c's enable_caches() only calls
 * dcache_enable() (which sets up the MMU with an identity-mapped page
 * table - a VMSA/ARMv7-A prerequisite for treating RAM as Normal
 * rather than Device memory) when smem_get_boot_flash()'s flash_type
 * comes back nonzero; the vendor's own comment there says "Skips
 * dcache_enable during JTAG recovery" - i.e. this IS the real,
 * intentional degraded-boot path for "no valid flash info available",
 * which is exactly our situation without faking it. Without the MMU,
 * QEMU's VMSA translation-disabled model architecturally treats all
 * memory as Device type, which enforces alignment unconditionally
 * regardless of SCTLR.A - this is *why* an unaligned STRH inside
 * lib/uip/dns.c's dns_init() (a struct uip_udp_conn field landing on
 * an odd address - sizeof(struct uip_udp_conn)==9, so every other
 * array slot is unaligned) took a genuine alignment fault. Confirmed
 * this isn't a QEMU CPU-model quirk (tried both cortex-a15 and
 * cortex-a7, identical fault) and is architecturally correct behavior
 * per target/arm/tcg/hflags.c's aprofile_require_alignment() - the fix
 * is getting appsbl to enable its own MMU the same way it would on
 * real hardware (which always has valid SMEM flash info), not working
 * around the alignment check. */
#define MR80X_SMEM_FLASH_TYPE_TYPE         498
#define MR80X_SMEM_FLASH_INDEX_TYPE        499
#define MR80X_SMEM_FLASH_CHIP_SELECT_TYPE  500
#define MR80X_SMEM_FLASH_BLOCK_SIZE_TYPE   501
#define MR80X_SMEM_FLASH_DENSITY_TYPE      502
#define MR80X_SMEM_BOOT_NAND_FLASH         2
#define MR80X_SMEM_FLASH_DATA_OFF 0x4100 /* clear of the machid data slot */

/* SMEM_HW_SW_BUILD_ID (=137) - smem_read_platform_type()
 * (arch/arm/cpu/armv7/qca/common/smem.c) reads this into a `union
 * qca_platform`, trying sizeof(qca_platform_v1)=72 bytes first (an
 * exact match against smem_read_alloc_entry()'s `size` check, which
 * requires the alloc_info entry's declared size to equal the
 * requested read length exactly, 8-byte-aligned - 72 already is).
 * Without this fake, ipq_smem_get_socinfo_version()/_cpu_type() print
 * "smem: Get socinfo - version/cpu type failed" (real hardware's SBL
 * always populates this; we skip SBL entirely, section 7b). All
 * fields zeroed (matches the rest of the SMEM region, already
 * zero-filled by mr80x_populate_ram()'s memset) - both call sites
 * only check the return status, not specific field values, so plain
 * zeros are enough to make them succeed without inventing plausible-
 * looking-but-fake chip identification data. */
#define MR80X_SMEM_HW_SW_BUILD_ID_TYPE     137
#define MR80X_SMEM_SOCINFO_DATA_OFF        0x4600
#define MR80X_SMEM_SOCINFO_SIZE            72

/* SMEM_AARM_PARTITION_TABLE (=9) - a nonzero flash_type routes
 * board_init() into the `default:` switch case (see board_init.c),
 * which calls smem_ptable_init() and treats *that* failing as fatal
 * ("cdp: SMEM init failed", aborts the whole initcall sequence) -
 * unlike the flash_type read itself, which fails softly. struct
 * smem_ptable { u32 magic[2]; u32 version; u32 len; struct smem_ptn
 * parts[32]; } (arch/arm/cpu/armv7/qca/common/smem.c) - only the
 * magic needs to be right and len can stay 0 (no partitions - nothing
 * downstream needs to actually resolve one by name for this board's
 * boot to keep going, only for smem_ptable_init() to return success). */
#define MR80X_SMEM_PTABLE_TYPE      9
#define MR80X_SMEM_PTABLE_DATA_OFF  0x4200
#define MR80X_SMEM_PTABLE_SIZE      912
#define MR80X_SMEM_PTABLE_MAGIC_1   0x55ee73aa
#define MR80X_SMEM_PTABLE_MAGIC_2   0xe35ebddb

/* GCC BLSP1 UART1 clock registers (ipq5018.h) - offsets are absolute
 * addresses in the vendor header; store relative to MR80X_GCC_BASE. */
#define GCC_BLSP1_UART1_APPS_CBCR      (0x0180203C - MR80X_GCC_BASE)
#define GCC_BLSP1_UART1_APPS_CMD_RCGR  (0x01802044 - MR80X_GCC_BASE)
#define GCC_BLSP1_UART1_APPS_CFG_RCGR  (0x01802048 - MR80X_GCC_BASE)
#define GCC_BLSP1_UART1_APPS_M         (0x0180204C - MR80X_GCC_BASE)
#define GCC_BLSP1_UART1_APPS_N         (0x01802050 - MR80X_GCC_BASE)
#define GCC_BLSP1_UART1_APPS_D         (0x01802054 - MR80X_GCC_BASE)
#define UART1_CMD_RCGR_UPDATE_BIT      0x1

/* ---- MSM UART DM register offsets from base, PERIPH_BLK_BLSP unset for
 * this board's build (confirmed: not in build/u-boot-2016/.config or
 * generated autoconf.h, so the #else branch of every #if PERIPH_BLK_BLSP
 * in uart.h applies). CSR and SR alias to the same offset (+0x08) in
 * this variant - CSR (baud select) is write-only and never read back by
 * the driver, so treating +0x08 as read-only status (SR) and ignoring
 * writes to it is exactly what real hardware does too. ---- */

#define UART_MR1    0x00
#define UART_MR2    0x04
#define UART_SR     0x08   /* read: status bits; write (as "CSR"): no-op */
#define UART_CR     0x10
#define UART_MISR   0x10   /* alias - see the RX-detection comment below;
                             * *not* actually unused, despite what this
                             * comment used to claim. */
#define UART_IMR    0x14
#define UART_ISR    0x14   /* alias, see UART_IMR */
#define UART_IPR    0x18
#define UART_TFWR   0x1C
#define UART_RFWR   0x20
#define UART_HCR    0x24
#define UART_DMRX   0x34
#define UART_IRDA   0x38
#define UART_RX_TOTAL_SNAP 0x38 /* alias, see the RX-detection comment below */
#define UART_DMEN   0x3C
#define UART_NCHAR  0x40
#define UART_TF0    0x70
#define UART_BADR   0x44

#define UART_SR_RXRDY  (1 << 0)
#define UART_SR_TXRDY  (1 << 2)
#define UART_SR_TXEMT  (1 << 3)
#define UART_MISR_RXSTALE (1 << 3)

/* qca_uart.c's ipq_serial_pending() (what tstc()/getc() actually call,
 * not a simple UART_SR/RXRDY check as originally assumed here) goes
 * through msm_boot_uart_dm_read(): poll UART_MISR for RXSTALE, then
 * read UART_RX_TOTAL_SNAP once per transfer for the real byte count,
 * then read the RX FIFO word (UART_TF0/RF, same offset in this
 * variant) - a 32-bit word can carry up to 4 packed bytes, with a
 * hardware-quirk workaround treating an all-zero word as "not ready"
 * regardless of what UART_RX_TOTAL_SNAP said. Getting *this* path
 * right matters a lot more than UART_SR/RXRDY (which nothing in this
 * driver's RX path actually reads) - confirmed by direct testing:
 * pre-seeding this emulator's RX buffer to guarantee an autoboot
 * abort (MR80X_STOP_AUTOBOOT) silently did nothing until this was
 * fixed, because tstc() was polling MISR/RXSTALE the whole time, a
 * register this file never modeled a real answer for. */

/* ============================================================
 * Catch-all logging stub for anything not modeled yet. Reads always
 * return 0; this is deliberately wrong for registers that gate a
 * poll-until-bit-set loop (those will need a real, if minimal, model
 * as boot progresses further - the log output says exactly which
 * region/offset needs one next).
 * ============================================================ */

static uint64_t mr80x_unimp_read(void *opaque, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "mr80x: unimplemented READ  region=%s off=0x%" HWADDR_PRIx
                  " size=%u\n", (const char *)opaque, offset, size);
    return 0;
}

static void mr80x_unimp_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "mr80x: unimplemented WRITE region=%s off=0x%" HWADDR_PRIx
                  " size=%u val=0x%" PRIx64 "\n",
                  (const char *)opaque, offset, size, value);
}

static const MemoryRegionOps mr80x_unimp_ops = {
    .read = mr80x_unimp_read,
    .write = mr80x_unimp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void mr80x_add_unimp_region(MemoryRegion *sysmem, const char *name,
                                    hwaddr base, hwaddr size)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &mr80x_unimp_ops, (void *)name, name,
                           size);
    memory_region_add_subregion(sysmem, base, mr);
}

/* ============================================================
 * TLMM/GPIO: reads always return all-ones (every input pin idles
 * "high"), writes are silently accepted. board/qca/arm/common/
 * cmd_bootqca.c's check_fw_gpio() reads GPIO14 (the reset button) and
 * treats it *active low* (`return !(val & 0x1)`) - reading back 0 (the
 * generic catch-all stub's default) reads as "button held down",
 * auto-triggering firmware recovery mode on every single boot
 * regardless of what was actually requested. All-ones reads as
 * "button not pressed", the correct idle state for a real device that
 * nobody is touching, so appsbl takes its normal boot path instead -
 * see BRINGUP-NOTES.md section 12. Pin-mux/config writes elsewhere in
 * this same region don't need to be readable-back for anything
 * observed so far, so one blanket policy for the whole block is
 * enough; split this into per-register handling only if something
 * else in this address range turns out to need a real value read
 * back.
 *
 * GPIO_IN_OUT_ADDR(14) == TLMM_BASE + 0x4 + 14*0x1000 == offset
 * 0xE004 within this region - the one exception to the blanket
 * all-ones policy: when the MR80X_RECOVERY environment variable is
 * set (checked once in mr80x_init(), stored in *opaque as a bool),
 * this single register instead reads back with bit0 clear, which
 * check_fw_gpio() reads as "button held down" - i.e. deliberately,
 * explicitly entering firmware recovery mode the same way holding
 * the real reset button does, without forcing it on every boot by
 * default (that was the original bug this region fixed). */
#define MR80X_TLMM_GPIO14_OFF 0xE004

static uint64_t mr80x_tlmm_read(void *opaque, hwaddr offset, unsigned size)
{
    bool *recovery = opaque;

    if (offset == MR80X_TLMM_GPIO14_OFF && recovery && *recovery) {
        return 0x00000000u;
    }
    qemu_log_mask(LOG_UNIMP,
                  "mr80x: tlmm READ  off=0x%" HWADDR_PRIx " size=%u -> ~0\n",
                  offset, size);
    return 0xFFFFFFFFu;
}

static const MemoryRegionOps mr80x_tlmm_ops = {
    .read = mr80x_tlmm_read,
    .write = mr80x_unimp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* ============================================================
 * GCC clock controller: only what uart1_clock_config() touches.
 * Every register just latches whatever is written; CMD_RCGR additionally
 * always reads back with UART1_CMD_RCGR_UPDATE_BIT already clear, so
 * uart1_trigger_update()'s poll loop exits immediately - on real
 * hardware this bit is cleared by the clock hardware once the mux
 * switch completes; we have no clock tree to simulate, so "instantly
 * done" is the correct-enough behavior here.
 * ============================================================ */

typedef struct MR80XGccState {
    MemoryRegion iomem;
    uint32_t regs[MR80X_GCC_SIZE / 4];
} MR80XGccState;

static uint64_t mr80x_gcc_read(void *opaque, hwaddr offset, unsigned size)
{
    MR80XGccState *s = opaque;
    uint32_t val = s->regs[offset / 4];

    /* Bit 0 (UPDATE) is the busy/in-progress flag every *_CMD_RCGR
     * register in this whole GCC block uses (ipq5018.h has ~18 of
     * them - UART, GMAC x4, SDCC1, QPIC_IO_MACRO, USB x4, PCIe x4 -
     * same convention throughout, not worth enumerating each one by
     * address). Always reporting it clear means every driver's
     * "trigger update, wait for hardware to finish" loop (e.g.
     * uart1_trigger_update(), the GMAC clock equivalent) exits on its
     * first read instead of spinning - fine since we have no real
     * clock tree to actually finish switching. Registers that aren't
     * a CMD_RCGR don't have driver code that depends on their bit 0
     * meaning "busy", so clearing it unconditionally is harmless for
     * them too. */
    val &= ~UART1_CMD_RCGR_UPDATE_BIT;
    return val;
}

static void mr80x_gcc_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    MR80XGccState *s = opaque;
    s->regs[offset / 4] = (uint32_t)value;
}

static const MemoryRegionOps mr80x_gcc_ops = {
    .read = mr80x_gcc_read,
    .write = mr80x_gcc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ============================================================
 * Generic timer counter-view MMIO registers (gcnt_cntcv_lo/hi from the
 * /timer DT node in ipq5018-soc.dtsi - 0x4A2000/0x4A2004), read by
 * arch/arm/cpu/armv7/qca/common/timer.c's read_counter(), which
 * __udelay() *and* get_timer()/CONFIG_BOOTDELAY's autoboot countdown
 * both spin on (get_timer() divides raw ticks by
 * GPT_FREQ_HZ/CONFIG_SYS_HZ - GPT_FREQ_HZ comes from the "gpt_freq_hz"
 * DT property, 240000 in every board DTS checked, including the one
 * baked into this exact appsbl binary).
 *
 * Originally modeled as a free-running counter that jumps forward by
 * a large fixed step on every read, specifically so tight hardware
 * busy-wait loops (polling a clock-control busy bit, a NAND status
 * register, microsecond-scale udelay()s) would resolve in a handful
 * of TCG-time instructions instead of real wall-clock microseconds -
 * fine for those, but it also meant get_timer()-based *human-scale*
 * waits (the several-second "Hit any key to stop autoboot" countdown
 * being the one a user actually notices) always appeared to have
 * already elapsed by the time the guest read it, even on the very
 * first read - the countdown printed "0" immediately, with no real
 * window to actually press a key interactively.
 *
 * Now driven by QEMU's own virtual clock (real elapsed wall-clock
 * time since the guest started, scaled to GPT_FREQ_HZ) instead of an
 * artificial step. This makes get_timer()-based delays take
 * genuinely real time - correct for a console someone is meant to
 * interact with - while still not requiring any change on the
 * *short* hardware-poll side: those loops just do correspondingly
 * more (cheap, TCG-fast) re-reads over the same real microseconds a
 * real chip would also need, not a change in outcome, just no longer
 * artificially compressed to "already done" the first time either.
 * ============================================================ */

#define MR80X_TIMER_BASE 0x4A2000
#define MR80X_TIMER_SIZE 0x8
#define MR80X_TIMER_FREQ_HZ 240000
/* Real hardware's CONFIG_BOOTDELAY=1 (this is a CONFIG_TP_IMAGE
 * build) means a genuinely tight 1-second window to press a key
 * before autoboot proceeds - on real hardware, connected via a real
 * TTL adapter, that's already not much time; over docker's stdin
 * (host terminal -> docker engine API -> container -> QEMU) there's
 * extra unavoidable latency in that path on top of ordinary human
 * reaction time, and 1 second in like this reliably isn't enough -
 * confirmed by direct user testing (couldn't interrupt autoboot even
 * after the BQL-blocking g_usleep() fix, which addressed a different,
 * real problem but not this one). Slow the timer down by
 * MR80X_TIME_SCALE (an env var, default below) so u-boot's own
 * "1 second" still means genuinely one second to *it*
 * (get_timer()/CONFIG_SYS_HZ math is untouched - only how fast real
 * wall-clock time maps to ticks changes), while the human actually
 * gets several real seconds of window. Everything else timer-gated
 * (hardware busy-polls, other delays) is proportionally slower too,
 * but those are micro/millisecond-scale to begin with, so the
 * absolute real-time cost stays negligible. */
#define MR80X_TIME_SCALE_DEFAULT 6

typedef struct MR80XTimerState {
    MemoryRegion iomem;
    uint32_t scale;
} MR80XTimerState;

static uint64_t mr80x_timer_counter(MR80XTimerState *s)
{
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return muldiv64(now_ns, MR80X_TIMER_FREQ_HZ,
                     (int64_t)NANOSECONDS_PER_SECOND * s->scale);
}

static uint64_t mr80x_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    MR80XTimerState *s = opaque;
    uint64_t counter = mr80x_timer_counter(s);

    if (offset == 0x0) {
        return (uint32_t)counter;
    } else if (offset == 0x4) {
        return (uint32_t)(counter >> 32);
    }
    return 0;
}

static void mr80x_timer_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    /* real hardware: read-only counter view; ignore writes */
}

static const MemoryRegionOps mr80x_timer_ops = {
    .read = mr80x_timer_read,
    .write = mr80x_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ============================================================
 * GCNT_PSHOLD (arch-qca-common/iomap.h) - reset_cpu()'s fallback path
 * (qti_scm_pshold(), board/qca/arm/ipq5018/ipq5018.c) writes 0 here
 * when the SCM/TrustZone call it tries first fails - which it always
 * will here, since we don't execute qsee (BRINGUP-NOTES.md section
 * 7b). On real hardware this write releases the PMIC's power-hold
 * line, causing an actual power-cycle. Without modeling it, whatever
 * called reset_cpu() (e.g. a crash handler, "Resetting CPU ...") just
 * falls through into `while(1);` with nothing having actually reset -
 * confirmed empirically: a real crash-recovery reset attempt produced
 * repeated "prefetch abort"/"Resetting CPU .../resetting ..." message
 * pairs with a *slightly different* LR each time instead of one clean
 * restart, i.e. execution kept limping forward through corrupted state
 * rather than actually restarting. Triggering a real QEMU system reset
 * here - which re-invokes mr80x_reset() the same way the very first
 * boot did - makes a guest-requested reset behave the same as real
 * hardware's power-cycle: RAM content (already-loaded appsbl code,
 * and this file's earlier one-time SMEM fakes) is untouched, only CPU
 * state resets to a fresh start at the entry point.
 * ============================================================ */

#define MR80X_PSHOLD_BASE 0x004AB000
#define MR80X_PSHOLD_SIZE 0x4

static uint64_t mr80x_pshold_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void mr80x_pshold_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "mr80x: GCNT_PSHOLD write val=0x%" PRIx64
                  " - requesting a real system reset\n", value);
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static const MemoryRegionOps mr80x_pshold_ops = {
    .read = mr80x_pshold_read,
    .write = mr80x_pshold_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ============================================================
 * QPIC NAND controller (drivers/mtd/nand/qpic_nand.c,
 * arch-qca-common/qpic_nand.h) - base QPIC_EBI2ND_BASE = 0x079B0000.
 * board_nand_init() gates its *entire* probe on one register:
 *
 *   hw_ver = readl(NAND_VERSION) >> 28;
 *   if (hw_ver >= QCA_QPIC_V2_1_1)  // enum: V1_4_20=0, V1_5_20=1, V2_1_1=2
 *       printf("QPIC controller support serial NAND\n");
 *   else {
 *       printf("... Qpic controller not support serial NAND\n");
 *       return;  // bails BEFORE qpic_nand_reset()/qpic_nand_onfi_probe()
 *   }
 *
 * NAND_VERSION is at NAND_REG(0x4F08) i.e. base+0x4F08. The generic
 * catch-all stub's default-zero read means hw_ver was always 0,
 * failing this check on every boot - which the recovery-mode boot
 * path handles gracefully (no NAND needed to serve HTTP) but the
 * *normal* boot path does not: it still tries to load a kernel from
 * NAND further down, hitting an uninitialized device/function table -
 * see BRINGUP-NOTES.md section 12's prefetch-abort-at-pc=0xc writeup.
 * Reporting hw_ver=2 here lets the real probe logic run instead of
 * bailing immediately - what that logic then needs next (ID read,
 * ONFI probe, BAM-based page read for the real env/kernel data in
 * FULL_FIRMWARE.bin) is deliberately being discovered empirically,
 * the same way every other peripheral in this file was, rather than
 * guessed up front.
 * ============================================================ */

#define MR80X_NAND_BASE 0x079B0000
#define MR80X_NAND_SIZE 0x10000
#define NAND_VERSION_OFF 0x4F08

/* Register offsets from arch-qca-common/qpic_nand.h - the small subset
 * qpic_nand_fetch_id()/qpic_nand_read_reg() actually touch. */
#define NAND_FLASH_CMD_OFF        0x0000
#define NAND_ADDR0_OFF            0x0004
#define NAND_ADDR1_OFF            0x0008
#define NAND_EXEC_CMD_OFF         0x0010
#define NAND_FLASH_STATUS_OFF     0x0014
#define NAND_READ_ID_OFF          0x0040
#define NAND_DEV_CMD_VLD_V1_5_20_OFF 0x70AC

#define NAND_CMD_FETCH_ID 0x0B
#define MR80X_NAND_PAGE_SIZE 2048

/* Fake serial NAND identity: GigaDevice GD5F1GQ4RE9IG (id bytes
 * {0xc8,0xc1} in qpic_serial_nand_tbl[]) - chosen because its
 * page_size=2048/erase_blk_size=0x20000/density=0x08000000 (128MiB)
 * exactly match the SMEM flash-type block_size/density fakes already
 * set up in mr80x_init(). NAND_READ_ID's low two bytes are
 * {vendor,device} = {id&0xff, (id>>8)&0xff}, so id=0x0000c1c8 yields
 * vendor=0xc8, device=0xc1. */
#define MR80X_FAKE_NAND_ID 0x0000c1c8u

typedef struct MR80XNandState {
    uint32_t regs[MR80X_NAND_SIZE / 4];
    /* Real flash *data* backing (see MR80X_BAM_DATA_PRODUCER_PIPE
     * handling below) - a read-only mmap of a real full-flash dump,
     * MR80X_NAND_IMAGE env var. NULL if not provided: page reads then
     * fall back to returning 0xFF (erased-flash convention) instead
     * of real data, same as before this existed. */
    const uint8_t *image_data;
    size_t image_size;
    /* Running byte offset in the current read stream.  The low 16 bits of
     * NAND_ADDR0 select the initial column.  Page reads then stream each
     * 2048-byte main area plus the 16 OOB bytes this APPSBL asks for before
     * advancing to the next page in multi-page reads. */
    uint32_t page_read_offset;
} MR80XNandState;

static void mr80x_nand_reset(void *opaque)
{
    MR80XNandState *s = opaque;

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[NAND_VERSION_OFF / 4] = 0x20000000u;
    s->page_read_offset = 0;
    /* image_data/image_size describe the persistent flash backing and must
     * survive a SoC reset, just like the contents of physical NAND. */
}

static uint32_t mr80x_nand_reg_read(MR80XNandState *s, hwaddr offset)
{
    return s->regs[offset / 4];
}

/* Shared by both direct-MMIO writes and the BAM cmd-pipe engine below -
 * on real hardware both paths ultimately land on the same NANDc
 * register file. Writing NAND_EXEC_CMD (the "go" trigger) is where we
 * synthesize a result for whatever opcode was latched into
 * NAND_FLASH_CMD, mirroring what the real controller would have done
 * against actual flash. */
static void mr80x_nand_reg_write(MR80XNandState *s, hwaddr offset,
                                  uint32_t value, uint32_t mask)
{
    s->regs[offset / 4] = (s->regs[offset / 4] & ~mask) | (value & mask);

    if (offset == NAND_ADDR0_OFF) {
        s->page_read_offset = value & 0xffff;
    }

    if (offset == NAND_EXEC_CMD_OFF && (value & mask & 0x1)) {
        uint32_t cmd = s->regs[NAND_FLASH_CMD_OFF / 4] & 0xFF;

        s->regs[NAND_FLASH_STATUS_OFF / 4] = 0; /* no NAND_FLASH_ERR bits */
        if (cmd == NAND_CMD_FETCH_ID) {
            s->regs[NAND_READ_ID_OFF / 4] = MR80X_FAKE_NAND_ID;
        }
    }
}

static uint64_t mr80x_nand_read(void *opaque, hwaddr offset, unsigned size)
{
    MR80XNandState *s = opaque;
    return mr80x_nand_reg_read(s, offset);
}

static void mr80x_nand_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MR80XNandState *s = opaque;
    mr80x_nand_reg_write(s, offset, (uint32_t)value, 0xFFFFFFFFu);
}

static const MemoryRegionOps mr80x_nand_ops = {
    .read = mr80x_nand_read,
    .write = mr80x_nand_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* ============================================================
 * QPIC BAM (Bus Access Manager) - drivers/dma/bam.c,
 * arch-qca-common/bam.h. Base QPIC_BAM_CTRL_BASE = 0x07984000.
 *
 * Real hardware: a DMA engine that walks descriptor rings in guest
 * RAM. qpic_nand.c's register accesses (qpic_nand_fetch_id(),
 * qpic_nand_read_reg()) don't touch NAND_* registers directly at all
 * - they build an array of `struct cmd_element` (16 bytes:
 * addr_n_cmd, reg_data, reg_mask, reserved) in RAM, wrap it in one
 * `struct bam_desc` (8 bytes: addr, size, reserved, flags) written
 * into the "cmd pipe" (pipe_num=CMD_PIPE=2)'s descriptor FIFO, then
 * kick the BAM by writing the new FIFO write-offset to
 * BAM_P_EVNT_REGn(2, ee). The actual NANDc register read/write only
 * happens once the BAM processes that descriptor.
 *
 * Modeled behavior: process synchronously on the EVNT_REGn kick write
 * - read back the just-added bam_desc, and when BAM_DESC_CMD_FLAG is
 * set, interpret its buffer as concatenated cmd_elements and apply
 * each directly against MR80XNandState's register file (the very
 * struct mr80x_nand_read/write above also use, so either access path
 * observes the same state):
 *   - CE_WRITE_TYPE (cmd_type=0): reg_data & reg_mask -> register.
 *   - CE_READ_TYPE  (cmd_type=1): register's value is written OUT to
 *     the RAM address held in reg_data - see bam_add_cmd_element()'s
 *     dcache-flush-of-`value` comment: for a read CE, the value field
 *     is a destination pointer, not data.
 * Addressing quirk: addr_n_cmd keeps only the low 24 bits of the real
 * register address (`reg_addr & ~BAM_CE_REG_ADDR_MASK`, top byte
 * repurposed for cmd_type). Every QPIC NAND register lives at
 * 0x079Bxxxx, so the discarded top byte is always reconstructable as
 * 0x07.
 * bam_wait_for_interrupt()'s poll loop (BAM_IRQ_SRCS then
 * BAM_P_IRQ_STTSn, looking for P_PRCSD_DESC_EN_MASK=1) succeeds
 * immediately since we set both synchronously inside the kick write.
 * BAM_P_SW_OFSTSn's value is read by bam_read_offset_update() but
 * assigned to a local variable that's never used - safe to return 0.
 *
 * The data-producer pipe (index/pipe_num 1) and status pipe (index 3)
 * carry real page *data* for qpic_nand_page_scope_read() - see
 * mr80x_bam_process_raw_desc() below: each codeword's data descriptor
 * copies real bytes from MR80X_NAND_IMAGE (if provided) at the
 * current page's file offset, and each status descriptor reports a
 * clean (no-error) 12-byte auto-status record, matching
 * qpic_nand_check_read_status()'s success path. The data-consumer
 * pipe (index 0, real flash *writes*) isn't driven yet - see
 * BRINGUP-NOTES.md.
 * ============================================================ */

#define MR80X_BAM_BASE 0x07984000
#define MR80X_BAM_SIZE 0x20000
#define MR80X_BAM_NUM_PIPES 4
#define MR80X_BAM_DATA_CONSUMER_PIPE 0  /* writes (guest -> flash), not driven yet */
#define MR80X_BAM_DATA_PRODUCER_PIPE 1  /* reads (flash -> guest) */
#define MR80X_BAM_CMD_PIPE 2
#define MR80X_BAM_STATUS_PIPE 3         /* per-codeword auto-status, page-scope reads */
#define MR80X_BAM_EE 0

#define MR80X_NAND_OOB_STREAM_SIZE 16
#define MR80X_NAND_READ_STREAM_STRIDE \
    (MR80X_NAND_PAGE_SIZE + MR80X_NAND_OOB_STREAM_SIZE)

#define BAM_P_CTRLn_BASE          0x00013000
#define BAM_P_RSTn_BASE           0x00013004
#define BAM_P_IRQ_STTSn_BASE      0x00013010
#define BAM_P_IRQ_CLRn_BASE       0x00013014
#define BAM_P_IRQ_ENn_BASE        0x00013018
#define BAM_P_SW_OFSTSn_BASE      0x00013800
#define BAM_P_EVNT_REGn_BASE      0x00013818
#define BAM_P_DESC_FIFO_ADDRn_BASE 0x0001381C
#define BAM_P_FIFO_SIZESn_BASE   0x00013820
#define BAM_IRQ_SRCS_BASE         0x00003000
#define BAM_DESC_CMD_FLAG (1 << 3)
#define BAM_P_PRCSD_DESC_MASK 1

typedef struct MR80XBamPipe {
    hwaddr fifo_base;
    uint32_t irq_stts;
    /* Latest offset advertised via P_EVNT_REG.  Raw producer/status
     * descriptors may wait here for the matching command-pipe kick. */
    uint32_t notified_evnt_off;
    uint32_t last_evnt_off;
    uint32_t fifo_size; /* bytes, from BAM_P_FIFO_SIZESn - real hardware
                          * masks the event/offset register modulo this,
                          * NOT a fixed 16-bit wrap (bam_sys_gen_event()'s
                          * `val &= fifo.size*BAM_DESC_SIZE - 1`). Getting
                          * this wrong silently corrupts which guest
                          * memory a later kick's descriptor is read
                          * from once enough kicks accumulate past a
                          * small pipe's real (small) FIFO. */
} MR80XBamPipe;

typedef struct MR80XBamState {
    MemoryRegion iomem;
    MR80XNandState *nand;
    MR80XBamPipe pipe[MR80X_BAM_NUM_PIPES];
    uint32_t generic_regs[MR80X_BAM_SIZE / 4];
    /* Only meaningful to the *kernel*'s interrupt-driven bam-dma-engine
     * driver (drivers/dma/qcom/bam_dma.c) - appsbl's own hand-rolled
     * QPIC/BAM driver polls BAM_P_IRQ_STTS directly and never enables
     * or waits on this at all, so it stayed unwired for this whole
     * project until the kernel's driver actually needed it (this
     * emulator had no interrupt controller at all before section 26).
     * Pulsed (not level-held) whenever any pipe's irq_stts becomes set
     * from descriptor processing below - the kernel's ISR reads/clears
     * the real status registers itself once woken, so a real level
     * hold isn't needed for this driver's completion-notification use;
     * this DT's SPI 0x92 has irq flags=0 (not the UART's 4=level-high),
     * consistent with an edge-style notification. */
    qemu_irq irq;
} MR80XBamState;

/* BAM_REVISION/BAM_NUM_PIPES (register layout for "qcom,bam-v1.7.0",
 * matching this exact DT node - drivers/dma/qcom/bam_dma.c's
 * bam_v1_7_reg_info table) - not needed by appsbl's own hand-rolled
 * QPIC/BAM driver (which never reads either), but the *kernel*'s
 * generic bam-dma-engine driver's probe()/bam_init() unconditionally
 * does, computing num_ees from BAM_REVISION bits [11:8] and
 * requiring the DT's "qcom,ee" value (0 for this SoC) be strictly
 * less than it - with these left at their all-zero reset default
 * (falling through to the generic unimplemented-register handling
 * every other unmodeled BAM offset gets), num_ees reads as 0, so
 * `bdev->ee (0) >= num_ees (0)` was unconditionally true and the
 * whole driver failed to probe with -EINVAL, which cascaded into
 * qcom-nandc never getting a DMA channel and the kernel never being
 * able to mount its UBI rootfs at all (BRINGUP-NOTES.md section 28). */
#define BAM_REVISION_OFF 0x01000
#define BAM_NUM_PIPES_OFF 0x01008

static void mr80x_bam_reset(void *opaque)
{
    MR80XBamState *s = opaque;

    memset(s->pipe, 0, sizeof(s->pipe));
    memset(s->generic_regs, 0, sizeof(s->generic_regs));
    s->generic_regs[BAM_REVISION_OFF / 4] = 1u << 8;   /* num_ees = 1 */
    s->generic_regs[BAM_NUM_PIPES_OFF / 4] = MR80X_BAM_NUM_PIPES;
    /* s->nand is the device link, not volatile controller state. */
}

static void mr80x_bam_process_cmd_desc(MR80XBamState *s, hwaddr desc_addr)
{
    uint8_t desc[8];
    uint32_t buf_addr, i;
    uint16_t buf_size;
    uint8_t flags;

    cpu_physical_memory_read(desc_addr, desc, sizeof(desc));
    buf_addr = ldl_le_p(desc + 0);
    buf_size = lduw_le_p(desc + 4);
    flags = desc[7];

    if (!(flags & BAM_DESC_CMD_FLAG)) {
        return; /* data-pipe transfer, not a cmd_element batch */
    }

    for (i = 0; i + 16 <= buf_size; i += 16) {
        uint8_t ce[16];
        uint32_t addr_n_cmd, reg_data;
        hwaddr reg_addr;
        int cmd_type;

        cpu_physical_memory_read(buf_addr + i, ce, sizeof(ce));
        addr_n_cmd = ldl_le_p(ce + 0);
        reg_data = ldl_le_p(ce + 4);
        /* reg_mask at ce+8 is always 0xFFFFFFFF in this driver -
         * not needed for correct behavior here. */

        reg_addr = 0x07000000u | (addr_n_cmd & 0x00FFFFFFu);
        cmd_type = (addr_n_cmd >> 24) & 0xFF;

        if (reg_addr < MR80X_NAND_BASE ||
            reg_addr >= MR80X_NAND_BASE + MR80X_NAND_SIZE) {
            qemu_log_mask(LOG_UNIMP,
                          "mr80x: bam cmd_element targets unmodeled reg "
                          "0x%" HWADDR_PRIx "\n", reg_addr);
            continue;
        }

        if (cmd_type == 1) { /* CE_READ_TYPE: reg_data is a dest pointer */
            uint32_t val = mr80x_nand_reg_read(s->nand,
                                                reg_addr - MR80X_NAND_BASE);
            uint8_t le[4];
            stl_le_p(le, val);
            cpu_physical_memory_write(reg_data, le, 4);
        } else { /* CE_WRITE_TYPE */
            mr80x_nand_reg_write(s->nand, reg_addr - MR80X_NAND_BASE,
                                  reg_data, 0xFFFFFFFFu);
        }
    }
}

/* Data-producer pipe (real page reads) and status pipe (per-codeword
 * auto-status) descriptors are plain {dest, len} buffers, not
 * cmd_element batches - see the comment block above. The *source*
 * side (which bytes of real flash a given descriptor should deliver)
 * is tracked separately in MR80XNandState.page_read_offset.  It starts at
 * NAND_ADDR0's low-16-bit column and advances through the 2048-byte main
 * area plus the 16-byte OOB slot requested by this APPSBL before rolling
 * to the next page in multi-page reads.  The *destination* address is
 * always taken directly from the descriptor itself, since the real
 * driver already computes a distinct, correctly-offset buffer
 * pointer per codeword (qpic_nand.c's `buffer += data_bytes`
 * between iterations, not shown in the cmd/data split above but
 * present in the real loop). Once page_read_offset reaches
 * MR80X_NAND_PAGE_SIZE (2048), any further bytes in this same page
 * read are OOB/spare data (real serial NAND spare-area bytes, not
 * captured by a raw MR80X_NAND_IMAGE dump) - filled with 0xFF
 * ("erased flash") rather than real content, which is fine: nothing
 * that matters for booting (env parsing, kernel/rootfs loading)
 * reads OOB data, only the main 2048 bytes/page. */
static void mr80x_bam_process_raw_desc(MR80XBamState *s, int pipe,
                                        hwaddr desc_addr)
{
    uint8_t desc[8];
    uint32_t dest_addr;
    uint16_t len;
    uint8_t buf[MR80X_NAND_PAGE_SIZE];
    MR80XNandState *nand = s->nand;

    cpu_physical_memory_read(desc_addr, desc, sizeof(desc));
    dest_addr = ldl_le_p(desc + 0);
    len = lduw_le_p(desc + 4);
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }

    if (pipe == MR80X_BAM_STATUS_PIPE) {
        memset(buf, 0, len); /* flash_sts=buffer_sts=erased_cw_sts=0 */
        cpu_physical_memory_write(dest_addr, buf, len);
        return;
    }

    /* MR80X_BAM_DATA_PRODUCER_PIPE */
    {
        uint32_t base_page = (nand->regs[NAND_ADDR0_OFF / 4] >> 16) |
                             (nand->regs[NAND_ADDR1_OFF / 4] << 16);
        uint32_t stream_off = nand->page_read_offset;
        uint32_t done = 0;

        while (done < len) {
            uint32_t page_delta = stream_off / MR80X_NAND_READ_STREAM_STRIDE;
            uint32_t column = stream_off % MR80X_NAND_READ_STREAM_STRIDE;
            uint32_t chunk;

            if (column < MR80X_NAND_PAGE_SIZE) {
                uint32_t main_left = MR80X_NAND_PAGE_SIZE - column;
                uint64_t file_off = (uint64_t)(base_page + page_delta) *
                                    MR80X_NAND_PAGE_SIZE + column;

                chunk = MIN((uint32_t)len - done, main_left);
                if (nand->image_data && file_off + chunk <= nand->image_size) {
                    memcpy(buf + done, nand->image_data + file_off, chunk);
                } else {
                    memset(buf + done, 0xFF, chunk);
                }
            } else {
                uint32_t oob_left = MR80X_NAND_READ_STREAM_STRIDE - column;

                chunk = MIN((uint32_t)len - done, oob_left);
                memset(buf + done, 0xFF, chunk);
            }

            done += chunk;
            stream_off += chunk;
        }

        cpu_physical_memory_write(dest_addr, buf, len);
        nand->page_read_offset = stream_off;
    }
}

static void mr80x_bam_drain_raw_pipe(MR80XBamState *s, uint32_t pipe)
{
    MR80XBamPipe *p = &s->pipe[pipe];
    uint32_t mask, delta, i;

    if (!p->fifo_base || !p->fifo_size) {
        return;
    }

    mask = p->fifo_size - 1;
    delta = (p->notified_evnt_off - p->last_evnt_off) & mask;
    for (i = 0; i < delta; i += 8) {
        hwaddr desc_addr = p->fifo_base +
                           ((p->last_evnt_off + i) & mask);
        mr80x_bam_process_raw_desc(s, pipe, desc_addr);
    }

    if (delta) {
        p->last_evnt_off = p->notified_evnt_off;
        p->irq_stts |= BAM_P_PRCSD_DESC_MASK;
        if (s->irq) {
            qemu_set_irq(s->irq, 1);
            qemu_set_irq(s->irq, 0);
        }
    }
}

static uint64_t mr80x_bam_read(void *opaque, hwaddr offset, unsigned size)
{
    MR80XBamState *s = opaque;

    if (offset >= BAM_IRQ_SRCS_BASE &&
        offset < BAM_IRQ_SRCS_BASE + 0x1000 * MR80X_BAM_NUM_PIPES) {
        uint32_t n = (offset - BAM_IRQ_SRCS_BASE) / 0x1000;
        uint32_t sub = (offset - BAM_IRQ_SRCS_BASE) % 0x1000;
        if (sub == 0 && n == MR80X_BAM_EE) {
            uint32_t srcs = 0, p;
            for (p = 0; p < MR80X_BAM_NUM_PIPES; p++) {
                if (s->pipe[p].irq_stts) {
                    srcs |= (1u << p);
                }
            }
            return srcs;
        }
    }

    if (offset >= BAM_P_IRQ_STTSn_BASE &&
        offset < BAM_P_IRQ_STTSn_BASE + 0x1000 * MR80X_BAM_NUM_PIPES) {
        uint32_t n = (offset - BAM_P_IRQ_STTSn_BASE) / 0x1000;
        if ((offset - BAM_P_IRQ_STTSn_BASE) % 0x1000 == 0) {
            return s->pipe[n].irq_stts;
        }
    }

    return s->generic_regs[offset / 4];
}

static void mr80x_bam_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    MR80XBamState *s = opaque;

    s->generic_regs[offset / 4] = (uint32_t)value;

    if (offset >= BAM_P_DESC_FIFO_ADDRn_BASE &&
        offset < BAM_P_DESC_FIFO_ADDRn_BASE + 0x1000 * MR80X_BAM_NUM_PIPES &&
        (offset - BAM_P_DESC_FIFO_ADDRn_BASE) % 0x1000 == 0) {
        uint32_t n = (offset - BAM_P_DESC_FIFO_ADDRn_BASE) / 0x1000;
        s->pipe[n].fifo_base = value;
        return;
    }

    if (offset >= BAM_P_FIFO_SIZESn_BASE &&
        offset < BAM_P_FIFO_SIZESn_BASE + 0x1000 * MR80X_BAM_NUM_PIPES &&
        (offset - BAM_P_FIFO_SIZESn_BASE) % 0x1000 == 0) {
        uint32_t n = (offset - BAM_P_FIFO_SIZESn_BASE) / 0x1000;
        s->pipe[n].fifo_size = value; /* already in bytes, bam_pipe_fifo_init() */
        return;
    }

    if (offset >= BAM_P_IRQ_CLRn_BASE &&
        offset < BAM_P_IRQ_CLRn_BASE + 0x1000 * MR80X_BAM_NUM_PIPES &&
        (offset - BAM_P_IRQ_CLRn_BASE) % 0x1000 == 0) {
        uint32_t n = (offset - BAM_P_IRQ_CLRn_BASE) / 0x1000;
        s->pipe[n].irq_stts &= ~(uint32_t)value;
        return;
    }

    if (offset >= BAM_P_EVNT_REGn_BASE &&
        offset < BAM_P_EVNT_REGn_BASE + 0x1000 * MR80X_BAM_NUM_PIPES &&
        (offset - BAM_P_EVNT_REGn_BASE) % 0x1000 == 0) {
        uint32_t n = (offset - BAM_P_EVNT_REGn_BASE) / 0x1000;

        /* qpic_nand_page_scope_read() advertises data/status descriptors
         * before the matching command descriptor.  Real BAM lock groups
         * hold those raw pipes until CMD has programmed NAND_ADDR0/1 and
         * the read location.  Processing raw kicks immediately used the
         * previous page address, shifting NAND reads and making a populated
         * UBI image appear empty.  Record raw kicks here; a command kick
         * executes its command elements first and then releases them. */
        if (s->pipe[n].fifo_base && s->pipe[n].fifo_size) {
            uint32_t mask = s->pipe[n].fifo_size - 1; /* fifo_size is pow2 */
            uint32_t new_off = (uint32_t)value;
            s->pipe[n].notified_evnt_off = new_off;

            if (n == MR80X_BAM_CMD_PIPE) {
                uint32_t old_off = s->pipe[n].last_evnt_off;
                uint32_t delta = (new_off - old_off) & mask;
                uint32_t i;

                for (i = 0; i < delta; i += 8) {
                    hwaddr desc_addr = s->pipe[n].fifo_base +
                                        ((old_off + i) & mask);
                    mr80x_bam_process_cmd_desc(s, desc_addr);
                }
                s->pipe[n].last_evnt_off = new_off;
                s->pipe[n].irq_stts |= BAM_P_PRCSD_DESC_MASK;
                if (s->irq) {
                    qemu_set_irq(s->irq, 1);
                    qemu_set_irq(s->irq, 0);
                }

                mr80x_bam_drain_raw_pipe(s,
                                         MR80X_BAM_DATA_PRODUCER_PIPE);
                mr80x_bam_drain_raw_pipe(s, MR80X_BAM_STATUS_PIPE);
            }
        }
        return;
    }
}

static const MemoryRegionOps mr80x_bam_ops = {
    .read = mr80x_bam_read,
    .write = mr80x_bam_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* ============================================================
 * IPQ5018 MDIO controller (drivers/net/ipq5018/ipq5018_mdio.c/.h) -
 * base 0x88000 (this is what the boot log's "Invalid read/write at
 * addr 0x88040/0x88044/0x88050" was: MDIO_CTRL_0/1/4_REG unmapped).
 * board_eth_init() -> ipq_gmac_init() identifies each PHY by reading
 * its MII_PHYSID1/2 (regnum 2/3) over this controller and switching on
 * the result; returning the GEPHY ID (0x004DD0C0, from
 * arch-ipq5018/ipq5018_gmac.h) for the phy_address gmac1_cfg uses (7,
 * same in both ipq5018-emulation.dts and every real board dts checked)
 * makes it take the simple internal-PHY init path
 * (ipq_gephy_phy_init()) instead of needing the external RTL8367
 * switch chip - deliberately NOT emulating that chip (BRINGUP-NOTES.md
 * section 6 originally scoped Ethernet as DesignWare-only; this MDIO
 * detour turned out to be the actual gate, discovered empirically, not
 * anticipated from static reading alone).
 * Any other phy_address gets 0xFFFF (standard "nothing answered"
 * value) - GMAC1/gmac2_cfg's switch-dependent path is left to fail
 * the same way it already was.
 * ============================================================ */

/* GICv2 - see the comment block in mr80x_init() where it's instantiated. */
#define MR80X_GIC_DIST_BASE 0x0B000000
#define MR80X_GIC_CPU_BASE  0x0B002000
#define MR80X_GIC_NUM_IRQ   256
#define MR80X_UART_IRQ      0x6b /* SPI number, per the DT's serial@78af000 */
#define MR80X_BAM_IRQ       0x92 /* SPI number, per the DT's dma@7984000 (QPIC NAND's BAM instance) */

#define MR80X_MDIO_BASE   0x88000
#define MR80X_MDIO_SIZE   0x1000
#define MDIO_CTRL_0_REG   0x40
#define MDIO_CTRL_1_REG   0x44
#define MDIO_CTRL_2_REG   0x48
#define MDIO_CTRL_3_REG   0x4c
#define MDIO_CTRL_4_REG   0x50
#define MDIO_CTRL_4_ACCESS_BUSY 0x10000

#define MR80X_GEPHY_PHY_ADDR 7
#define MR80X_GEPHY_ID       0x004DD0C0

typedef struct MR80XMdioState {
    MemoryRegion iomem;
    uint32_t ctrl1;   /* holds (mii_id << 8 | regnum) from the last write */
    uint32_t result;  /* precomputed read result, latched on CTRL_4 START */
} MR80XMdioState;

static uint64_t mr80x_mdio_read(void *opaque, hwaddr offset, unsigned size)
{
    MR80XMdioState *s = opaque;

    switch (offset) {
    case MDIO_CTRL_3_REG:
        return s->result;
    case MDIO_CTRL_4_REG:
        return 0; /* never busy - the wait_busy() poll loop exits immediately */
    default:
        return 0;
    }
}

static void mr80x_mdio_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MR80XMdioState *s = opaque;

    switch (offset) {
    case MDIO_CTRL_1_REG:
        s->ctrl1 = (uint32_t)value;
        break;
    case MDIO_CTRL_4_REG: {
        unsigned mii_id = (s->ctrl1 >> 8) & 0x1f;
        unsigned regnum = s->ctrl1 & 0xff;

        if (mii_id == MR80X_GEPHY_PHY_ADDR && regnum == 2) {
            s->result = (MR80X_GEPHY_ID >> 16) & 0xffff;
        } else if (mii_id == MR80X_GEPHY_PHY_ADDR && regnum == 3) {
            s->result = MR80X_GEPHY_ID & 0xffff;
        } else if (mii_id == MR80X_GEPHY_PHY_ADDR && regnum == 17) {
            /* GEPHY_PHY_SPEC_STATUS (drivers/net/ipq_common/ipq_gephy.h):
             * report link up, full duplex, 100Mbps -
             * GEPHY_STATUS_LINK_PASS(0x400) | FULL_DUPLEX(0x2000) |
             * SPEED_100MBS(0x80). */
            s->result = 0x2480;
        } else {
            s->result = 0xffff;
        }
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps mr80x_mdio_ops = {
    .read = mr80x_mdio_read,
    .write = mr80x_mdio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ============================================================
 * MSM UART DM: minimum viable console. TF writes go straight to the
 * chardev backend; SR always reports TX ready/empty (no backpressure -
 * this is a boot console, not a throughput test). RX comes from the
 * chardev's input callback into a small ring buffer.
 * ============================================================ */

#define UART_RX_BUF_SIZE 64

typedef struct MR80XUartState {
    MemoryRegion iomem;
    CharBackend chr;
    uint8_t rx_buf[UART_RX_BUF_SIZE];
    unsigned rx_head, rx_tail;
    bool tx_eol_pending;
    uint32_t nchar_remaining;
    /* Only used by the *kernel*'s msm_serial driver (section 25) -
     * u-boot's own qca_uart.c never enables interrupts, it's a pure
     * polling loop, so this stayed NULL/unused for this whole
     * project until the GIC existed at all. Best-effort: raised
     * whenever RX data is pending, matching the same "is there a
     * byte waiting" condition UART_MISR/RXSTALE already reports for
     * u-boot's tstc() (see that comment) - the exact IMR-masking/ack
     * semantics msm_serial.c expects aren't independently confirmed
     * (no kernel source here to grep, unlike appsbl), so this covers
     * the RX-has-data case specifically, not a full interrupt model. */
    qemu_irq irq;
} MR80XUartState;

static bool mr80x_uart_rx_empty(MR80XUartState *s)
{
    return s->rx_head == s->rx_tail;
}

static unsigned mr80x_uart_rx_count(MR80XUartState *s)
{
    return (s->rx_head + UART_RX_BUF_SIZE - s->rx_tail) % UART_RX_BUF_SIZE;
}

static void mr80x_uart_update_irq(MR80XUartState *s)
{
    if (s->irq) {
        qemu_set_irq(s->irq, !mr80x_uart_rx_empty(s));
    }
}

static uint64_t mr80x_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    MR80XUartState *s = opaque;

    switch (offset) {
    case UART_SR: {
        uint32_t sr = UART_SR_TXRDY | UART_SR_TXEMT;
        if (!mr80x_uart_rx_empty(s)) {
            sr |= UART_SR_RXRDY;
        }
        return sr;
    }
    case UART_ISR:
        /* IMR/ISR alias; report nothing pending beyond what SR covers -
         * qca_uart.c only polls specific bits it needs (TX_READY on the
         * TX path via a different helper not wired yet, stale-RX events
         * for RX) - revisit if boot gets stuck polling this. */
        return 0;
    case UART_MISR:
        /* See the RX-detection comment above UART_MISR_RXSTALE's
         * #define - this, not UART_SR/RXRDY, is what tstc()/getc()
         * actually poll to decide "is there a byte waiting". */
        return mr80x_uart_rx_empty(s) ? 0 : UART_MISR_RXSTALE;
    case UART_RX_TOTAL_SNAP:
        return mr80x_uart_rx_count(s);
    case UART_NCHAR + 0x00:
    case UART_TF0:
    case UART_TF0 + 4:
    case UART_TF0 + 8:
    case UART_TF0 + 12:
        /* RF (receive fifo) aliases the same offsets as TF in this
         * variant per uart.h; pull one byte per word-sized read. */
        if (!mr80x_uart_rx_empty(s)) {
            uint8_t c = s->rx_buf[s->rx_tail];
            s->rx_tail = (s->rx_tail + 1) % UART_RX_BUF_SIZE;
            mr80x_uart_update_irq(s);
            return c;
        }
        return 0;
    default:
        return 0;
    }
}

/* ============================================================
 * CoreSight (ARM's on-chip hardware debug/trace fabric - CSR, CTI,
 * TMC, funnels, replicator, ETM, TPDA, STM) is real silicon on the
 * real SoC, but is *debug-only*: JTAG/trace-capture infrastructure
 * for chip bring-up, never touched by normal boot/operation on real
 * hardware either unless a debug probe is actually attached. This
 * emulator obviously can't model it, and the kernel's CoreSight
 * drivers (BRINGUP-NOTES.md section 26) don't uniformly tolerate that
 * absence: several probe with a harmless `-EINVAL`/`-22` failure, but
 * whichever one runs right after "REPLICATOR 1.0 initialized" (no
 * further per-driver log line before the fault, so not pinned down to
 * an exact one without kernel debug symbols this exact 4.4.60 build
 * doesn't have anywhere in this workspace) dereferences a NULL
 * pointer and panics the kernel outright.
 *
 * Real production device trees for boards without a debug probe
 * attached routinely disable these same nodes for exactly this class
 * of reason - this isn't a workaround unique to emulation. Since
 * appsbl/u-boot hands the kernel a device tree extracted from
 * *inside* the signed FIT image (not something this project's own
 * source controls, unlike appsbl itself), the fix has to happen by
 * patching the live, already-loaded blob in guest RAM at the last
 * safe moment - right as u-boot's own console print of "Starting
 * kernel" (the literal last line it ever prints, checked byte-by-byte
 * as it flows through the *existing* UART TX path below, needing no
 * new hook or timing assumption) confirms the DTB is fully placed and
 * about to be handed off, but before the kernel has parsed any of it.
 *
 * No libfdt available to this build (not vendored by QEMU itself -
 * it links the *host's* libfdt package, which this Dockerfile never
 * installs, and none of this project's existing machine code needed
 * it before now) - hand-rolls the trivial parts of the flattened
 * devicetree format instead: walk the structure block's token stream
 * (FDT_BEGIN_NODE/END_NODE/PROP/NOP/END, all that's needed here -
 * see the Devicetree Specification's "flattened format" chapter),
 * and for every "compatible" property whose value contains the
 * substring "coresight" (true of every one of these nodes' bindings:
 * "arm,coresight-*", "qcom,coresight-*"), zero out that property's
 * value in place - same length, no resizing/relocation needed, and a
 * blanked compatible string can't `of_match_device()` against any
 * driver's ID table, so the kernel just leaves that platform_device
 * unbound instead of ever calling into a probe() function for it. */
#define MR80X_FDT_MAGIC 0xd00dfeedu
#define MR80X_FDT_BEGIN_NODE 0x1u
#define MR80X_FDT_END_NODE   0x2u
#define MR80X_FDT_PROP       0x3u
#define MR80X_FDT_NOP        0x4u
#define MR80X_FDT_END        0x9u

/* Tries to walk and patch ONE candidate FDT at physical address
 * `fdt_base`. Returns true only if this candidate is confirmed to be
 * the *real* board device tree (a "compatible" or "model" property
 * containing "ipq5018", matching this exact board's real DT,
 * independently confirmed earlier by dumping and decompiling it -
 * BRINGUP-NOTES.md section 25) - false for anything else, including
 * a header that parses cleanly but isn't the board DT. That
 * distinction matters: u-boot's FIT image container (loaded whole at
 * CONFIG_SYS_LOAD_ADDR, 0x44000000, well before the real board DT
 * u-boot later relocates to run the kernel from) is *itself* stored
 * in valid flattened-devicetree format - its "images"/"configurations"
 * nodes hold the kernel/fdt/etc as opaque data properties - so it
 * passes every structural check a raw scan can make, but patching
 * *it* does nothing (its own tree has no "compatible" properties at
 * all resembling real hardware), leaving the caller looking like it
 * silently found nothing to patch. Modifies `buf`/writes it back to
 * guest RAM only on success; a false return leaves guest RAM
 * untouched. */
/* Per-node tracking, indexed by nesting depth (properties always
 * appear as direct children of FDT_BEGIN_NODE, before any nested
 * child nodes or the matching FDT_END_NODE - reset whenever a new
 * node starts, consulted and acted on at that same node's
 * FDT_END_NODE). A small fixed depth is plenty - this DT nests at
 * most a handful of levels deep (soc -> peripheral -> sub-block). */
#define MR80X_FDT_MAX_DEPTH 32
typedef struct {
    uint32_t compat_val_off;
    uint32_t compat_len;
    bool has_compat;
    bool has_coresight_marker;
} MR80XFdtNodeState;

static bool mr80x_try_patch_fdt_at(hwaddr fdt_base, uint32_t totalsize,
                                    uint32_t off_dt_struct,
                                    uint32_t off_dt_strings)
{
    g_autofree uint8_t *buf = g_malloc(totalsize);
    MR80XFdtNodeState stack[MR80X_FDT_MAX_DEPTH];
    int depth = 0;
    unsigned patched = 0;
    bool is_board_dt = false;
    uint32_t off;

    cpu_physical_memory_read(fdt_base, buf, totalsize);
    memset(&stack[0], 0, sizeof(stack[0]));

    off = off_dt_struct;
    while (off + 4 <= totalsize) {
        uint32_t tok = ldl_be_p(buf + off);

        off += 4;
        if (tok == MR80X_FDT_BEGIN_NODE) {
            while (off < totalsize && buf[off] != 0) {
                off++;
            }
            off = (off + 1 + 3) & ~3u; /* skip the NUL too, then align */
            if (depth + 1 >= MR80X_FDT_MAX_DEPTH) {
                break; /* deeper than any real node in this DT - bail safe */
            }
            depth++;
            memset(&stack[depth], 0, sizeof(stack[depth]));
        } else if (tok == MR80X_FDT_PROP) {
            uint32_t len, nameoff, val_off, i;
            const char *pname;

            if (off + 8 > totalsize) {
                break;
            }
            len = ldl_be_p(buf + off);
            nameoff = ldl_be_p(buf + off + 4);
            val_off = off + 8;
            if ((uint64_t)val_off + len > totalsize) {
                break;
            }
            off = (val_off + len + 3) & ~3u;

            if (off_dt_strings + nameoff >= totalsize) {
                continue;
            }
            pname = (const char *)(buf + off_dt_strings + nameoff);

            if (!strcmp(pname, "compatible") || !strcmp(pname, "model")) {
                for (i = 0; len >= 7 && i + 7 <= len; i++) {
                    if (!memcmp(buf + val_off + i, "ipq5018", 7)) {
                        is_board_dt = true;
                        break;
                    }
                }
            }
            if (!strcmp(pname, "compatible")) {
                stack[depth].compat_val_off = val_off;
                stack[depth].compat_len = len;
                stack[depth].has_compat = true;
            }
            /* Every real CoreSight component node carries at least one
             * "coresight-*" property (coresight-name at minimum, often
             * coresight-ctis/coresight-cpu too) - a far more reliable
             * marker than "compatible" for this subsystem specifically,
             * since several of its nodes (TMC/funnel/ETM/replicator)
             * bind through the generic ARM PrimeCell/AMBA bus via
             * `compatible = "arm,primecell"` plus a numeric
             * `arm,primecell-periphid`, not a "coresight"-named
             * compatible string at all - confirmed by comparing this
             * against the actual decompiled DT after an earlier,
             * narrower "compatible contains coresight" version of this
             * patch left those specific nodes untouched (still visibly
             * probing successfully in dmesg) while only catching CTI/
             * CSR/TPDA/etc, whose compatible strings *do* say
             * "coresight" directly. */
            if (!strncmp(pname, "coresight-", 10)) {
                stack[depth].has_coresight_marker = true;
            }
        } else if (tok == MR80X_FDT_END_NODE) {
            if (stack[depth].has_coresight_marker && stack[depth].has_compat) {
                memset(buf + stack[depth].compat_val_off, 0,
                       stack[depth].compat_len);
                patched++;
            }
            if (depth > 0) {
                depth--;
            }
        } else if (tok == MR80X_FDT_NOP) {
            /* nothing to skip */
        } else {
            break; /* FDT_END, or an unexpected/malformed token - stop */
        }
    }

    if (!is_board_dt) {
        return false;
    }
    if (patched) {
        cpu_physical_memory_write(fdt_base, buf, totalsize);
        info_report("mr80x: disabled %u CoreSight device-tree node(s) at "
                    "0x%" HWADDR_PRIx " before kernel handoff - debug-only "
                    "silicon this emulator can't model, was crashing the "
                    "kernel during probe (see BRINGUP-NOTES.md section 27)",
                    patched, fdt_base);
    }
    return true;
}

/* Scans RAM for the flattened-devicetree magic and tries every
 * plausible-looking header found (see mr80x_try_patch_fdt_at()'s
 * comment for why "plausible-looking" isn't enough on its own to
 * stop at the first hit) until one is confirmed to be the real board
 * DT and patched, or RAM is exhausted. Two known false-positive
 * sources in practice, both confirmed via a live boot log: the
 * *kernel image* itself, decompressed into RAM at a lower address
 * than the real DTB ("Load Address: 0x41208000" vs u-boot's "Loading
 * Device Tree to 0x4a3ef000"), coincidentally contains those same 4
 * magic bytes somewhere in several MB of compiled code/data; and
 * u-boot's own FIT image container, loaded whole at
 * CONFIG_SYS_LOAD_ADDR (0x44000000) *before* the real DTB, which is
 * itself valid FDT-format data (see above) that parses cleanly but
 * isn't the board DT. */
static void mr80x_patch_fdt_disable_coresight(MachineState *machine)
{
    uint8_t *ram = memory_region_get_ram_ptr(machine->ram);
    static const uint8_t magic[4] = { 0xd0, 0x0d, 0xfe, 0xed };
    size_t i;

    for (i = 0; i + 40 <= MR80X_RAM_SIZE; i += 4) {
        uint32_t totalsize, off_dt_struct, off_dt_strings;
        hwaddr fdt_base;

        if (memcmp(ram + i, magic, 4) != 0) {
            continue;
        }
        totalsize = ldl_be_p(ram + i + 4);
        off_dt_struct = ldl_be_p(ram + i + 8);
        off_dt_strings = ldl_be_p(ram + i + 12); /* struct fdt_header:
                                                   * magic(0) totalsize(4)
                                                   * off_dt_struct(8)
                                                   * off_dt_strings(12) -
                                                   * offset 16 is
                                                   * off_mem_rsvmap, a
                                                   * bug caught only by
                                                   * comparing against a
                                                   * live debug dump. */
        if (totalsize == 0 || totalsize > 4 * MiB ||
            (uint64_t)i + totalsize > MR80X_RAM_SIZE ||
            off_dt_struct + 4 > totalsize) {
            continue;
        }
        if (ldl_be_p(ram + i + off_dt_struct) != MR80X_FDT_BEGIN_NODE) {
            continue;
        }

        fdt_base = MR80X_RAM_BASE + i;
        if (mr80x_try_patch_fdt_at(fdt_base, totalsize, off_dt_struct,
                                    off_dt_strings)) {
            return;
        }
        i += totalsize - 4; /* skip the rest of this candidate's blob */
    }

    warn_report("mr80x: could not locate the kernel's device tree in RAM "
                "to disable CoreSight nodes (no candidate FDT header "
                "confirmed as the real board DT) - the kernel will likely "
                "crash probing CoreSight, see BRINGUP-NOTES.md section 27");
}

/* qca_uart.c's msm_boot_uart_dm_write() path
 * (msm_boot_uart_replace_lr_with_cr()) blindly expands every '\n' to
 * "\r\n" - but several call sites already printf literal "\r\n"
 * themselves, so this exact binary's real output contains line
 * endings like bare "\r" with no '\n' at all, or runs of 2-4 '\r' in
 * a row - confirmed byte-for-byte via plain shell redirection (no
 * pty/terminal involved), so this is the real binary's own output,
 * not something introduced between it and a terminal. A real
 * terminal receiving a bare '\r' just returns the cursor to column 0
 * without moving down a line, so *every* one of these makes the next
 * text overwrite the current line instead of starting a new one.
 *
 * Normalize instead of forwarding verbatim: the first '\r' or '\n'
 * of a line ending is expanded to a proper "\r\n"; any '\r'/'\n'
 * immediately following (the redundant partner of that same logical
 * line break, or a repeat of it) is swallowed. Anything else clears
 * the pending state and is forwarded untouched - this deliberately
 * leaves other control characters (e.g. '\b' backspace, used by the
 * "Hit any key to stop autoboot" countdown to rewrite a single digit
 * in place) alone, since those aren't line endings. */
/* Set once, in mr80x_init() - the one MachineState this whole custom
 * board ever has, needed here only to reach machine->ram for the
 * CoreSight device-tree patch above. mr80x_kernel_handoff_match/
 * _triggered are reset in mr80x_reset() (not local statics) so the
 * console `reset` command's second boot cycle re-arms this and
 * re-patches the freshly-reloaded DTB, instead of only ever firing
 * once for the machine's entire lifetime. */
static MachineState *mr80x_machine;
static unsigned mr80x_kernel_handoff_match;
static bool mr80x_kernel_handoff_triggered;

static void mr80x_watch_for_kernel_handoff(uint8_t c)
{
    static const char needle[] = "Starting kernel";

    if (mr80x_kernel_handoff_triggered || !mr80x_machine) {
        return;
    }
    if (c == needle[mr80x_kernel_handoff_match]) {
        mr80x_kernel_handoff_match++;
        if (mr80x_kernel_handoff_match == strlen(needle)) {
            mr80x_kernel_handoff_triggered = true;
            mr80x_patch_fdt_disable_coresight(mr80x_machine);
        }
    } else {
        /* "Starting kernel" has no internal repeats that would need a
         * real partial-match restart (e.g. no re-entrant prefix), so
         * a flat reset-to-zero on any mismatch is exact here. */
        mr80x_kernel_handoff_match = (c == needle[0]) ? 1 : 0;
    }
}

static void mr80x_uart_putc(MR80XUartState *s, uint8_t c)
{
    mr80x_watch_for_kernel_handoff(c);

    if (c == '\r' || c == '\n') {
        if (s->tx_eol_pending) {
            return;
        }
        s->tx_eol_pending = true;
        qemu_chr_fe_write_all(&s->chr, (const uint8_t *)"\r\n", 2);
        return;
    }
    s->tx_eol_pending = false;

    qemu_chr_fe_write_all(&s->chr, &c, 1);
}

static void mr80x_uart_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MR80XUartState *s = opaque;

    switch (offset) {
    case UART_NCHAR:
        /* NO_CHARS_FOR_TX - real hardware requires this written before
         * each TF push, giving the byte count that write (or run of
         * wide writes) actually carries. u-boot's own qca_uart.c
         * always writes 1 before a single char (see the TF0 comment
         * below) - the *kernel*'s msm_serial driver, discovered only
         * once real interrupt-driven output started flowing at all
         * (BRINGUP-NOTES.md section 25/26), instead batches up to 4
         * characters into one 32-bit TF write for throughput. Without
         * tracking this, this model was blindly taking just the low
         * byte of every TF write - silently dropping 3 of every 4
         * kernel console characters, producing garbled/scrambled
         * dmesg output that still *looked* superficially like output
         * was happening, which is what actually surfaced the bug. */
        s->nchar_remaining = (uint32_t)value;
        return;
    case UART_TF0:
    case UART_TF0 + 4:
    case UART_TF0 + 8:
    case UART_TF0 + 12: {
        /* Forward however many of this write's up-to-4 packed bytes
         * NCHAR says are actually valid (low byte first, matching
         * real hardware's FIFO push order) - defaults to 1 if NCHAR
         * was never written (e.g. a hypothetical caller that skips
         * it), preserving this model's original single-char behavior
         * for that case. */
        unsigned n = s->nchar_remaining ? MIN(s->nchar_remaining, 4u) : 1u;

        for (unsigned i = 0; i < n; i++) {
            mr80x_uart_putc(s, (uint8_t)(value >> (i * 8)));
        }
        if (s->nchar_remaining) {
            s->nchar_remaining -= n;
        }
        return;
    }
    case UART_SR:
        /* aliases CSR (baud rate select) in this variant - write-only,
         * never read back, no-op is correct (see file header comment). */
        return;
    default:
        return;
    }
}

static const MemoryRegionOps mr80x_uart_ops = {
    .read = mr80x_uart_read,
    .write = mr80x_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mr80x_uart_rx(void *opaque, const uint8_t *buf, int size)
{
    MR80XUartState *s = opaque;
    for (int i = 0; i < size; i++) {
        unsigned next = (s->rx_head + 1) % UART_RX_BUF_SIZE;
        if (next == s->rx_tail) {
            break; /* drop on overflow */
        }
        s->rx_buf[s->rx_head] = buf[i];
        s->rx_head = next;
    }
    mr80x_uart_update_irq(s);
}

static int mr80x_uart_can_rx(void *opaque)
{
    return UART_RX_BUF_SIZE - 1;
}

static void mr80x_uart_event(void *opaque, QEMUChrEvent event) {}

/* ============================================================
 * GMAC1 - MAC config registers at base+0x0/+0x4, DesignWare-style DMA
 * block at base+0x1000 (drivers/net/ipq5018/ipq5018_gmac.c,
 * arch-ipq5018/ipq5018_gmac.h - NOT drivers/net/designware.c, this
 * SoC has its own copy, see BRINGUP-NOTES.md section 10). TX/RX are
 * driven by the driver polling an OWNERSHIP BIT inside the descriptor
 * structs themselves (in guest RAM), not a hardware status register -
 * ipq_eth_send() writes DmaTxPollDemand then spins re-reading its own
 * descriptor's status word until bit31 (DescOwnByDma) clears.  That
 * means the "device" side of TX is entirely: on the poll-demand
 * write, read the current descriptor, do the send, clear the bit,
 * write it back - no interrupt or async completion needed. RX is the
 * mirror: on a real incoming packet, find the current RX descriptor,
 * check *it* still has the ownership bit set (meaning it's free for
 * us to fill), write the packet + frame length, clear the bit.
 *
 * struct ipq_gmac_desc_t (32 bytes, 8-word enhanced descriptor):
 *   +0  status   (u32, bit31 DescOwnByDma, RX frame length in bits 29:16)
 *   +4  length   (u32, TX buffer1 size in bits 12:0)
 *   +8  buffer1  (u32, physical address of packet data)
 *   +12 data1    (u32, NEXT descriptor's physical address - the ring is a
 *                 chain of pointers set up by the driver, not computed
 *                 from a fixed stride)
 *   +16..28 extstatus/reserved1/timestamplow/timestamphigh (unused here)
 * ============================================================ */

#define GMAC_DMA_OFFSET          0x1000
#define GMAC_DMA_BUSMODE         (GMAC_DMA_OFFSET + 0x00)
#define GMAC_DMA_TXPOLLDEMAND    (GMAC_DMA_OFFSET + 0x04)
#define GMAC_DMA_RXPOLLDEMAND    (GMAC_DMA_OFFSET + 0x08)
#define GMAC_DMA_RXBASEADDR      (GMAC_DMA_OFFSET + 0x0C)
#define GMAC_DMA_TXBASEADDR      (GMAC_DMA_OFFSET + 0x10)
#define DMAMAC_SRST              (1 << 0)

#define DESC_OWN_BY_DMA          0x80000000u
#define DESC_FRAME_LEN_MASK      0x3FFF0000u
#define DESC_FRAME_LEN_SHIFT     16
#define DESC_SIZE1_MASK          0x00001FFFu

#define GMAC_MAX_FRAME 2048

#define TYPE_MR80X_GMAC "mr80x-gmac"
OBJECT_DECLARE_SIMPLE_TYPE(MR80XGmacState, MR80X_GMAC)

struct MR80XGmacState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    NICState *nic;
    NICConf conf;
    uint32_t regs[0x2000 / 4]; /* covers the whole iomem region below */
    hwaddr cur_tx_desc;
    hwaddr cur_rx_desc;
    bool have_rx_desc;
};

static void mr80x_gmac_do_tx(MR80XGmacState *s)
{
    hwaddr d = s->cur_tx_desc;
    uint32_t status, length, buffer1, next;
    uint8_t buf[GMAC_MAX_FRAME];
    unsigned len;

    if (!d) {
        return;
    }
    cpu_physical_memory_read(d + 0, &status, 4);
    status = le32_to_cpu(status);
    if (!(status & DESC_OWN_BY_DMA)) {
        return; /* nothing queued */
    }
    cpu_physical_memory_read(d + 4, &length, 4);
    cpu_physical_memory_read(d + 8, &buffer1, 4);
    cpu_physical_memory_read(d + 12, &next, 4);
    length = le32_to_cpu(length);
    buffer1 = le32_to_cpu(buffer1);
    next = le32_to_cpu(next);

    len = length & DESC_SIZE1_MASK;
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    cpu_physical_memory_read(buffer1, buf, len);
    qemu_send_packet(qemu_get_queue(s->nic), buf, len);

    status &= ~DESC_OWN_BY_DMA;
    status = cpu_to_le32(status);
    cpu_physical_memory_write(d + 0, &status, 4);

    s->cur_tx_desc = next;
}

static ssize_t mr80x_gmac_receive(NetClientState *nc, const uint8_t *buf,
                                   size_t size)
{
    MR80XGmacState *s = qemu_get_nic_opaque(nc);
    hwaddr d = s->cur_rx_desc;
    uint32_t status, buffer1, next, framelen;

    if (!s->have_rx_desc || !d || size > GMAC_MAX_FRAME - 4) {
        return 0;
    }
    cpu_physical_memory_read(d + 0, &status, 4);
    status = le32_to_cpu(status);
    if (!(status & DESC_OWN_BY_DMA)) {
        return 0; /* driver hasn't given this slot back to us yet */
    }
    cpu_physical_memory_read(d + 8, &buffer1, 4);
    cpu_physical_memory_read(d + 12, &next, 4);
    buffer1 = le32_to_cpu(buffer1);
    next = le32_to_cpu(next);

    cpu_physical_memory_write(buffer1, buf, size);

    /* ipq_eth_recv() does `length - 4` assuming a 4-byte FCS trailer
     * that real MAC hardware strips-but-still-counts; our virtual NIC
     * packets have no FCS, so report size+4 to keep that math correct
     * without actually needing 4 extra real bytes in the buffer. */
    framelen = ((uint32_t)(size + 4) << DESC_FRAME_LEN_SHIFT) &
               DESC_FRAME_LEN_MASK;
    status = cpu_to_le32(framelen); /* ownership bit cleared: hand to driver */
    cpu_physical_memory_write(d + 0, &status, 4);

    s->cur_rx_desc = next;
    return size;
}

static int mr80x_gmac_can_receive(NetClientState *nc)
{
    MR80XGmacState *s = qemu_get_nic_opaque(nc);
    uint32_t status;

    if (!s->have_rx_desc || !s->cur_rx_desc) {
        return 0;
    }
    cpu_physical_memory_read(s->cur_rx_desc, &status, 4);
    return (le32_to_cpu(status) & DESC_OWN_BY_DMA) != 0;
}

static uint64_t mr80x_gmac_read(void *opaque, hwaddr offset, unsigned size)
{
    MR80XGmacState *s = opaque;

    /* ipq_mac_reset() writes DMAMAC_SRST then busy-polls busmode until
     * it self-clears (real DMA hardware clears it within a few bus
     * cycles). Model that by clearing it on readback - without this,
     * the poll spins forever and ipq_eth_init() never reaches its RX/
     * TX descriptor ring setup, so no packet can ever be sent or
     * received despite PHY link-up succeeding earlier in the same
     * function (that's why "eth0 up Speed :100" printed but no ARP
     * reply for 192.168.0.1 ever went out - the RX ring was never
     * programmed for the driver to have anything to receive into). */
    if (offset == GMAC_DMA_BUSMODE) {
        s->regs[offset / 4] &= ~DMAMAC_SRST;
    }
    return s->regs[offset / 4];
}

static void mr80x_gmac_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MR80XGmacState *s = opaque;
    s->regs[offset / 4] = (uint32_t)value;

    switch (offset) {
    case GMAC_DMA_RXBASEADDR:
        s->cur_rx_desc = value;
        s->have_rx_desc = true;
        break;
    case GMAC_DMA_TXBASEADDR:
        s->cur_tx_desc = value;
        break;
    case GMAC_DMA_TXPOLLDEMAND:
        mr80x_gmac_do_tx(s);
        break;
    case GMAC_DMA_RXPOLLDEMAND:
        /* Nothing to do - we push received packets in as they arrive
         * via mr80x_gmac_receive() rather than waiting to be polled;
         * this write just means "driver refilled a descriptor". */
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mr80x_gmac_ops = {
    .read = mr80x_gmac_read,
    .write = mr80x_gmac_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static NetClientInfo mr80x_gmac_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = mr80x_gmac_receive,
    .can_receive = mr80x_gmac_can_receive,
};

static void mr80x_gmac_realize(DeviceState *dev, Error **errp)
{
    MR80XGmacState *s = MR80X_GMAC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mr80x_gmac_ops, s,
                           "mr80x.gmac", 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&mr80x_gmac_net_info, &s->conf,
                           object_get_typename(OBJECT(dev)), dev->id,
                           &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const Property mr80x_gmac_properties[] = {
    DEFINE_NIC_PROPERTIES(MR80XGmacState, conf),
};

static void mr80x_gmac_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = mr80x_gmac_realize;
    device_class_set_props(dc, mr80x_gmac_properties);
}

static const TypeInfo mr80x_gmac_typeinfo = {
    .name = TYPE_MR80X_GMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MR80XGmacState),
    .class_init = mr80x_gmac_class_init,
};

static void mr80x_gmac_register_types(void)
{
    type_register_static(&mr80x_gmac_typeinfo);
}
type_init(mr80x_gmac_register_types);

/* ============================================================
 * Machine init
 * ============================================================ */

/* Writes one smem_alloc_info entry (allocated=1, offset, size=8) plus
 * a single little-endian uint32 payload at that offset - the shape
 * every SMEM_BOOT_FLASH_* read uses (smem_read_alloc_entry() is always
 * called with len=sizeof(uint32_t), padded to 8 by its own
 * (len+7)&~7 formula). */
static void mr80x_smem_fake_u32_entry(unsigned type, hwaddr data_off,
                                       uint32_t value)
{
    uint32_t v;
    hwaddr entry = MR80X_SMEM_BASE + MR80X_SMEM_ALLOC_INFO_OFF + type * 16;

    v = cpu_to_le32(1);
    cpu_physical_memory_write(entry + 0, &v, 4);
    v = cpu_to_le32(data_off);
    cpu_physical_memory_write(entry + 4, &v, 4);
    v = cpu_to_le32(8);
    cpu_physical_memory_write(entry + 8, &v, 4);

    v = cpu_to_le32(value);
    cpu_physical_memory_write(MR80X_SMEM_BASE + data_off, &v, 4);
}

/* ============================================================
 * Monitor-mode SMC trampoline. appsbl runs with no real secure
 * monitor (sbl1/qsee, BRINGUP-NOTES.md section 7b) present, yet
 * several early boot-path functions - do_bootipq()'s QFPROM
 * authenticate check, qti_scm_pshold()'s reset fallback,
 * is_scm_armv8()'s own calling-convention probe (all in
 * arch/arm/cpu/armv7/qca/common/scm.c) - issue a bare `smc #0` and
 * expect a well-defined error code back in r0, not a crash.
 *
 * Without any code at the CPU's Monitor-mode SMC vector
 * (MVBAR + 0x08), `smc` traps into whatever garbage/unmapped memory
 * MVBAR happens to reset to (0, architecturally, since only Secure
 * firmware we don't run would ever program it) - this was the exact
 * cause of the prefetch-abort-at-pc=0xc crash immediately after
 * "Hit any key to stop autoboot": do_bootipq() calls
 * qca_scm_call(SCM_SVC_FUSE, QFPROM_IS_AUTHENTICATE_CMD, ...) as its
 * very first hardware access, before ever touching NAND - confirmed
 * by the crash's LR being byte-identical whether or not NAND
 * identification (section above) succeeds.
 *
 * Fix: point MVBAR (set directly on the QEMU CPU object at reset,
 * since no guest code path can ever legitimately set it without
 * secure firmware) at a 2-instruction trampoline placed at this
 * vector's slot:
 *     mvn  r0, #3   ; r0 = 0xFFFFFFFC = SCM_EOPNOTSUPP (-4)
 *     movs pc, lr   ; return from Monitor mode to the smc's caller
 * scm.c's own error-remap table turns SCM_EOPNOTSUPP into
 * -EOPNOTSUPP, which every caller in this codebase already treats
 * as "SCM feature not present, continue without it" - exactly the
 * real-hardware-without-TrustZone-firmware behavior we want instead
 * of a crash. is_scm_armv8()'s version-probe call also just reads
 * r0: a nonzero/failing r0 there correctly makes it fall back to the
 * legacy SCM calling convention for every later call too, so one
 * trampoline handles both call conventions used in this file.
 * ============================================================ */

/* Must live inside the SAME 1MiB section as CONFIG_SYS_TEXT_BASE
 * (0x4A920000) - NOT some arbitrary "unused" RAM address. Three
 * earlier attempts elsewhere in RAM (0x00080000; top of the full
 * 512MiB QEMU is given; top of the 256MiB the "DRAM: 256 MiB" boot
 * message reports) all still faulted instruction fetch with IFSR 0xd
 * (Permission fault), confirmed via `-d int` exception tracing. Root
 * cause, found in arch/arm/lib/cache-cp15.c's dram_bank_mmu_setup()
 * (CONFIG_IPQ_NO_RELOC path, which ipq5018.h enables): u-boot's own
 * static page table first marks the *entire* 4GB address space
 * SHARED_DEVICE (execute-unfriendly), then for DRAM specifically
 * marks only the one 1MiB section containing CONFIG_SYS_TEXT_BASE
 * with the exec-friendly UBOOT_CACHE_SETUP attribute - every other
 * MiB, including ones that are perfectly valid backing RAM in QEMU,
 * keeps the earlier device-like attribute and cannot be fetched from.
 *
 * Originally placed 4KiB *below* the entry point - inside that same
 * safe 1MiB section, but WRONG anyway: ipq5018.h's own memory map
 * comment (ascii diagram above CONFIG_SYS_INIT_SP_ADDR) shows the
 * malloc heap, page table, gd/bd structs and all three exception
 * stacks are carved out of the ~1MiB region immediately *below*
 * text_base, growing toward it - i.e. exactly where the trampoline
 * sat. Confirmed via gdb: bytes at that address were the correct
 * trampoline content at reset time, but had already been zeroed out
 * (real malloc()/heap activity, not a QEMU bug) by the time execution
 * reached is_scm_armv8() - explaining the "trampoline bytes correct
 * at start, garbage/all-zero by the time it's actually used" mystery
 * from earlier sessions, and the resulting silent
 * reset-back-to-U-Boot-banner every normal boot hit right after
 * "Hit any key to stop autoboot".
 *
 * First retry - entry + 0xD0000, "comfortably above the image" - was
 * ALSO wrong, and zeroed too, for a different reason: u-boot's own
 * BSS section (__bss_start=0x4A9AF298 to __bss_end=0x4A9F84E0 in this
 * build's u-boot.map, ~295KiB) extends *well* past the raw image size
 * used for that estimate, and BSS gets zeroed by the C runtime very
 * early - long before is_scm_armv8() runs. 0x4A9F0008 sat inside it.
 *
 * Fixed for real by moving past *both* the image and its BSS:
 * entry + 0xD9000 (0x4A9F9000) sits just above __bss_end (0x4A9F84E0)
 * with ~2.8KiB margin, and reserve_mmu()'s SKIP_RELOC-path TLB table
 * (CONFIG_SYS_TEXT_BASE + mon_len, rounded up to the next 64KiB - see
 * common/board_f.c) lands at 0x4AA00000, a full 1MiB section *above*
 * this one (mon_len tracks __bss_end, and 0x4A9F84E0 rounds up past
 * the 0x4A9FFFFF section boundary) - so it doesn't reach back down
 * into this leftover space either. Still safely inside the same
 * exec-permitted 1MiB section (text_base sits 0x20000 into it, section
 * ends at 0x4A9FFFFF, leaving ~0x6FFF bytes of headroom past this
 * address). Verified via gdb that these bytes are still intact (not
 * zeroed) at the is_scm_armv8() breakpoint, unlike both earlier
 * locations. */
#define MR80X_MVBAR_BASE (MR80X_APPSBL_ENTRY + 0xD9000)
#define MR80X_MVBAR_SIZE 0x60
#define MR80X_MVBAR_SMC_OFF 0x08

/* ============================================================
 * Everything below re-populates RAM content on every reset, not just
 * the first boot - see BRINGUP-NOTES.md section 18. Real hardware's
 * GCNT_PSHOLD write (mr80x_pshold_write() above) triggers an actual
 * power-cycle: the DRAM controller re-inits from scratch (RAM content
 * is genuinely gone, not preserved), and SBL re-runs on every
 * power-up, re-loading appsbl into RAM and re-populating SMEM fresh
 * before ever jumping into it. A QEMU "warm" qemu_system_reset()
 * resets CPU/device state but *preserves* RAM by default - fine for
 * most guest OSes, but wrong for this specific reset source, and the
 * mismatch was root-caused (via gdb) to a deterministic malloc()
 * corruption on the *second* normal-boot cycle: u-boot's own
 * heap/BSS state from the first cycle's malloc() usage was still
 * sitting in RAM, an assumption real hardware's actual power-cycle
 * would never let it make. Fixed by clearing all of RAM and re-doing
 * every one-time mr80x_init()-era setup (SMEM fakes, ELF/image load,
 * MVBAR trampoline) here instead, called from mr80x_reset() on every
 * reset including the implicit first one QEMU performs automatically
 * after machine init - so "first boot" and "every reset after" are
 * now the exact same code path, matching real hardware.
 * ============================================================ */

typedef struct MR80XResetState {
    ARMCPU *cpu;
    MachineState *machine;
} MR80XResetState;

static void mr80x_populate_ram(MachineState *machine)
{
    void *ram_ptr = memory_region_get_ram_ptr(machine->ram);
    memset(ram_ptr, 0, MR80X_RAM_SIZE);

    /* Fake just enough SMEM for fdtdec_setup()'s machid lookup to
     * succeed - see the comment by MR80X_SMEM_BASE above. */
    {
        uint32_t v;
        hwaddr entry = MR80X_SMEM_BASE + MR80X_SMEM_ALLOC_INFO_OFF +
                        MR80X_SMEM_MACHID_TYPE * 16;

        v = cpu_to_le32(1);
        cpu_physical_memory_write(entry + 0, &v, 4);   /* allocated */
        v = cpu_to_le32(MR80X_SMEM_MACHID_DATA_OFF);
        cpu_physical_memory_write(entry + 4, &v, 4);   /* offset */
        v = cpu_to_le32(8);
        cpu_physical_memory_write(entry + 8, &v, 4);   /* size */

        v = cpu_to_le32(0);
        cpu_physical_memory_write(MR80X_SMEM_BASE + MR80X_SMEM_MACHID_DATA_OFF,
                                   &v, 4);              /* format */
        v = cpu_to_le32(MR80X_TARGET_MACHID);
        cpu_physical_memory_write(
            MR80X_SMEM_BASE + MR80X_SMEM_MACHID_DATA_OFF + 4, &v, 4);
    }

    /* Fake SMEM_BOOT_FLASH_* so enable_caches() actually enables the
     * MMU/D-cache instead of taking the "JTAG recovery" no-MMU path -
     * see the comment by MR80X_SMEM_FLASH_TYPE_TYPE above. */
    mr80x_smem_fake_u32_entry(MR80X_SMEM_FLASH_TYPE_TYPE,
                               MR80X_SMEM_FLASH_DATA_OFF,
                               MR80X_SMEM_BOOT_NAND_FLASH);
    mr80x_smem_fake_u32_entry(MR80X_SMEM_FLASH_INDEX_TYPE,
                               MR80X_SMEM_FLASH_DATA_OFF + 0x10, 0);
    mr80x_smem_fake_u32_entry(MR80X_SMEM_FLASH_CHIP_SELECT_TYPE,
                               MR80X_SMEM_FLASH_DATA_OFF + 0x20, 0);
    mr80x_smem_fake_u32_entry(MR80X_SMEM_FLASH_BLOCK_SIZE_TYPE,
                               MR80X_SMEM_FLASH_DATA_OFF + 0x30, 0x20000);
    mr80x_smem_fake_u32_entry(MR80X_SMEM_FLASH_DENSITY_TYPE,
                               MR80X_SMEM_FLASH_DATA_OFF + 0x40,
                               128 * 1024 * 1024);

    /* SMEM_HW_SW_BUILD_ID - see the comment by
     * MR80X_SMEM_HW_SW_BUILD_ID_TYPE above. All-zero payload (already
     * zeroed by memset above), only the alloc_info triplet needs
     * writing. */
    {
        uint32_t v;
        hwaddr entry = MR80X_SMEM_BASE + MR80X_SMEM_ALLOC_INFO_OFF +
                        MR80X_SMEM_HW_SW_BUILD_ID_TYPE * 16;

        v = cpu_to_le32(1);
        cpu_physical_memory_write(entry + 0, &v, 4);   /* allocated */
        v = cpu_to_le32(MR80X_SMEM_SOCINFO_DATA_OFF);
        cpu_physical_memory_write(entry + 4, &v, 4);   /* offset */
        v = cpu_to_le32(MR80X_SMEM_SOCINFO_SIZE);
        cpu_physical_memory_write(entry + 8, &v, 4);   /* size */
    }

    /* SMEM_AARM_PARTITION_TABLE - see the comment by
     * MR80X_SMEM_PTABLE_TYPE above. All 16 real partitions from the
     * flash dump (BRINGUP-NOTES.md section 4b), not just the 3 this
     * emulator originally needed to unblock specific boot steps -
     * requested so every real partition is visible/usable from the
     * console (`smeminfo`, `nand` commands, etc) without needing to
     * extend this table by hand each time a new one turns out to
     * matter. Names/offsets/sizes for "0:APPSBLENV", "rootfs" and
     * "0:ART" are confirmed exact matches against real lookup calls
     * (board_init.c, nm_fwup.c, ethaddr.c respectively - grepped the
     * literal strings). The rest follow the same *convention* every
     * Qualcomm MIBIB partition table on this SoC family uses
     * ("0:NAME" uppercase for firmware/system partitions; bare
     * lowercase for UBI-hosted OS/data partitions, matching rootfs's
     * own confirmed pattern) but aren't individually
     * source-confirmed the way those three are - if one turns out to
     * be looked up under a different exact string, only that one
     * entry needs correcting. Rest of the 912-byte struct (unused
     * slots) is left as already-zeroed fresh RAM. */
    {
        static const struct {
            const char *name;
            uint32_t start;
            uint32_t size;
        } parts[] = {
            { "0:SBL1",       0x000000, 0x080000 },
            { "0:MIBIB",      0x080000, 0x080000 },
            { "0:BOOTCONFIG", 0x100000, 0x040000 },
            { "0:BOOTCONFIG1",0x140000, 0x040000 },
            { "0:QSEE",       0x180000, 0x100000 },
            { "0:DEVCFG",     0x280000, 0x040000 },
            { "0:CDT",        0x2C0000, 0x040000 },
            { "0:APPSBLENV",  0x300000, 0x080000 },
            { "0:APPSBL",     0x380000, 0x140000 },
            { "0:ART",        0x4C0000, 0x100000 },
            { "0:TRAINING",   0x5C0000, 0x080000 },
            { "rootfs",       0x640000, 0x2A00000 },
            { "rootfs_1",     0x3040000, 0x2A00000 },
            { "tp-data",      0x5A40000, 0x840000 },
            { "radio",        0x6280000, 0x440000 },
            { "data",         0x66C0000, 0x080000 },
        };
        uint32_t v;
        hwaddr entry = MR80X_SMEM_BASE + MR80X_SMEM_ALLOC_INFO_OFF +
                        MR80X_SMEM_PTABLE_TYPE * 16;
        hwaddr data = MR80X_SMEM_BASE + MR80X_SMEM_PTABLE_DATA_OFF;
        unsigned i;

        v = cpu_to_le32(1);
        cpu_physical_memory_write(entry + 0, &v, 4);
        v = cpu_to_le32(MR80X_SMEM_PTABLE_DATA_OFF);
        cpu_physical_memory_write(entry + 4, &v, 4);
        v = cpu_to_le32(MR80X_SMEM_PTABLE_SIZE);
        cpu_physical_memory_write(entry + 8, &v, 4);

        v = cpu_to_le32(MR80X_SMEM_PTABLE_MAGIC_1);
        cpu_physical_memory_write(data + 0, &v, 4);
        v = cpu_to_le32(MR80X_SMEM_PTABLE_MAGIC_2);
        cpu_physical_memory_write(data + 4, &v, 4);
        v = cpu_to_le32(1); /* version */
        cpu_physical_memory_write(data + 8, &v, 4);
        v = cpu_to_le32(ARRAY_SIZE(parts));
        cpu_physical_memory_write(data + 12, &v, 4);

        /* struct smem_ptn { char name[16]; u32 start; u32 size; u32 attr; }
         * (packed, 28 bytes), start/size in flash_block_size (0x20000)
         * units - board_init() hard-requires finding "0:APPSBLENV" via
         * smem_getpart() or it aborts boot the same fatal way ptable
         * itself did. */
        for (i = 0; i < ARRAY_SIZE(parts); i++) {
            char name[16];
            hwaddr part = data + 16 + i * 28;

            memset(name, 0, sizeof(name));
            pstrcpy(name, sizeof(name), parts[i].name);
            cpu_physical_memory_write(part + 0, name, 16);
            v = cpu_to_le32(parts[i].start / 0x20000);
            cpu_physical_memory_write(part + 16, &v, 4);
            v = cpu_to_le32(parts[i].size / 0x20000);
            cpu_physical_memory_write(part + 20, &v, 4);
            v = cpu_to_le32(0);
            cpu_physical_memory_write(part + 24, &v, 4); /* attr */
        }
    }

    /* Monitor-mode SMC trampoline - see the MR80X_MVBAR_BASE comment
     * block above mr80x_reset(). MUST be re-written here on every
     * populate/reset cycle: this function's own memset() above wipes
     * it out along with the rest of RAM otherwise - confirmed via
     * gdb that this exact code was silently missing after an earlier
     * refactor moved trampoline-writing out of mr80x_init() and into
     * this function without actually bringing the write itself along,
     * leaving the trampoline's memory as all-zero ("andeq r0,r0,r0")
     * instead of the intended 2 instructions - explaining a
     * from-here-on class of "SMC calls never return" crashes that
     * looked like exotic ARM/QEMU Monitor-mode semantics but were
     * really just this. */
    {
        /* Extended trampoline (see the Option A comment block below
         * mr80x_reset() for the full rationale) - assembled from real
         * source via the same ARM cross-toolchain this project's own
         * appsbl build already uses
         * (mr80x-appsbl-builder:openwrt-gcc5.2-binutils2.24), not
         * hand-derived, to avoid exactly the class of bit-level
         * encoding mistakes hand-assembly invites. Full source and
         * rationale (including *why* the two SMC calling conventions
         * below need different default return values - the actual
         * bug that blocked Option A the first time around) is in
         * board/mvbar-trampoline.S:
         *
         *   movw    ip, #0x0601      ; is_scm_armv8() probe:
         *   movt    ip, #0x8200      ; fn_id 0x82000601 ->
         *   cmp     r0, ip           ; pretend an armv8 TZ answered
         *   bne     1f               ; "yes" (r0=0, r1=1) so appsbl's
         *   mov     r0, #0           ; jump_kernel64() doesn't just
         *   mov     r1, #1           ; hang() before ever trying the
         *   movs    pc, lr           ; real SMC below.
         * 1:
         *   movw    ip, #0x010f      ; jump_kernel64()'s own SMC:
         *   movt    ip, #0x0200      ; fn_id 0x0200010f - r2 holds the
         *   cmp     r0, ip           ; physical address of scm.c's
         *   bne     2f               ; on-stack `kernel_params` struct.
         *   movw    ip, #(MR80X_HANDOFF_TRIGGER_BASE & 0xffff)
         *   movt    ip, #(MR80X_HANDOFF_TRIGGER_BASE >> 16)
         *   str     r2, [ip]        ; hand r2 to mr80x_handoff_trigger_write()
         *   b       3f
         * 2:
         *   cmp     r0, #1          ; r0==1 is the *legacy* SCM calling
         *   beq     4f              ; convention's fixed trap value
         *                           ; (smc.c's smc()) - never a real
         *                           ; armv8 QCA_SCM_FNID value.
         * 3:
         *   mvn     r0, #94         ; armv8 convention default: -95
         *   movs    pc, lr          ; (-EOPNOTSUPP, pre-remapped -
         *                           ; __scm_call_64() doesn't call
         *                           ; scm_remap_error() itself, unlike
         *                           ; the legacy path below).
         * 4:
         *   mvn     r0, #3          ; legacy convention default: raw
         *   movs    pc, lr          ; SCM_EOPNOTSUPP (-4) -
         *                           ; __scm_call() remaps this itself.
         *
         * The two movw/movt pairs above encode MR80X_HANDOFF_TRIGGER_
         * BASE (0x0A000000) directly - this array must be
         * re-assembled from board/mvbar-trampoline.S (see that file's
         * header for the exact command) if that address ever
         * changes. */
        static const uint8_t trampoline[] = {
            0x01, 0xc6, 0x00, 0xe3, 0x00, 0xc2, 0x48, 0xe3,
            0x0c, 0x00, 0x50, 0xe1, 0x02, 0x00, 0x00, 0x1a,
            0x00, 0x00, 0xa0, 0xe3, 0x01, 0x10, 0xa0, 0xe3,
            0x0e, 0xf0, 0xb0, 0xe1, 0x0f, 0xc1, 0x00, 0xe3,
            0x00, 0xc2, 0x40, 0xe3, 0x0c, 0x00, 0x50, 0xe1,
            0x03, 0x00, 0x00, 0x1a, 0x00, 0xc0, 0x00, 0xe3,
            0x00, 0xca, 0x40, 0xe3, 0x00, 0x20, 0x8c, 0xe5,
            0x01, 0x00, 0x00, 0xea, 0x01, 0x00, 0x50, 0xe3,
            0x01, 0x00, 0x00, 0x0a, 0x5e, 0x00, 0xe0, 0xe3,
            0x0e, 0xf0, 0xb0, 0xe1, 0x03, 0x00, 0xe0, 0xe3,
            0x0e, 0xf0, 0xb0, 0xe1,
        };
        QEMU_BUILD_BUG_ON(sizeof(trampoline) > MR80X_MVBAR_SIZE);
        cpu_physical_memory_write(MR80X_MVBAR_BASE + MR80X_MVBAR_SMC_OFF,
                                   trampoline, sizeof(trampoline));
    }

    /* NOTE: the ELF/image itself is NOT (re-)loaded here - QEMU's own
     * loader functions (called once from mr80x_init(), below) already
     * register the loaded data as a "ROM" blob that QEMU automatically
     * re-applies on every reset via its own internal rom_reset()
     * handler (registered at that same call, guaranteed to run AFTER
     * mr80x_reset() - see the ordering comment in mr80x_init()). Calling
     * load_elf_as()/load_image_targphys() a second time here would hit
     * QEMU's "ROM images must be loaded at startup" hard error - this
     * function's memset() above only needs to leave a clean slate for
     * that automatic restore to land on. */
}

/* ============================================================
 * Option A: the real appsbl AArch32->AArch64 handoff. appsbl's own
 * arch/arm/lib/bootm.c calls jump_kernel64(kernel_entry, ft_addr) as
 * the very last thing it does for a 64-bit kernel (matching real
 * hardware - IPQ5018 is a genuine Cortex-A53, see the AArch64 test
 * path comment above mr80x_aarch64_reset() and BRINGUP-NOTES.md
 * section 28) - jump_kernel64() is declared noreturn and, on real
 * hardware, never actually returns: TrustZone firmware handles the
 * SMC by dropping straight into AArch64 at kernel_entry with x0=fdt,
 * no ERET back to the AArch32 caller at all.
 *
 * The MVBAR trampoline above (board/mvbar-trampoline.S) recognizes
 * this specific SMC (fn_id 0x0200010f) and STRs r2 - the physical
 * address of scm.c's on-stack `kernel_params` struct - to this
 * dedicated MMIO register instead of just returning SCM_EOPNOTSUPP.
 * The write handler below reads reg_x0 (fdt address, struct offset 0)
 * and kernel_start (struct offset 72, see the `kernel_params` typedef
 * in arch-qca-common/scm.h) out of guest RAM, saves them, and
 * requests a machine reset - mirroring real hardware's own "this SMC
 * never returns to its caller" behavior instead of trying to emulate
 * a live in-flight AArch32->AArch64 ERET transition. (That
 * alternative was researched in depth: QEMU's own
 * target/arm/tcg/helper-a64.c HELPER(exception_return) has the
 * reference sequence, but it's for a CPU that keeps running through
 * the transition - appsbl doesn't need that, it's already handing
 * off for good.)
 *
 * mr80x_reset()'s pending-handoff branch (below) then does exactly
 * what mr80x_aarch64_reset() already does successfully for the
 * isolated AArch64 test path: cpu_reset() + cpu_set_pc() + x0=fdt,
 * *without* re-running mr80x_populate_ram() - the kernel Image and
 * FDT appsbl's own (already-working) FIT-loading logic placed in RAM
 * survive the reset untouched, only the CPU state resets to point at
 * them in AArch64.
 *
 * The one new wrinkle reset() alone doesn't solve: QEMU's
 * arm_cpu_reset_hold() unconditionally sets env->aarch64=true on
 * *every* reset for any CPU with the AARCH64 feature bit - fine for
 * the isolated test path (which never runs AArch32 code at all), but
 * appsbl itself must reset into AArch32 on its own (first, and every
 * plain `reset`) boot. Fixed by *dynamically* toggling the
 * ARM_FEATURE_AARCH64 (and, only for the handoff reset itself,
 * ARM_FEATURE_EL3 - matching the isolated path's has_el3=off, needed
 * per BRINGUP-NOTES.md section 28 to avoid an EL3->EL0 reset
 * fallback) feature bits via QEMU's own set_feature()/unset_feature()
 * (target/arm/cpu.h, already public, no core QEMU source touched) at
 * the start of mr80x_reset() - both bits are checked dynamically by
 * the emulator throughout a CPU's life, not just at realize, so this
 * is safe to flip per-reset. This machine's CPU model itself also had
 * to move from cortex-a7 to cortex-a53 (run.sh's normal invocation
 * now always passes `-cpu cortex-a53,aarch64=on`) so the AArch64
 * register/cp_regs set actually exists to switch into - real hardware
 * is the same single Cortex-A53 core throughout appsbl and the
 * kernel, never an A7, so this is more accurate anyway, not a
 * divergence.
 * ============================================================ */
#define MR80X_HANDOFF_TRIGGER_BASE 0x0A000000
#define MR80X_HANDOFF_TRIGGER_SIZE 0x1000

static bool mr80x_aarch32_handoff_pending;
static hwaddr mr80x_aarch32_handoff_kernel_entry;
static hwaddr mr80x_aarch32_handoff_fdt_addr;

static uint64_t mr80x_handoff_trigger_read(void *opaque, hwaddr offset,
                                            unsigned size)
{
    return 0;
}

static void mr80x_handoff_trigger_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size)
{
    hwaddr params_addr = (hwaddr)(uint32_t)value;
    uint32_t fdt_lo, kernel_lo;

    cpu_physical_memory_read(params_addr + 0, &fdt_lo, 4);
    cpu_physical_memory_read(params_addr + 72, &kernel_lo, 4);

    mr80x_aarch32_handoff_fdt_addr = le32_to_cpu(fdt_lo);
    mr80x_aarch32_handoff_kernel_entry = le32_to_cpu(kernel_lo);
    mr80x_aarch32_handoff_pending = true;

    info_report("mr80x: appsbl jump_kernel64() SMC intercepted (params at "
                "0x%" PRIx64 ") - kernel_entry=0x%" PRIx64 " fdt=0x%" PRIx64
                " - requesting AArch64 handoff reset",
                (uint64_t)params_addr,
                (uint64_t)mr80x_aarch32_handoff_kernel_entry,
                (uint64_t)mr80x_aarch32_handoff_fdt_addr);

    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static const MemoryRegionOps mr80x_handoff_trigger_ops = {
    .read = mr80x_handoff_trigger_read,
    .write = mr80x_handoff_trigger_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ============================================================
 * Linux's own SMC calls (PSCI - CPU_ON to boot the second CPU core the
 * DT declares, PSCI_VERSION/FEATURES probes) hit the exact same
 * MVBAR/SMC trampoline (above) appsbl's SCM_SVC_FUSE calls do - real
 * hardware would route both to the same Monitor-mode firmware too.
 * But unlike appsbl (which runs with u-boot's own, unusually
 * permissive single-1MiB-section-executable MMU setup, see the
 * MR80X_MVBAR_BASE comment), the kernel builds its *own* page tables
 * from scratch and, for reasons not fully root-caused (neither the
 * DT's `memory` node nor its `reserved-memory` carve-outs exclude
 * this physical address - confirmed by dumping and decompiling the
 * live in-RAM DTB via the QEMU monitor's `pmemsave` mid-boot), simply
 * doesn't map the trampoline's physical page at all - confirmed via
 * `-d int`: a Prefetch Abort with IFSR 0x5 (translation fault) at
 * exactly the trampoline's physical address, right as the kernel
 * (genuinely alive and running real code, not crashed - visible
 * setting up its own per-mode exception stacks around pc=0x8131252x
 * first) issues its first post-"Starting kernel..." `smc`.
 *
 * Fix: use QEMU's own *native* PSCI implementation
 * (target/arm/tcg/psci.c, the same C-code SMC interception the `virt`
 * machine type uses for SMP boot) instead of guest-visible trampoline
 * code for calls made after u-boot hands off - entirely sidesteps the
 * "is this physical page mapped by whichever page tables happen to be
 * active" question, since QEMU intercepts the `smc` *before* any
 * guest instruction fetch happens for it at all.
 *
 * Can't just enable `cpu->psci_conduit` unconditionally from machine
 * start, though: `arm_is_psci_call()` intercepts *every* `smc`
 * regardless of which function ID it carries (checked before the
 * instruction executes), so it would also swallow appsbl's own
 * Qualcomm-specific SCM_SVC_FUSE calls - QEMU's PSCI handler's
 * "unrecognized function ID" case returns r0=QEMU_PSCI_RET_NOT_SUPPORTED
 * (-1, kvm-consts.h), which appsbl's own scm.c `scm_remap_error()`
 * maps to `-EIO` (since -1 == SCM_ERROR, checked before
 * SCM_EOPNOTSUPP=-4 in that switch) - NOT `-EOPNOTSUPP`, which is
 * exactly the value do_bootipq() branches on (section 21) -
 * reintroducing the original silent-reset bug this session already
 * fixed once.
 *
 * So this is gated dynamically: a periodic QEMU-side timer (no guest
 * involvement at all, just watches CPU state) polls the CPU's PC, and
 * flips `psci_conduit` to SMC the first time PC lands outside
 * appsbl's own ~1MiB code footprint - a simple, image-independent
 * proxy for "u-boot is done, this has to be the kernel (or later)"
 * that doesn't require knowing any specific FIT image's load address
 * ahead of time. Reset back to DISABLED (own trampoline handles
 * everything again) on every machine reset, so the console `reset`
 * command's second boot cycle - which re-runs appsbl's own SCM_SVC_FUSE
 * calls - keeps working exactly as before this existed. */
#define MR80X_PSCI_WATCH_INTERVAL_NS (5 * SCALE_MS)

static QEMUTimer *mr80x_psci_watch_timer;

static void mr80x_psci_watch_tick(void *opaque)
{
    MR80XResetState *rs = opaque;
    target_ulong pc = rs->cpu->env.regs[15];

    if (pc < MR80X_APPSBL_ENTRY || pc >= MR80X_APPSBL_ENTRY + MiB) {
        rs->cpu->psci_conduit = QEMU_PSCI_CONDUIT_SMC;
        info_report("mr80x: pc=0x%lx is past appsbl's own code - "
                    "switching SMC calls to QEMU's native PSCI "
                    "handling (kernel CPU_ON/VERSION/etc, real appsbl "
                    "SCM calls are already done by now)",
                    (unsigned long)pc);
        return;
    }

    timer_mod(mr80x_psci_watch_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MR80X_PSCI_WATCH_INTERVAL_NS);
}

static void mr80x_reset(void *opaque)
{
    MR80XResetState *rs = opaque;
    CPUState *cs = CPU(rs->cpu);

    if (mr80x_aarch32_handoff_pending) {
        /* Option A: appsbl's own jump_kernel64() SMC already ran and
         * handed us the real kernel entry/fdt addresses - see the
         * comment block above mr80x_handoff_trigger_write(). Enter
         * AArch64 exactly like the isolated test path's
         * mr80x_aarch64_reset() does, *without* re-populating RAM: the
         * kernel Image + FDT appsbl's own FIT-loading logic already
         * placed there survive this reset untouched. */
        mr80x_aarch32_handoff_pending = false;

        set_feature(&rs->cpu->env, ARM_FEATURE_AARCH64);
        unset_feature(&rs->cpu->env, ARM_FEATURE_EL3);

        cpu_reset(cs);
        cpu_set_pc(cs, mr80x_aarch32_handoff_kernel_entry);
        rs->cpu->env.xregs[0] = mr80x_aarch32_handoff_fdt_addr;
        rs->cpu->psci_conduit = QEMU_PSCI_CONDUIT_SMC;

        info_report("mr80x: entering AArch64 kernel at 0x%" PRIx64
                    ", x0(fdt)=0x%" PRIx64,
                    (uint64_t)mr80x_aarch32_handoff_kernel_entry,
                    (uint64_t)mr80x_aarch32_handoff_fdt_addr);

        tb_flush(cs);
        return;
    }

    /* Normal appsbl (AArch32) boot - undo the handoff reset's feature
     * toggles every time so a plain console `reset` after a *previous*
     * handoff (or a QEMU restart with stale static state - it isn't,
     * these are per-process, but symmetry costs nothing) still lands
     * back in AArch32 with EL3 present, matching real cold-boot
     * behavior. Harmless/idempotent on the very first, implicit reset
     * too, since both bits already default this way at CPU realize. */
    unset_feature(&rs->cpu->env, ARM_FEATURE_AARCH64);
    set_feature(&rs->cpu->env, ARM_FEATURE_EL3);

    cpu_reset(cs);
    mr80x_populate_ram(rs->machine);
    cpu_set_pc(cs, MR80X_APPSBL_ENTRY);
    rs->cpu->env.cp15.mvbar = MR80X_MVBAR_BASE;
    rs->cpu->psci_conduit = QEMU_PSCI_CONDUIT_DISABLED;
    mr80x_kernel_handoff_match = 0;
    mr80x_kernel_handoff_triggered = false;

    if (!mr80x_psci_watch_timer) {
        mr80x_psci_watch_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                               mr80x_psci_watch_tick, rs);
    }
    timer_mod(mr80x_psci_watch_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MR80X_PSCI_WATCH_INTERVAL_NS);

    /* mr80x_populate_ram() pokes RAM (including the MVBAR trampoline)
     * via cpu_physical_memory_write() from board code, not via a
     * guest CPU store - on the *second and later* reset cycles, TCG
     * may still hold translation blocks compiled from the *previous*
     * cycle's content at these same physical addresses (translated
     * the first time this code ran as guest instructions). Force a
     * full flush so every fetch after this reset re-translates from
     * the RAM content we just wrote, instead of possibly executing a
     * stale cached translation of whatever used to be here. */
    tb_flush(cs);
}

/* Load the ELF stored in the real NAND's APPSBL partition. Qualcomm's
 * preceding PBL/SBL/QSEE stages normally parse this ELF and place its PT_LOAD
 * segments in RAM before entering it; those proprietary stages are outside
 * this machine's scope, so the board model performs their final handoff.
 *
 * QEMU's ELF loader only accepts a filename, not a slice of a larger file.
 * Copy the exact APPSBL partition to a private temporary file, let the normal
 * loader parse/register it as reset-restored ROM content, then unlink it. The
 * same FULL_FIRMWARE.bin remains attached to QPIC as NAND backing below. */
static void mr80x_load_appsbl_from_nand(const char *path)
{
    g_autofree uint8_t *image = g_malloc(MR80X_APPSBL_FLASH_SIZE);
    g_autofree char *tmp_path = NULL;
    g_autoptr(GError) tmp_error = NULL;
    struct stat st;
    size_t done = 0;
    int nand_fd;
    int tmp_fd;
    ssize_t sz;

    nand_fd = open(path, O_RDONLY);
    if (nand_fd < 0) {
        error_report("mr80x: could not open NAND image '%s': %s", path,
                     strerror(errno));
        exit(1);
    }
    if (fstat(nand_fd, &st) < 0) {
        error_report("mr80x: could not stat NAND image '%s': %s", path,
                     strerror(errno));
        close(nand_fd);
        exit(1);
    }
    if ((uint64_t)st.st_size < MR80X_APPSBL_FLASH_OFFSET +
                               MR80X_APPSBL_FLASH_SIZE) {
        error_report("mr80x: NAND image '%s' is too small for APPSBL "
                     "partition (need at least 0x%x bytes, got 0x%" PRIx64 ")",
                     path,
                     MR80X_APPSBL_FLASH_OFFSET + MR80X_APPSBL_FLASH_SIZE,
                     (uint64_t)st.st_size);
        close(nand_fd);
        exit(1);
    }

    while (done < MR80X_APPSBL_FLASH_SIZE) {
        ssize_t n = pread(nand_fd, image + done,
                          MR80X_APPSBL_FLASH_SIZE - done,
                          MR80X_APPSBL_FLASH_OFFSET + done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            error_report("mr80x: failed reading APPSBL partition from '%s': %s",
                         path, n < 0 ? strerror(errno) : "unexpected EOF");
            close(nand_fd);
            exit(1);
        }
        done += n;
    }
    close(nand_fd);

    if (memcmp(image, ELFMAG, SELFMAG) != 0) {
        error_report("mr80x: APPSBL partition at NAND offset 0x%x is not an ELF",
                     MR80X_APPSBL_FLASH_OFFSET);
        exit(1);
    }

    tmp_fd = g_file_open_tmp("mr80x-appsbl-XXXXXX", &tmp_path, &tmp_error);
    if (tmp_fd < 0) {
        error_report("mr80x: could not create temporary APPSBL file: %s",
                     tmp_error->message);
        exit(1);
    }
    done = 0;
    while (done < MR80X_APPSBL_FLASH_SIZE) {
        ssize_t n = write(tmp_fd, image + done,
                          MR80X_APPSBL_FLASH_SIZE - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            error_report("mr80x: failed writing temporary APPSBL file: %s",
                         n < 0 ? strerror(errno) : "short write");
            close(tmp_fd);
            unlink(tmp_path);
            exit(1);
        }
        done += n;
    }
    close(tmp_fd);

    sz = load_elf_as(tmp_path, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                     0, EM_ARM, 0, 0, &address_space_memory);
    unlink(tmp_path);
    if (sz < 0) {
        error_report("mr80x: could not load APPSBL ELF from NAND image '%s': %s",
                     path, load_elf_strerror(sz));
        exit(1);
    }

    info_report("mr80x: booting APPSBL from NAND '%s' partition "
                "0x%x..0x%x (%zd ELF bytes loaded)",
                path, MR80X_APPSBL_FLASH_OFFSET,
                MR80X_APPSBL_FLASH_OFFSET + MR80X_APPSBL_FLASH_SIZE - 1, sz);
}

/* ============================================================
 * Deliberately separate, isolated boot path: loads a raw AArch64
 * Linux kernel `Image` and its own devicetree blob directly, bypassing
 * appsbl entirely - the real IPQ5018 is a genuine AArch64-capable
 * Cortex-A53 (confirmed: OpenWrt's mature, real-hardware-tested
 * "qualcommax" target builds ARCH=aarch64/CPU_TYPE=cortex-a53
 * uniformly for this whole SoC family, and appsbl's own
 * arch/arm/cpu/armv7/qca/common/scm.c has a dedicated
 * `jump_kernel64()` using an SCM call to switch out of AArch32 right
 * before handing off to a 64-bit kernel - see BRINGUP-NOTES.md section
 * 28), but appsbl itself is, and stays, permanently AArch32 the whole
 * way through (no armv8 Qualcomm/IPQ5018 board port exists anywhere in
 * this project's vendor u-boot source - only generic upstream armv8
 * support for unrelated vendors). Actually emulating that AArch32->
 * AArch64 SMC-mediated handoff is real, separate work (tracked next);
 * this path exists to test - *before* investing in that - whether this
 * board's existing peripheral models (GIC, UART) are even compatible
 * with a real, modern, fully-sourced OpenWrt kernel for this exact
 * device at all, decoupled from whether the handoff mechanism works.
 * Enabled via MR80X_AARCH64_KERNEL (+ MR80X_AARCH64_DTB) - see
 * run.sh. Uses the OpenWrt-built *initramfs* image specifically
 * (kernel with an embedded rootfs) so it can reach a real userspace
 * shell without also needing working NAND/UBI. */
#define MR80X_AARCH64_KERNEL_BASE 0x41000000
#define MR80X_AARCH64_DTB_BASE    0x44000000

typedef struct MR80XAArch64ResetState {
    ARMCPU *cpu;
} MR80XAArch64ResetState;

static void mr80x_aarch64_reset(void *opaque)
{
    MR80XAArch64ResetState *rs = opaque;
    CPUState *cs = CPU(rs->cpu);

    cpu_reset(cs);
    cpu_set_pc(cs, MR80X_AARCH64_KERNEL_BASE);
    /* AArch64 Linux boot protocol: x0 = DTB physical address,
     * x1-x3 = 0 (already true after cpu_reset()). */
    rs->cpu->env.xregs[0] = MR80X_AARCH64_DTB_BASE;
    /* This test path's kernel makes real PSCI calls (CPU_ON for the
     * second core, VERSION/FEATURES probes) same as section 25/26 -
     * route them to QEMU's native PSCI handling from the very start,
     * since there's no appsbl SCM traffic in this isolated path to
     * conflict with (contrast mr80x_reset()'s watch-timer-gated
     * version, needed there specifically to not break appsbl). */
    rs->cpu->psci_conduit = QEMU_PSCI_CONDUIT_SMC;
}

static void mr80x_init_aarch64_test(MachineState *machine, ARMCPU *cpu,
                                     const char *kernel_path,
                                     const char *dtb_path)
{
    ssize_t sz;
    MR80XAArch64ResetState *rs;

    sz = load_image_targphys(kernel_path, MR80X_AARCH64_KERNEL_BASE,
                              MR80X_RAM_SIZE -
                              (MR80X_AARCH64_KERNEL_BASE - MR80X_RAM_BASE));
    if (sz < 0) {
        error_report("mr80x: could not load AArch64 kernel Image '%s'",
                      kernel_path);
        exit(1);
    }
    info_report("mr80x: loaded AArch64 kernel Image '%s' (%zd bytes) at "
                "0x%x", kernel_path, sz, MR80X_AARCH64_KERNEL_BASE);

    sz = load_image_targphys(dtb_path, MR80X_AARCH64_DTB_BASE,
                              MR80X_RAM_SIZE -
                              (MR80X_AARCH64_DTB_BASE - MR80X_RAM_BASE));
    if (sz < 0) {
        error_report("mr80x: could not load AArch64 DTB '%s'", dtb_path);
        exit(1);
    }
    info_report("mr80x: loaded AArch64 DTB '%s' (%zd bytes) at 0x%x",
                dtb_path, sz, MR80X_AARCH64_DTB_BASE);

    rs = g_new0(MR80XAArch64ResetState, 1);
    rs->cpu = cpu;
    qemu_register_reset(mr80x_aarch64_reset, rs);
}

static void mr80x_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj = object_new(machine->cpu_type);
    ARMCPU *cpu = ARM_CPU(cpuobj);

    mr80x_machine = machine;

    object_property_set_bool(cpuobj, "reset-hivecs", false, &error_fatal);
    /* Register the full AArch64 system-register set at realize time
     * regardless of the exact -cpu string given, same as run.sh's
     * isolated AArch64 test path already relies on explicitly - Option
     * A's handoff reset (mr80x_reset()) only *toggles* the
     * ARM_FEATURE_AARCH64 feature bit per-reset (has_el3 likewise, but
     * left at its default true here - see the has_el3 comment above
     * mr80x_handoff_trigger_write()), it doesn't create these
     * registers itself. */
    object_property_set_bool(cpuobj, "aarch64", true, &error_fatal);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    /* NOT unsetting ARM_FEATURE_AARCH64 back off here: the isolated
     * AArch64 test path below (mr80x_init_aarch64_test()) shares this
     * exact same CPU object and needs it to stay AArch64-capable from
     * its very first, implicit reset - it has no appsbl phase at all
     * and never runs mr80x_reset(). The normal appsbl path *does* need
     * to start in AArch32, but mr80x_reset() (its own reset handler,
     * registered below) already unsets the feature on every reset
     * including that implicit first one - see the comment block
     * above it. */

    /* ============================================================
     * GICv2 interrupt controller - required for the *kernel* (u-boot
     * never enables interrupts at all, its drivers are pure polling
     * loops, so this machine ran without any interrupt controller at
     * all until now - see BRINGUP-NOTES.md section 25). Without a
     * working GIC, Linux's own boot has no working timer tick (the
     * CPU's built-in architected timer, already correctly emulated by
     * QEMU's cortex-a7 model, delivers its expiry as a *PPI line into
     * the GIC* - with no GIC, that signal has nowhere to go) and no
     * way to receive interrupt-driven device IRQs (msm_serial, this
     * board's console driver on the kernel side, is interrupt-driven,
     * unlike u-boot's own bare-register-polling qca_uart.c).
     *
     * Real IPQ5018 hardware's GIC ("qcom,msm-qgic2" in the DT dumped
     * from live guest RAM - see section 25) is at GICD=0xb000000,
     * GICC=0xb002000 - a plain GICv2 layout QEMU's existing, reusable
     * `arm_gic` device (the same one hw/arm/highbank.c and others use)
     * models directly, no custom device needed.
     *
     * The DT's `timer { compatible = "arm,armv8-timer"; interrupts =
     * <1 2 0xf08 1 3 0xf08 1 4 0xf08 1 1 0xf08>; ... }` node gives the
     * PPI numbers this *specific* SoC's GIC wiring uses for the
     * architected timer's four interrupt lines (secure-phys=PPI2,
     * non-secure-phys=PPI3, virtual=PPI4, hyp=PPI1) - notably NOT the
     * generic ARM "virt" machine's convention (29/30/27/26,
     * include/hw/arm/bsa.h) QEMU's other boards use, so those generic
     * constants don't apply here; wired directly to the DT's own
     * numbers below instead. All four are wired regardless of which
     * one the kernel actually ends up using (harmless if unused) since
     * it isn't fully confirmed whether this boot chain ever flips the
     * CPU to Non-secure state at all (nothing in this minimal
     * emulator's boot path does that switch - see section 21/25). */
    DeviceState *gic = qdev_new(gic_class_name());
    qdev_prop_set_uint32(gic, "num-cpu", 1);
    qdev_prop_set_uint32(gic, "num-irq", MR80X_GIC_NUM_IRQ);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 0, MR80X_GIC_DIST_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 1, MR80X_GIC_CPU_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(gic), 0,
                        qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(gic), 1,
                        qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));
    {
        /* GICv2 (single CPU) gpio-in layout: indices
         * [0, num_irq-32) are the SPIs (index N = SPI N, i.e. INTID
         * N+32); indices [num_irq-32, num_irq) that follow are this
         * one CPU's private SGI/PPI bank, indexed directly by their
         * 0-31 local ID (== INTID for SGI/PPI, which live below 32).
         * See the "unnamed GPIO inputs" doc comment in
         * include/hw/intc/arm_gic.h. */
        unsigned priv_base = MR80X_GIC_NUM_IRQ - 32;

        qdev_connect_gpio_out(DEVICE(cpu), GTIMER_SEC,
                               qdev_get_gpio_in(gic, priv_base + 18));
        qdev_connect_gpio_out(DEVICE(cpu), GTIMER_PHYS,
                               qdev_get_gpio_in(gic, priv_base + 19));
        qdev_connect_gpio_out(DEVICE(cpu), GTIMER_VIRT,
                               qdev_get_gpio_in(gic, priv_base + 20));
        qdev_connect_gpio_out(DEVICE(cpu), GTIMER_HYP,
                               qdev_get_gpio_in(gic, priv_base + 17));
    }

    memory_region_add_subregion(sysmem, MR80X_RAM_BASE, machine->ram);

    const char *aarch64_kernel = getenv("MR80X_AARCH64_KERNEL");

    if (aarch64_kernel) {
        const char *aarch64_dtb = getenv("MR80X_AARCH64_DTB");

        if (!aarch64_dtb) {
            error_report("mr80x: MR80X_AARCH64_KERNEL needs "
                         "MR80X_AARCH64_DTB too");
            exit(1);
        }
        mr80x_init_aarch64_test(machine, cpu, aarch64_kernel, aarch64_dtb);
        goto peripherals;
    }

    if (!machine->kernel_filename && !getenv("MR80X_NAND_IMAGE")) {
        error_report("mr80x: provide -kernel for a development APPSBL override "
                     "or MR80X_NAND_IMAGE to boot APPSBL from the full flash");
        exit(1);
    }

    /* SMEM fakes and the MVBAR trampoline live in mr80x_populate_ram()
     * now, called from mr80x_reset() - which QEMU invokes once
     * automatically right after this function returns (same timing as
     * before) and again on every subsequent guest-triggered reset, so
     * "first boot" and "every reset after" go through the exact same
     * real-RAM-content setup. See the comment block above
     * mr80x_populate_ram().
     *
     * Registered *before* the ELF/image load below on purpose: QEMU's
     * own loader functions register their own internal rom_reset()
     * reset handler as a side effect of loading, and reset handlers
     * fire in registration order - putting ours first guarantees our
     * RAM clear (inside mr80x_populate_ram()) always runs, then QEMU's
     * rom_reset() runs right after and restores the loaded image on
     * top of that clean slate, every single reset including the
     * implicit first one. Reversing this order would let our clear
     * wipe out the image *after* QEMU had just restored it. */
    {
        MR80XResetState *rs = g_new0(MR80XResetState, 1);
        rs->cpu = cpu;
        rs->machine = machine;
        qemu_register_reset(mr80x_reset, rs);
    }

    if (machine->kernel_filename) {
        ssize_t sz = load_elf_as(machine->kernel_filename, NULL, NULL, NULL,
                                  NULL, NULL, NULL, NULL, 0, EM_ARM, 0, 0,
                                  &address_space_memory);
        if (sz < 0) {
            /* Not an ELF (e.g. a raw appsbl.bin) - load it as a flat
             * image at its known link address instead. */
            sz = load_image_targphys(machine->kernel_filename,
                                      MR80X_APPSBL_ENTRY,
                                      MR80X_RAM_SIZE -
                                      (MR80X_APPSBL_ENTRY - MR80X_RAM_BASE));
            if (sz < 0) {
                error_report("mr80x: could not load '%s' as ELF or raw image",
                              machine->kernel_filename);
                exit(1);
            }
        }
        info_report("mr80x: booting development APPSBL override '%s'",
                    machine->kernel_filename);
    } else {
        mr80x_load_appsbl_from_nand(getenv("MR80X_NAND_IMAGE"));
    }

peripherals:

    /* GCC clock controller stub */
    MR80XGccState *gcc = g_new0(MR80XGccState, 1);
    memory_region_init_io(&gcc->iomem, NULL, &mr80x_gcc_ops, gcc,
                           "mr80x.gcc", MR80X_GCC_SIZE);
    memory_region_add_subregion(sysmem, MR80X_GCC_BASE, &gcc->iomem);

    /* Generic timer counter-view registers - see the
     * MR80X_TIME_SCALE_DEFAULT comment above for why this runs slower
     * than real time by default. */
    MR80XTimerState *timer = g_new0(MR80XTimerState, 1);
    timer->scale = MR80X_TIME_SCALE_DEFAULT;
    {
        const char *scale_env = getenv("MR80X_TIME_SCALE");
        if (scale_env && atoi(scale_env) > 0) {
            timer->scale = atoi(scale_env);
        }
    }
    memory_region_init_io(&timer->iomem, NULL, &mr80x_timer_ops, timer,
                           "mr80x.timer", MR80X_TIMER_SIZE);
    memory_region_add_subregion(sysmem, MR80X_TIMER_BASE, &timer->iomem);

    /* GCNT_PSHOLD - see the comment block above */
    {
        MemoryRegion *pshold = g_new0(MemoryRegion, 1);
        memory_region_init_io(pshold, NULL, &mr80x_pshold_ops, NULL,
                               "mr80x.pshold", MR80X_PSHOLD_SIZE);
        memory_region_add_subregion(sysmem, MR80X_PSHOLD_BASE, pshold);
    }

    /* Option A AArch32->AArch64 handoff trigger - see the comment
     * block above mr80x_handoff_trigger_write(). Harmless to register
     * unconditionally (including for the isolated AArch64 test path,
     * which never issues appsbl's own SMC traffic and so never writes
     * here). */
    {
        MemoryRegion *handoff = g_new0(MemoryRegion, 1);
        memory_region_init_io(handoff, NULL, &mr80x_handoff_trigger_ops, NULL,
                               "mr80x.handoff-trigger",
                               MR80X_HANDOFF_TRIGGER_SIZE);
        memory_region_add_subregion(sysmem, MR80X_HANDOFF_TRIGGER_BASE,
                                     handoff);
    }

    /* MDIO controller - see the MR80X_MDIO_BASE comment block */
    MR80XMdioState *mdio = g_new0(MR80XMdioState, 1);
    memory_region_init_io(&mdio->iomem, NULL, &mr80x_mdio_ops, mdio,
                           "mr80x.mdio", MR80X_MDIO_SIZE);
    memory_region_add_subregion(sysmem, MR80X_MDIO_BASE, &mdio->iomem);

    /* UART */
    MR80XUartState *uart = g_new0(MR80XUartState, 1);
    memory_region_init_io(&uart->iomem, NULL, &mr80x_uart_ops, uart,
                           "mr80x.uart", MR80X_UART_SIZE);
    memory_region_add_subregion(sysmem, MR80X_UART_BASE, &uart->iomem);
    qemu_chr_fe_init(&uart->chr, serial_hd(0), &error_abort);
    qemu_chr_fe_set_handlers(&uart->chr, mr80x_uart_can_rx, mr80x_uart_rx,
                              mr80x_uart_event, NULL, uart, NULL, true);
    uart->irq = qdev_get_gpio_in(gic, MR80X_UART_IRQ);

    /* MR80X_STOP_AUTOBOOT - a guaranteed, timing-independent way to
     * drop into the interactive u-boot console, for when the real
     * "Hit any key to stop autoboot" countdown isn't reliably
     * catchable: even with the timer made real-time-accurate (see the
     * MR80X_TIME_SCALE_DEFAULT comment above) and the BQL-blocking
     * g_usleep() removed from UART TX, actually landing a keypress
     * inside a short (CONFIG_BOOTDELAY=1s on this TP-Link build)
     * window is still at the mercy of terminal -> docker engine API
     * -> container -> QEMU latency stacking with ordinary human
     * reaction time - confirmed unreliable by direct user testing
     * even after those fixes. Pre-seeding the UART's own RX buffer
     * with a byte before the guest ever runs means
     * abortboot_normal()'s very first tstc() poll (which happens
     * immediately on entering its wait loop, no real delay needed)
     * sees it and aborts autoboot instantly - zero timing dependency,
     * unlike waiting for a real keypress to race the countdown. */
    if (getenv("MR80X_STOP_AUTOBOOT")) {
        static const uint8_t stop_key[] = { ' ' };
        mr80x_uart_rx(uart, stop_key, sizeof(stop_key));
    }

    /* Everything else touched during early boot that we haven't modeled
     * yet: catch, log, return 0. Widen/replace piecemeal as the boot
     * log shows what's actually needed next (QPIC NAND, DesignWare
     * Ethernet, any other GCC/TCSR/TLMM regions board_nand_init() or
     * board_eth_init() reach for). */
    mr80x_add_unimp_region(sysmem, "mr80x.unimp-0x01900000",
                            0x01900000, 16 * MiB);

    /* QPIC NAND - see the MR80X_NAND_BASE comment block above */
    MR80XNandState *nand_state = g_new0(MR80XNandState, 1);
    mr80x_nand_reset(nand_state);
    qemu_register_reset(mr80x_nand_reset, nand_state);
    /* Real page *data* backing for MR80X_BAM_DATA_PRODUCER_PIPE - a
     * raw full-flash dump (e.g. FULL_FIRMWARE.bin, BRINGUP-NOTES.md
     * section 4b), mmap'd read-only. Optional: without it, page reads
     * just return 0xFF (same as before this existed) - readenv() and
     * kernel/rootfs loading will still fail, but nothing crashes. */
    {
        const char *path = getenv("MR80X_NAND_IMAGE");
        if (path) {
            int fd = open(path, O_RDONLY);
            struct stat st;
            if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0) {
                void *m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (m != MAP_FAILED) {
                    nand_state->image_data = m;
                    nand_state->image_size = st.st_size;
                    info_report("mr80x: NAND backed by '%s' (%" PRIu64
                                 " bytes)", path, (uint64_t)st.st_size);
                } else {
                    warn_report("mr80x: mmap('%s') failed: %s", path,
                                strerror(errno));
                }
            } else {
                warn_report("mr80x: could not open MR80X_NAND_IMAGE '%s': %s",
                            path, strerror(errno));
            }
            if (fd >= 0) {
                close(fd);
            }
        }
    }
    {
        MemoryRegion *nand = g_new0(MemoryRegion, 1);
        memory_region_init_io(nand, NULL, &mr80x_nand_ops, nand_state,
                               "mr80x.nand", MR80X_NAND_SIZE);
        memory_region_add_subregion(sysmem, MR80X_NAND_BASE, nand);
    }
    /* remainder of the 1MiB NAND-adjacent range not yet modeled */
    mr80x_add_unimp_region(sysmem, "mr80x.unimp-nand-rest-0x79C0000",
                            0x079C0000, 1 * MiB - MR80X_NAND_SIZE);

    /* QPIC BAM - see the MR80X_BAM_BASE comment block above. Shares
     * nand_state so cmd_element-driven register accesses land in the
     * same backing store as direct MMIO to the NAND region. */
    {
        MR80XBamState *bam = g_new0(MR80XBamState, 1);
        bam->nand = nand_state;
        bam->irq = qdev_get_gpio_in(gic, MR80X_BAM_IRQ);
        qemu_register_reset(mr80x_bam_reset, bam);
        memory_region_init_io(&bam->iomem, NULL, &mr80x_bam_ops, bam,
                               "mr80x.bam", MR80X_BAM_SIZE);
        memory_region_add_subregion(sysmem, MR80X_BAM_BASE, &bam->iomem);
    }
    mr80x_add_unimp_region(sysmem, "mr80x.unimp-gmac2-0x39D00000",
                            0x39D00000, 1 * MiB);
    {
        MemoryRegion *tlmm = g_new0(MemoryRegion, 1);
        bool *recovery = g_new0(bool, 1);
        const char *env = getenv("MR80X_RECOVERY");
        *recovery = (env && env[0] && strcmp(env, "0") != 0);
        if (*recovery) {
            info_report("mr80x: MR80X_RECOVERY set - GPIO14 (reset button) "
                         "will read as held down, same as real hardware's "
                         "firmware recovery mode entry");
        }
        memory_region_init_io(tlmm, NULL, &mr80x_tlmm_ops, recovery,
                               "mr80x.tlmm", 1 * MiB);
        memory_region_add_subregion(sysmem, 0x01000000, tlmm);
    }

    /* GMAC1 - real device (see mr80x_gmac_realize and friends above).
     * gmac1_cfg's "base" in every DTB checked, including our target
     * ipq5018-emulation.dts, is 0x39C00000. Wired to whatever -netdev
     * the user supplies (or QEMU's default usermode/slirp netdev if
     * none is given) via qemu_configure_nic_device, same as any other
     * board's onboard NIC. */
    {
        DeviceState *gmac1 = qdev_new(TYPE_MR80X_GMAC);
        qemu_configure_nic_device(gmac1, true, NULL);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(gmac1), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(gmac1), 0, 0x39C00000);
    }
}

static void mr80x_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "QCA IPQ5018 / Mercusys MR80X v5 research machine "
               "(minimal - see BRINGUP-NOTES.md)";
    mc->init = mr80x_init;
    /* cortex-a53, not -a7: real IPQ5018 hardware is a genuine Cortex-A53
     * the whole way through, including while appsbl itself runs in
     * AArch32 - see the AArch64 test path comment above
     * mr80x_aarch64_reset() and BRINGUP-NOTES.md section 28. Needed
     * unconditionally now (not just for the isolated AArch64 test path)
     * so mr80x_init()'s own "aarch64" property set below has a CPU class
     * that actually registers AArch64 system registers at realize -
     * Option A's handoff reset (mr80x_reset()) dynamically toggles the
     * ARM_FEATURE_AARCH64 feature *bit* per-reset, but that only works
     * if those registers already exist in cp_regs from realize time. */
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->default_ram_size = MR80X_RAM_SIZE;
    mc->default_ram_id = "mr80x.ram";
    mc->ignore_memory_transaction_failures = true;
    mc->max_cpus = 1;
}

static const TypeInfo mr80x_machine_typeinfo = {
    .name = MACHINE_TYPE_NAME("mr80x"),
    .parent = TYPE_MACHINE,
    .class_init = mr80x_machine_class_init,
};

static void mr80x_machine_register_types(void)
{
    type_register_static(&mr80x_machine_typeinfo);
}
type_init(mr80x_machine_register_types);
