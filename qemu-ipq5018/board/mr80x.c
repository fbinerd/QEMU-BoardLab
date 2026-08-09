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
 * back. */

static uint64_t mr80x_tlmm_read(void *opaque, hwaddr offset, unsigned size)
{
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
#define GMAC_DMA_TXPOLLDEMAND    (GMAC_DMA_OFFSET + 0x04)
#define GMAC_DMA_RXPOLLDEMAND    (GMAC_DMA_OFFSET + 0x08)
#define GMAC_DMA_RXBASEADDR      (GMAC_DMA_OFFSET + 0x0C)
#define GMAC_DMA_TXBASEADDR      (GMAC_DMA_OFFSET + 0x10)

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

static void mr80x_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu_set_pc(cs, MR80X_APPSBL_ENTRY);
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
        v = cpu_to_le32(1); /* len - one partition: 0:APPSBLENV */
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
    mr80x_add_unimp_region(sysmem, "mr80x.unimp-nand-0x79B0000",
                            0x079B0000, 1 * MiB);
    mr80x_add_unimp_region(sysmem, "mr80x.unimp-gmac2-0x39D00000",
                            0x39D00000, 1 * MiB);
    {
        MemoryRegion *tlmm = g_new0(MemoryRegion, 1);
        memory_region_init_io(tlmm, NULL, &mr80x_tlmm_ops, NULL,
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
