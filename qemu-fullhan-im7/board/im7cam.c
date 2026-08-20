/*
 * Fullhan `im7` (Imou IPC-S21F camera) QEMU machine model - research/bringup
 *
 * Deliberately minimal - a first skeleton, not a working boot yet. See
 * BRINGUP-NOTES.md in this directory for the full reasoning and the
 * evidence (or explicit lack of it) behind every address below. Unlike
 * qemu-ipq5018/board/mr80x.c, there's no vendor GPL source to transcribe
 * from here - every address is either found by disassembling our own
 * extracted U-Boot binary, or borrowed from an independent RE of a
 * *sibling* Fullhan chip and cross-checked against our own binary. See
 * BRINGUP-NOTES.md sections 1-3 for exactly which is which and how
 * confident each one is.
 *
 * Isolated from qemu-ipq5018/board/mr80x.c on purpose, per project
 * instruction: own file, own Kconfig symbol (CONFIG_IM7CAM), own vendor
 * QEMU copy/Dockerfile, own machine name (-M im7cam), and a plain
 * arm-softmmu target instead of aarch64-softmmu (this device is confirmed
 * 32-bit ARM only - Linux zImage, no arm64 kernel anywhere in the dump).
 * The only thing "reused" from mr80x.c is the generic catch-all
 * logging-stub idiom below (im7cam_unimp_*) - it's plain QEMU
 * MemoryRegionOps boilerplate, not IPQ5018-specific, reimplemented here
 * under its own name rather than shared, so nothing links the two boards
 * together.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "chardev/char-fe.h"
#include "cpu.h"
#include "qom/object.h"

/* ---- memory map - see BRINGUP-NOTES.md sections 1-3 ---- */

/* Section 1: the image's own second header (offset 0x160-0x17B) repeats
 * the word 0xA0800000 twice (load address + entry point). Corroborated by
 * pointer literals inside the code (section 2's disassembly) landing in
 * the 0xA080xxxx-0xA083xxxx range, consistent with this base plus normal
 * .bss/heap growth. Confirmed by live disassembly at 0x2000 (section 4):
 * the whole 320 KB file loads here unmodified - 0x2000 is a real, classic
 * ARM exception vector table (b reset; 7x ldr pc,[pc,#20]; 0x12345678
 * magic), and reset code at 0x2054 does a completely standard U-Boot
 * start.S sequence (CPSR mode switch, VBAR write - the exact instruction
 * that faulted on arm926, see section 4 attempt 2 - cache/TLB invalidate,
 * SCTLR/MMU enable, BSS clear, jump to a C entry at 0xa0800504). */
#define IM7CAM_IMAGE_LOAD_ADDR 0xA0800000

/* NOT the same as the image load address above, on purpose - this was
 * this skeleton's first real bug (section 4): the reset code computes its
 * initial SP as *(0x2398) - 0x480000 - 0x80 = 0xA0800000 - 0x480080 =
 * 0xA037FF80 (0x2398's value, 0xA0800000, confirmed by reading the file
 * directly - not a relocation issue, the pointer is already a plain,
 * correct absolute address). That's ~4.5 MB *below* the image's own load
 * address - normal (stack conventionally grows down from just below
 * where code+BSS end, into lower memory the image itself doesn't
 * occupy), but fatal for a machine that only mapped RAM starting at the
 * image's load address: every push happened to now-silently-discarded
 * unmapped memory (ignore_memory_transaction_failures=true), corrupting
 * execution with no error until the CPU eventually wandered off via a
 * bad computed jump (observed via gdbstub: pc landed at 0xa5ec0270,
 * nowhere near any address this file ever computed on purpose).
 * 0xA0000000 is a round, conservative choice comfortably below the
 * computed SP - not derived from any real evidence *for that specific
 * value*, just "clearly far enough below 0xA037FF80". */
#define IM7CAM_RAM_BASE   0xA0000000
/* Needs to cover [0xA0000000, past 0xA08D0840] (BSS end, section 4) with
 * real margin - not confirmed as the chip's real DRAM size, just picked
 * generously now that the base moved down by 8 MB. */
#define IM7CAM_RAM_SIZE   (64 * MiB)

