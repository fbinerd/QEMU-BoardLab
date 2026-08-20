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
#include "cpu.h"
#include "qom/object.h"

/* ---- memory map - see BRINGUP-NOTES.md sections 1-3 ---- */

/* Section 1: the image's own second header (offset 0x160-0x17B) repeats
 * the word 0xA0800000 twice (load address + entry point). Corroborated by
 * pointer literals inside the code (section 2's disassembly) landing in
 * the 0xA080xxxx-0xA083xxxx range, consistent with this base plus normal
 * .bss/heap growth. NOT yet confirmed whether the *whole* 320 KB partition
 * (header included) loads here, or just the part from file offset 0x1000
 * or 0x2000 onward - this skeleton assumes "whole file", the cheapest
 * hypothesis to falsify first. */
#define IM7CAM_RAM_BASE   0xA0800000
/* Not confirmed - placeholder. This class of low-end IPC SoC typically
 * ships 32-128 MB of DRAM; 64 MB is a middle guess, not a measurement. */
#define IM7CAM_RAM_SIZE   (64 * MiB)

/* Section 2: 0xF0700000 appears in our own U-Boot binary's literal pool
 * (file offset 0xaae8) at a device/struct-init call site, AND matches an
 * independent Ghidra-based RE of a sibling Fullhan chip (FH8852V201,
 * github.com/pavliha/fh8852v201-dump, OpenIPC project) which lists that
 * same address as UART0. Two independent signals agreeing - the strongest
 * evidence in this file - but the in-block register layout (TX data
 * offset, TX-ready bit position, RX path) is NOT confirmed. This is
 * exactly why it's wired below as a logging stub, not a real UART model:
 * the plan is to read the access trace back out and reverse the protocol
 * from real guest behavior, not guess it. */
#define IM7CAM_UART_BASE  0xF0700000
#define IM7CAM_UART_SIZE  0x1000

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
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented READ  region=%s off=0x%" HWADDR_PRIx
                  " size=%u\n", (const char *)opaque, offset, size);
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

/* ---- reset: just points the CPU at the hypothesized entry point.
 * No RAM re-population on reset yet (unlike mr80x_reset()) - this
 * skeleton is meant for a single boot attempt per QEMU invocation, not
 * guest-triggered reboots. Add that once there's an actual reason to. */

typedef struct IM7CamResetState {
    ARMCPU *cpu;
} IM7CamResetState;

/* Entry point: NOT the raw load base. First test (whole 320 KB file
 * loaded at IM7CAM_RAM_BASE, entry = IM7CAM_RAM_BASE) ran 10s with no
 * UART trace output at all - consistent with the CPU executing the
 * image's own header/reloc-table bytes as garbage instructions and
 * getting stuck in an undefined-instruction trap loop at the (unmapped,
 * reads-as-zero) default vector base, never reaching real code. Section
 * 1 of BRINGUP-NOTES.md independently found real code starts at file
 * offset 0x2000 (by push{...,lr} prologue density, not by decoding the
 * header) - so entry = base + 0x2000 is the next hypothesis to try,
 * keeping the "load the whole file unmodified at the base" load model
 * unchanged (the cheaper of the two variables to have gotten wrong).
 * Still NOT confirmed - update this comment (and BRINGUP-NOTES.md) once
 * real UART trace output settles the question either way. */
#define IM7CAM_ENTRY_OFFSET 0x2000

static void im7cam_reset(void *opaque)
{
    IM7CamResetState *rs = opaque;
    CPUState *cs = CPU(rs->cpu);

    cpu_reset(cs);
    cpu_set_pc(cs, IM7CAM_RAM_BASE + IM7CAM_ENTRY_OFFSET);
}

static void im7cam_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj = object_new(machine->cpu_type);
    ARMCPU *cpu = ARM_CPU(cpuobj);

    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    memory_region_add_subregion(sysmem, IM7CAM_RAM_BASE, machine->ram);

    im7cam_add_unimp_region(sysmem, "im7cam.uart-guess", IM7CAM_UART_BASE,
                             IM7CAM_UART_SIZE);
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

    ssize_t sz = load_image_targphys(machine->kernel_filename,
                                      IM7CAM_RAM_BASE, IM7CAM_RAM_SIZE);
    if (sz < 0) {
        error_report("im7cam: could not load '%s'",
                      machine->kernel_filename);
        exit(1);
    }
    info_report("im7cam: loaded '%s' (%zd bytes) at 0x%" PRIx32,
                machine->kernel_filename, sz, (uint32_t)IM7CAM_RAM_BASE);

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
