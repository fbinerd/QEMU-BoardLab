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
#include "chardev/char-fe.h"
#include "cpu.h"
#include "qom/object.h"
#include "elf.h"

/* ---- memory map (appsbl/CLEAN_ROOM_STATUS.md, ipq5018.h) ---- */

#define MR80X_RAM_BASE      0x40000000
#define MR80X_RAM_SIZE      (512 * MiB)
#define MR80X_APPSBL_ENTRY  0x4A920000

#define MR80X_UART_BASE     0x078AF000
#define MR80X_UART_SIZE     0x1000

#define MR80X_GCC_BASE      0x01800000
#define MR80X_GCC_SIZE      (256 * KiB)

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

    if (offset == GCC_BLSP1_UART1_APPS_CMD_RCGR) {
        val &= ~UART1_CMD_RCGR_UPDATE_BIT;
    }
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
 * Machine init
 * ============================================================ */

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
    mr80x_add_unimp_region(sysmem, "mr80x.unimp-gmac1-0x39C00000",
                            0x39C00000, 1 * MiB);
    mr80x_add_unimp_region(sysmem, "mr80x.unimp-gmac2-0x39D00000",
                            0x39D00000, 1 * MiB);
    mr80x_add_unimp_region(sysmem, "mr80x.unimp-tlmm-0x01000000",
                            0x01000000, 1 * MiB);
}

static void mr80x_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "QCA IPQ5018 / Mercusys MR80X v5 research machine "
               "(minimal - see BRINGUP-NOTES.md)";
    mc->init = mr80x_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a15");
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