/* Section 2: 0xF0700000 appears in our own U-Boot binary's literal pool
 * (file offset 0xaae8) at a device/struct-init call site, AND matches an
 * independent Ghidra-based RE of a sibling Fullhan chip (FH8852V201,
 * github.com/pavliha/fh8852v201-dump, OpenIPC project) which lists that
 * same address as UART0. Two independent signals agreeing - the strongest
 * evidence in this file - but the in-block register layout (TX data
 * offset, TX-ready bit position, RX path) started out NOT confirmed - it
 * was wired as a logging stub for the first several boot attempts (like
 * every other peripheral here), specifically *to* read the access trace
 * back out and reverse the protocol from real guest behavior. That
 * worked (section 4): the trace showed a byte written to offset 0x0 right
 * after a poll loop reading offset 0x7c until bit 0x2 (2) is set -
 * confirmed by cross-referencing the exact byte value written ('A',
 * 0x41) against section 2's disassembly of the device-init call site,
 * which primed a struct with that exact byte. TX only (no RX modeled -
 * nothing's exercised that path yet); other offsets seen in the trace
 * (0x4, 0x8, 0xc - written 0/0x9/0x80/7 etc., presumably baud/line
 * control) are accepted and logged but not modeled for real yet. */
#define IM7CAM_UART_BASE      0xF0700000
#define IM7CAM_UART_SIZE      0x1000
#define IM7CAM_UART_REG_TX     0x00
#define IM7CAM_UART_REG_STATUS 0x7c
#define IM7CAM_UART_STATUS_TXRDY (1 << 1)

/* Other 0xF0??0000-pattern addresses found the exact same way as the UART
 * one (present as a literal 32-bit word inside our own U-Boot binary,
 * within the confirmed .text region) - see BRINGUP-NOTES.md section 2.
 * fh8852v201-dump's independent RE labels the same three low ones I2C0/
 * GPIO0/SPI0 on the sibling chip; the other three (0xf0c/d/e00000) had no
 * label there and their purpose here is unknown - included anyway so an
 * access to any of them shows up in the trace log instead of vanishing
 * into unmapped-memory silence (mc->ignore_memory_transaction_failures
 * below makes truly unmapped accesses silent, which would otherwise hide
 * exactly the information this stub exists to surface). */
#define IM7CAM_I2C0_BASE  0xF0200000
#define IM7CAM_GPIO0_BASE 0xF0300000
#define IM7CAM_SPI0_BASE  0xF0500000
#define IM7CAM_UNK_C_BASE 0xF0C00000
#define IM7CAM_UNK_D_BASE 0xF0D00000
#define IM7CAM_UNK_E_BASE 0xF0E00000
#define IM7CAM_PERIPH_STUB_SIZE 0x10000

/* ============================================================
 * Catch-all logging stub for every peripheral - literally everything
 * right now, this board has no real device models yet. Reads always
 * return 0 (deliberately - see qemu-ipq5018/board/mr80x.c's own identical
 * comment on its equivalent stub: this is wrong for any register that
 * gates a poll-until-bit-set loop, and *that's the point* - the log line
 * for whichever offset the guest spins on next is exactly the signal
 * needed to model that register for real).
 * ============================================================ */

static uint64_t im7cam_unimp_read(void *opaque, hwaddr offset, unsigned size)
{
    const char *name = (const char *)opaque;

    /* First real "graduate a stub once the trace shows what it needs"
     * case after the UART one: 0xf0e00000 off 0x1c/0x28 is written an
     * incrementing byte pattern (0x00..0xff, then 0xff<<20) then spun on
     * at off 0x28 - reads exactly like a timer/counter's reload-then-
     * poll-for-expiry sequence. Not yet confirmed *which* register means
     * what (unlike the UART case, no cross-reference from our own
     * disassembly has been done for this one yet) - this is a quick
     * "make the poll succeed and see what happens next" probe, not a
     * modeled register. Revisit properly if/when it turns out to matter
     * beyond just unblocking this one spin. */
    if (name && !strcmp(name, "im7cam.unk-0xf0e00000") && offset == 0x28) {
        return 0xFFFFFFFF;
    }

    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented READ  region=%s off=0x%" HWADDR_PRIx
                  " size=%u\n", name, offset, size);
    return 0;
}

static void im7cam_unimp_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented WRITE region=%s off=0x%" HWADDR_PRIx
                  " size=%u val=0x%" PRIx64 "\n",
                  (const char *)opaque, offset, size, value);
}

