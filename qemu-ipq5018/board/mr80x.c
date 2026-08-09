/*
 * QCA IPQ5018 / Mercusys MR80X v5 research machine model
 *
 * Deliberately minimal and incomplete: enough to boot the REAL appsbl
 * (u-boot 2016.01) ELF from the sibling `appsbl` clean-room project far
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
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "chardev/char-fe.h"
#include "cpu.h"
#include "qom/object.h"
#include "elf.h"
#include "net/net.h"
#include "hw/qdev-properties.h"

/* ---- memory map (appsbl/CLEAN_ROOM_STATUS.md, ipq5018.h) ---- */

#define MR80X_RAM_BASE      0x40000000
#define MR80X_RAM_SIZE      (512 * MiB)
#define MR80X_APPSBL_ENTRY  0x4A920000

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
#define UART_MISR   0x10   /* alias, unused by this driver's read path */
#define UART_IMR    0x14
#define UART_ISR    0x14   /* alias, see UART_IMR */
#define UART_IPR    0x18
#define UART_TFWR   0x1C
#define UART_RFWR   0x20
#define UART_HCR    0x24
#define UART_DMRX   0x34
#define UART_IRDA   0x38
#define UART_DMEN   0x3C
#define UART_NCHAR  0x40
#define UART_TF0    0x70
#define UART_BADR   0x44

#define UART_SR_RXRDY  (1 << 0)
#define UART_SR_TXRDY  (1 << 2)
#define UART_SR_TXEMT  (1 << 3)

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
 * __udelay() spins on. Not wired to a real clock: each read of the LO
 * half just advances a free-running counter by a large step, so any
 * delay-loop-until-elapsed check on real silicon terminates almost
 * immediately here too - we don't need wall-clock-accurate delays for
 * this to boot correctly, just forward progress.
 * ============================================================ */

#define MR80X_TIMER_BASE 0x4A2000
#define MR80X_TIMER_SIZE 0x8
#define MR80X_TIMER_STEP 0x100000

typedef struct MR80XTimerState {
    MemoryRegion iomem;
    uint64_t counter;
} MR80XTimerState;

static uint64_t mr80x_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    MR80XTimerState *s = opaque;

    if (offset == 0x0) {
        s->counter += MR80X_TIMER_STEP;
        return (uint32_t)s->counter;
    } else if (offset == 0x4) {
        return (uint32_t)(s->counter >> 32);
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
#define NAND_EXEC_CMD_OFF         0x0010
#define NAND_FLASH_STATUS_OFF     0x0014
#define NAND_READ_ID_OFF          0x0040
#define NAND_DEV_CMD_VLD_V1_5_20_OFF 0x70AC

#define NAND_CMD_FETCH_ID 0x0B

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
} MR80XNandState;

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
 * Only the cmd pipe (index/pipe_num 2) actually executes cmd_elements;
 * the data pipes (0,1, for real page read/write DMA) are latched but
 * not yet driven - real flash *data* (kernel/rootfs) isn't reachable
 * yet, only device identification, which is what unblocks the next
 * boot step.
 * ============================================================ */

#define MR80X_BAM_BASE 0x07984000
#define MR80X_BAM_SIZE 0x20000
#define MR80X_BAM_NUM_PIPES 4
#define MR80X_BAM_CMD_PIPE 2
#define MR80X_BAM_EE 0

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
} MR80XBamPipe;

typedef struct MR80XBamState {
    MemoryRegion iomem;
    MR80XNandState *nand;
    MR80XBamPipe pipe[MR80X_BAM_NUM_PIPES];
    uint32_t generic_regs[MR80X_BAM_SIZE / 4];
} MR80XBamState;

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

        /* The "kick": a new descriptor was appended right before the
         * new write-offset given here. We only track the cmd pipe -
         * the driver always adds exactly one descriptor per kick in
         * this code path, so the newest descriptor sits 8 bytes
         * before the new offset in the (power-of-2-sized) ring. */
        if (n == MR80X_BAM_CMD_PIPE && s->pipe[n].fifo_base) {
            uint32_t new_off = (uint32_t)value;
            uint32_t desc_off = (new_off - 8) & 0xFFFF;
            mr80x_bam_process_cmd_desc(s, s->pipe[n].fifo_base + desc_off);
        }
        s->pipe[n].irq_stts |= BAM_P_PRCSD_DESC_MASK;
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
} MR80XUartState;