static const MemoryRegionOps im7cam_unimp_ops = {
    .read = im7cam_unimp_read,
    .write = im7cam_unimp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void im7cam_add_unimp_region(MemoryRegion *sysmem, const char *name,
                                     hwaddr base, hwaddr size)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);

    memory_region_init_io(mr, NULL, &im7cam_unimp_ops, (void *)name, name,
                           size);
    memory_region_add_subregion(sysmem, base, mr);
}

/* ============================================================
 * Real (if minimal) UART model, replacing the logging stub once the
 * trace revealed the actual TX protocol (see the big comment above
 * IM7CAM_UART_BASE). Status register always reports TX-ready - real
 * hardware's actual ready/busy timing isn't modeled, just "always go",
 * which is fine for a polled bootloader console. Everything other than
 * the TX data/status offsets still logs via the same im7cam_unimp_*
 * path as every other peripheral, so any *new* register this device
 * turns out to need (RX, baud/line control actually mattering, etc.)
 * still shows up in the trace instead of silently no-opping.
 * ============================================================ */

typedef struct Im7camUartState {
    MemoryRegion iomem;
    CharBackend chr;
} Im7camUartState;

static uint64_t im7cam_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    if (offset == IM7CAM_UART_REG_STATUS) {
        return IM7CAM_UART_STATUS_TXRDY;
    }
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented READ  region=im7cam.uart "
                  "off=0x%" HWADDR_PRIx " size=%u\n", offset, size);
    return 0;
}

static void im7cam_uart_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Im7camUartState *s = opaque;

    if (offset == IM7CAM_UART_REG_TX) {
        uint8_t c = (uint8_t)value;
        qemu_chr_fe_write_all(&s->chr, &c, 1);
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented WRITE region=im7cam.uart "
                  "off=0x%" HWADDR_PRIx " size=%u val=0x%" PRIx64 "\n",
                  offset, size, value);
}