static bool mr80x_uart_rx_empty(MR80XUartState *s)
{
    return s->rx_head == s->rx_tail;
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
            return c;
        }
        return 0;
    default:
        return 0;
    }
}

static void mr80x_uart_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MR80XUartState *s = opaque;

    switch (offset) {
    case UART_TF0:
    case UART_TF0 + 4:
    case UART_TF0 + 8:
    case UART_TF0 + 12: {
        /* Real HW packs up to 4 chars per 32-bit TF write for wide
         * transfers, but qca_uart.c's putc path (single_char, see
         * ipq_serial_putc) always writes one character as a byte at
         * TF0 preceded by NO_CHARS_FOR_TX=1 - handle the common case
         * directly, extend if a wider write shows up in practice. */
        uint8_t c = (uint8_t)value;
        qemu_chr_fe_write_all(&s->chr, &c, 1);
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

/* Lives 4KiB below the appsbl entry point, inside the SAME 1MiB
 * section as CONFIG_SYS_TEXT_BASE (0x4A920000) - NOT some arbitrary
 * "unused" RAM address. Three earlier attempts elsewhere in RAM
 * (0x00080000; top of the full 512MiB QEMU is given; top of the
 * 256MiB the "DRAM: 256 MiB" boot message reports) all still faulted
 * instruction fetch with IFSR 0xd (Permission fault), confirmed via
 * `-d int` exception tracing. Root cause, found in
 * arch/arm/lib/cache-cp15.c's dram_bank_mmu_setup()
 * (CONFIG_IPQ_NO_RELOC path, which ipq5018.h enables): u-boot's own
 * static page table first marks the *entire* 4GB address space
 * SHARED_DEVICE (execute-unfriendly), then for DRAM specifically
 * marks only the one 1MiB section containing CONFIG_SYS_TEXT_BASE
 * with the exec-friendly UBOOT_CACHE_SETUP attribute - every other
 * MiB, including ones that are perfectly valid backing RAM in QEMU,
 * keeps the earlier device-like attribute and cannot be fetched from.
 * This address is guaranteed to fall in that one safe section
 * regardless of exactly how large u-boot believes DRAM to be. */
#define MR80X_MVBAR_BASE (MR80X_APPSBL_ENTRY - 0x1000)
#define MR80X_MVBAR_SIZE 0x20
#define MR80X_MVBAR_SMC_OFF 0x08

static void mr80x_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu_set_pc(cs, MR80X_APPSBL_ENTRY);
    cpu->env.cp15.mvbar = MR80X_MVBAR_BASE;
}

static void mr80x_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj = object_new(machine->cpu_type);
    ARMCPU *cpu = ARM_CPU(cpuobj);

    object_property_set_bool(cpuobj, "reset-hivecs", false, &error_fatal);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    memory_region_add_subregion(sysmem, MR80X_RAM_BASE, machine->ram);

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

    /* SMEM_AARM_PARTITION_TABLE - see the comment by
     * MR80X_SMEM_PTABLE_TYPE above. Rest of the 912-byte struct
     * (len=0 partitions) is left as already-zeroed fresh RAM. */
    {
        uint32_t v;
        hwaddr entry = MR80X_SMEM_BASE + MR80X_SMEM_ALLOC_INFO_OFF +
                        MR80X_SMEM_PTABLE_TYPE * 16;
        hwaddr data = MR80X_SMEM_BASE + MR80X_SMEM_PTABLE_DATA_OFF;

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
        v = cpu_to_le32(2); /* len - 0:APPSBLENV and rootfs */
        cpu_physical_memory_write(data + 12, &v, 4);

        /* struct smem_ptn { char name[16]; u32 start; u32 size; u32 attr; }
         * (packed, 28 bytes) - board_init() hard-requires finding
         * "0:APPSBLENV" via smem_getpart() (offset/size returned in
         * units of flash_block_size, 0x20000 - see
         * MR80X_SMEM_FLASH_BLOCK_SIZE_TYPE above) or it aborts boot
         * the same fatal way ptable itself did. Real offset/size
         * (0x300000/0x80000) from the actual flash dump - see
         * BRINGUP-NOTES.md section 4b's partition table. */
        {
            static const char name[16] = "0:APPSBLENV";
            hwaddr part = data + 16;
            cpu_physical_memory_write(part + 0, name, 16);
            v = cpu_to_le32(0x300000 / 0x20000); /* start, in blocks */
            cpu_physical_memory_write(part + 16, &v, 4);
            v = cpu_to_le32(0x80000 / 0x20000); /* size, in blocks */
            cpu_physical_memory_write(part + 20, &v, 4);
            v = cpu_to_le32(0);
            cpu_physical_memory_write(part + 24, &v, 4); /* attr */
        }

        /* "rootfs" (no "0:" prefix, unlike APPSBLENV - matches the real
         * flash dump exactly, see BRINGUP-NOTES.md section 4b partition
         * #11) - needed for nm_upgradeFirmware()'s "does this upload fit"
         * size check (lib/nvrammanager/nm_fwup.c) to find a nonzero
         * rootfs_flash_size instead of always rejecting every upload
         * with "Bad file size: ... flash: 0". Real offset/size
         * (0x640000/0x2A00000) from the actual flash dump. */
        {
            static const char name[16] = "rootfs";
            hwaddr part = data + 16 + 28;
            cpu_physical_memory_write(part + 0, name, 16);
            v = cpu_to_le32(0x640000 / 0x20000); /* start, in blocks */
            cpu_physical_memory_write(part + 16, &v, 4);
            v = cpu_to_le32(0x2A00000 / 0x20000); /* size, in blocks */
            cpu_physical_memory_write(part + 20, &v, 4);
            v = cpu_to_le32(0);
            cpu_physical_memory_write(part + 24, &v, 4); /* attr */
        }
    }

    if (!machine->kernel_filename) {
        error_report("mr80x: use -kernel to load appsbl.unpadded.elf "
                      "(or an -kernel-compatible raw appsbl.bin via "
                      "-device loader,file=...,addr=0x4A920000 instead)");
        exit(1);
    }

    ssize_t sz = load_elf_as(machine->kernel_filename, NULL, NULL, NULL,
                              NULL, NULL, NULL, NULL, 0, EM_ARM, 0, 0,
                              &address_space_memory);
    if (sz < 0) {
        /* Not an ELF (e.g. a raw appsbl.bin) - load it as a flat image
         * at its known link address instead. */
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

    /* Monitor-mode SMC trampoline - see the MR80X_MVBAR_BASE comment
     * block above mr80x_reset(). Written after the ELF/image load
     * above so nothing overwrites it. */
    {
        static const uint8_t trampoline[] = {
            0x03, 0x00, 0xE0, 0xE3, /* mvn  r0, #3   (little-endian) */
            0x0E, 0xF0, 0xB0, 0xE1, /* movs pc, lr                  */
        };
        cpu_physical_memory_write(MR80X_MVBAR_BASE + MR80X_MVBAR_SMC_OFF,
                                   trampoline, sizeof(trampoline));
    }

    qemu_register_reset(mr80x_reset, cpu);

    /* GCC clock controller stub */
    MR80XGccState *gcc = g_new0(MR80XGccState, 1);
    memory_region_init_io(&gcc->iomem, NULL, &mr80x_gcc_ops, gcc,
                           "mr80x.gcc", MR80X_GCC_SIZE);
    memory_region_add_subregion(sysmem, MR80X_GCC_BASE, &gcc->iomem);

    /* Generic timer counter-view registers */
    MR80XTimerState *timer = g_new0(MR80XTimerState, 1);
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

    /* Everything else touched during early boot that we haven't modeled
     * yet: catch, log, return 0. Widen/replace piecemeal as the boot
     * log shows what's actually needed next (QPIC NAND, DesignWare
     * Ethernet, any other GCC/TCSR/TLMM regions board_nand_init() or
     * board_eth_init() reach for). */
    mr80x_add_unimp_region(sysmem, "mr80x.unimp-0x01900000",
                            0x01900000, 16 * MiB);

    /* QPIC NAND - see the MR80X_NAND_BASE comment block above */
    MR80XNandState *nand_state = g_new0(MR80XNandState, 1);
    nand_state->regs[NAND_VERSION_OFF / 4] = 0x20000000u;
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
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
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