static const MemoryRegionOps im7cam_uart_ops = {
    .read = im7cam_uart_read,
    .write = im7cam_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void im7cam_add_uart(MemoryRegion *sysmem)
{
    Im7camUartState *s = g_new0(Im7camUartState, 1);

    memory_region_init_io(&s->iomem, NULL, &im7cam_uart_ops, s,
                           "im7cam.uart", IM7CAM_UART_SIZE);
    memory_region_add_subregion(sysmem, IM7CAM_UART_BASE, &s->iomem);
    qemu_chr_fe_init(&s->chr, serial_hd(0), &error_abort);
}

/* ---- reset: just points the CPU at the hypothesized entry point.
 * No RAM re-population on reset yet (unlike mr80x_reset()) - this
 * skeleton is meant for a single boot attempt per QEMU invocation, not
 * guest-triggered reboots. Add that once there's an actual reason to. */

typedef struct IM7CamResetState {
    ARMCPU *cpu;
} IM7CamResetState;

/* The first 0x2000 bytes of the *file* (header + relocation table,
 * section 1) are NOT loaded - confirmed by a second bug, not just a
 * guess this time (section 4, second gdbstub snapshot): with the whole
 * file loaded unmodified and SP correctly landing in now-mapped RAM
 * (the first bug's fix), SP still read back as 0. The reset code sets
 * VBAR (section 4) from a pool constant baked into the file as the
 * plain value 0xA0800000 = IM7CAM_IMAGE_LOAD_ADDR itself - i.e. the
 * *real* vector table's expected final runtime address, per the image's
 * own linked assumptions, is IM7CAM_IMAGE_LOAD_ADDR, not
 * IM7CAM_IMAGE_LOAD_ADDR + 0x2000. The only way both that pool constant
 * and the vector table actually being at file offset 0x2000 are true
 * simultaneously is if the real loader skips the header and places file
 * offset 0x2000 at IM7CAM_IMAGE_LOAD_ADDR - so that's what this does
 * now, and the entry point is just the plain load address, no added
 * offset. Every other pool constant found so far (BSS bounds, board_init_f
 * address) is an absolute address already, so this doesn't affect them. */
#define IM7CAM_HEADER_SKIP 0x2000

static void im7cam_reset(void *opaque)
{
    IM7CamResetState *rs = opaque;
    CPUState *cs = CPU(rs->cpu);

    cpu_reset(cs);
    cpu_set_pc(cs, IM7CAM_IMAGE_LOAD_ADDR);
}

static void im7cam_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj = object_new(machine->cpu_type);
    ARMCPU *cpu = ARM_CPU(cpuobj);

    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    memory_region_add_subregion(sysmem, IM7CAM_RAM_BASE, machine->ram);

    im7cam_add_uart(sysmem);
    im7cam_add_unimp_region(sysmem, "im7cam.i2c0-guess", IM7CAM_I2C0_BASE,
                             IM7CAM_PERIPH_STUB_SIZE);
    im7cam_add_unimp_region(sysmem, "im7cam.gpio0-guess", IM7CAM_GPIO0_BASE,
                             IM7CAM_PERIPH_STUB_SIZE);
    im7cam_add_unimp_region(sysmem, "im7cam.spi0-guess", IM7CAM_SPI0_BASE,
                             IM7CAM_PERIPH_STUB_SIZE);
    im7cam_add_unimp_region(sysmem, "im7cam.unk-0xf0c00000",
                             IM7CAM_UNK_C_BASE, IM7CAM_PERIPH_STUB_SIZE);
    im7cam_add_unimp_region(sysmem, "im7cam.unk-0xf0d00000",
                             IM7CAM_UNK_D_BASE, IM7CAM_PERIPH_STUB_SIZE);
    im7cam_add_unimp_region(sysmem, "im7cam.unk-0xf0e00000",
                             IM7CAM_UNK_E_BASE, IM7CAM_PERIPH_STUB_SIZE);

    if (!machine->kernel_filename) {
        error_report("im7cam: no -kernel given - pass the extracted "
                      "0_U-Boot.bin (see BRINGUP-NOTES.md)");
        exit(1);
    }

    gchar *filebuf = NULL;
    gsize filelen = 0;
    GError *gerr = NULL;

    if (!g_file_get_contents(machine->kernel_filename, &filebuf, &filelen,
                              &gerr)) {
        error_report("im7cam: could not read '%s': %s",
                      machine->kernel_filename, gerr->message);
        exit(1);
    }
    if (filelen <= IM7CAM_HEADER_SKIP) {
        error_report("im7cam: '%s' is only %zu bytes - smaller than the "
                      "%u-byte header this board strips before loading "
                      "(section 1/4 of BRINGUP-NOTES.md). Wrong file?",
                      machine->kernel_filename, filelen,
                      (unsigned)IM7CAM_HEADER_SKIP);
        exit(1);
    }

    size_t codelen = filelen - IM7CAM_HEADER_SKIP;
    rom_add_blob_fixed("im7cam.uboot", filebuf + IM7CAM_HEADER_SKIP, codelen,
                        IM7CAM_IMAGE_LOAD_ADDR);
    info_report("im7cam: loaded '%s' (%zu of %zu bytes, skipped %u-byte "
                "header) at 0x%" PRIx32,
                machine->kernel_filename, codelen, filelen,
                (unsigned)IM7CAM_HEADER_SKIP, (uint32_t)IM7CAM_IMAGE_LOAD_ADDR);

    IM7CamResetState *rs = g_new0(IM7CamResetState, 1);
    rs->cpu = cpu;
    qemu_register_reset(im7cam_reset, rs);
}

static void im7cam_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Fullhan im7 / Imou IPC-S21F research machine "
               "(skeleton only - see BRINGUP-NOTES.md)";
    mc->init = im7cam_init;
    /* Was arm926 (ARMv5) - real evidence overturned that guess almost
     * immediately: with IM7CAM_ENTRY_OFFSET pointed at real code, the very
     * first thing it did was write cp15 c12,c0,0 (VBAR, the exception
     * vector base register) - a real ARMv7-A/ARMv6+Security-Extensions
     * register that doesn't exist at all on ARMv5, so arm926 logged
     * "unsupported AArch32 system register" on real guest code, not
     * garbage. cortex-a7 is the new placeholder: a real ARMv7-A core,
     * common in exactly this class/era of cheap embedded SoC, and QEMU's
     * best-supported ARMv7-A model. Still not chip-confirmed - see
     * BRINGUP-NOTES.md section 3. */
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    mc->default_ram_size = IM7CAM_RAM_SIZE;
    mc->default_ram_id = "im7cam.ram";
    mc->ignore_memory_transaction_failures = true;
    mc->max_cpus = 1;
}

static const TypeInfo im7cam_machine_typeinfo = {
    .name = MACHINE_TYPE_NAME("im7cam"),
    .parent = TYPE_MACHINE,
    .class_init = im7cam_machine_class_init,
};

static void im7cam_machine_register_types(void)
{
    type_register_static(&im7cam_machine_typeinfo);
}
type_init(im7cam_machine_register_types);
