/*
 * Amlogic GXLX / p271 research machine.
 *
 * This is deliberately a boot-oriented model, not a complete SoC.  It
 * starts at the BL31 -> BL33 boundary because the factory FIP is encrypted,
 * then runs an ordinary Meson GXL U-Boot against the unmodified full eMMC
 * dump.  Addresses come from the decoded factory gxlx_p271_1g DTB and the
 * public Meson GX Linux/U-Boot drivers.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/intc/arm_gic.h"
#include "hw/irq.h"
#include "hw/sd/sd.h"
#include "hw/sd/sdcard_legacy.h"
#include "sysemu/block-backend-global-state.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "chardev/char-fe.h"
#include "target/arm/cpu.h"
#include "target/arm/gtimer.h"
#include "qom/object.h"

#define TYPE_GXL_MMC "gxl-p211-mmc"
OBJECT_DECLARE_SIMPLE_TYPE(GXLMmcState, GXL_MMC)

#define GXL_RAM_BASE          0x00000000ULL
#define GXL_RAM_SIZE          (1 * GiB)
#define GXL_UBOOT_ENTRY       0x01000000ULL
#define GXL_FACTORY_DTB_ADDR  0x30000000ULL

#define GXL_UART_BASE         0xc81004c0ULL
#define GXL_UART_A_BASE       0xc11084c0ULL
#define GXL_UART_SIZE         0x18
#define GXL_GIC_DIST_BASE     0xc4301000ULL
#define GXL_GIC_CPU_BASE      0xc4302000ULL
#define GXL_GIC_NUM_IRQ       256
#define GXL_MMC_BASE          0xd0074000ULL
#define GXL_MMC_SIZE          0x800
#define GXL_MMC_SPI           218

/* Meson AO UART registers. */
#define UART_WFIFO       0x00
#define UART_RFIFO       0x04
#define UART_CONTROL     0x08
#define UART_STATUS      0x0c
#define UART_MISC        0x10
#define UART_REG5        0x14
#define UART_RX_EMPTY    (1u << 20)
#define UART_TX_FULL     (1u << 21)
#define UART_TX_EMPTY    (1u << 22)

/* Meson GX SD/eMMC registers and command descriptor bits. */
#define MMC_CLOCK        0x00
#define MMC_DELAY        0x04
#define MMC_ADJUST       0x08
#define MMC_START        0x40
#define MMC_CFG          0x44
#define MMC_STATUS       0x48
#define MMC_IRQ_EN       0x4c
#define MMC_CMD_CFG      0x50
#define MMC_CMD_ARG      0x54
#define MMC_CMD_DAT      0x58
#define MMC_CMD_RSP      0x5c
#define MMC_CMD_RSP1     0x60
#define MMC_CMD_RSP2     0x64
#define MMC_CMD_RSP3     0x68
#define MMC_SRAM         0x200

#define CMD_LENGTH_MASK  0x1ffu
#define CMD_BLOCK_MODE   (1u << 9)
#define CMD_END_CHAIN    (1u << 11)
#define CMD_NO_RESP      (1u << 16)
#define CMD_NO_CMD       (1u << 17)
#define CMD_DATA_IO      (1u << 18)
#define CMD_DATA_WR      (1u << 19)
#define CMD_RESP_128     (1u << 21)
#define CMD_INDEX_SHIFT  24
#define CMD_OWNER        (1u << 31)
#define STATUS_EOC       (1u << 13)
#define STATUS_RESP_TIMEOUT (1u << 11)

typedef struct GXLUartState {
    MemoryRegion iomem;
    CharBackend chr;
    uint32_t control;
    uint32_t misc;
    uint32_t reg5;
    uint8_t rx[256];
    unsigned rx_head;
    unsigned rx_count;
} GXLUartState;

struct GXLMmcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq;
    uint32_t regs[GXL_MMC_SIZE / 4];
};

typedef struct GXLResetState {
    ARMCPU *cpu;
} GXLResetState;

static int gxl_uart_can_receive(void *opaque)
{
    GXLUartState *s = opaque;
    return ARRAY_SIZE(s->rx) - s->rx_count;
}

static void gxl_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    GXLUartState *s = opaque;

    while (size-- > 0 && s->rx_count < ARRAY_SIZE(s->rx)) {
        s->rx[(s->rx_head + s->rx_count) % ARRAY_SIZE(s->rx)] = *buf++;
        s->rx_count++;
    }
}

static uint64_t gxl_uart_read(void *opaque, hwaddr off, unsigned size)
{
    GXLUartState *s = opaque;

    switch (off) {
    case UART_RFIFO:
        if (s->rx_count) {
            uint8_t ch = s->rx[s->rx_head];
            s->rx_head = (s->rx_head + 1) % ARRAY_SIZE(s->rx);
            s->rx_count--;
            qemu_chr_fe_accept_input(&s->chr);
            return ch;
        }
        return 0;
    case UART_CONTROL:
        return s->control;
    case UART_STATUS:
        return UART_TX_EMPTY | (s->rx_count ? 0 : UART_RX_EMPTY);
    case UART_MISC:
        return s->misc;
    case UART_REG5:
        return s->reg5;
    default:
        return 0;
    }
}

static void gxl_uart_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    GXLUartState *s = opaque;
    uint8_t ch;

    switch (off) {
    case UART_WFIFO:
        ch = value;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;
    case UART_CONTROL:
        s->control = value;
        break;
    case UART_MISC:
        s->misc = value;
        break;
    case UART_REG5:
        s->reg5 = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps gxl_uart_ops = {
    .read = gxl_uart_read,
    .write = gxl_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gxl_mmc_update_irq(GXLMmcState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[MMC_STATUS / 4] &
                            s->regs[MMC_IRQ_EN / 4]));
}

static void gxl_mmc_store_response(GXLMmcState *s, const uint8_t *rsp,
                                   int len)
{
    if (len >= 16) {
        s->regs[MMC_CMD_RSP3 / 4] = ldl_be_p(rsp + 0);
        s->regs[MMC_CMD_RSP2 / 4] = ldl_be_p(rsp + 4);
        s->regs[MMC_CMD_RSP1 / 4] = ldl_be_p(rsp + 8);
        s->regs[MMC_CMD_RSP / 4] = ldl_be_p(rsp + 12);
    } else if (len >= 4) {
        s->regs[MMC_CMD_RSP / 4] = ldl_be_p(rsp);
    } else {
        s->regs[MMC_CMD_RSP / 4] = 0;
    }
}

static void gxl_mmc_transfer(GXLMmcState *s, uint32_t cfg, uint32_t arg,
                             uint32_t data_addr)
{
    uint8_t response[16] = { 0 };
    SDRequest req = {
        .cmd = (cfg >> CMD_INDEX_SHIFT) & 0x3f,
        .arg = arg,
        .crc = 0,
    };
    unsigned blocks = cfg & CMD_LENGTH_MASK;
    unsigned block_len = 1u << ((s->regs[MMC_CFG / 4] >> 4) & 0xf);
    size_t length;
    uint8_t *buf = NULL;
    int response_len = 0;

    if (!(cfg & CMD_NO_CMD)) {
        response_len = sdbus_do_command(&s->sdbus, &req, response);
        /*
         * QEMU's generic eMMC currently advertises only a narrow OCR window
         * after the legacy U-Boot negotiation.  The factory Meson driver
         * offers 0x00200080, so retain the card's busy/capacity bits while
         * exposing the standard MMC voltage windows expected by Linux.
         */
        if (req.cmd == 1 && response_len >= 4) {
            stl_be_p(response, ldl_be_p(response) | 0x00ff8000u);
        }
        if (response_len > 0) {
            gxl_mmc_store_response(s, response, response_len);
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "gxl-p211-mmc: CMD%u arg=%08x cfg=%08x data=%08x "
                      "rsp=%d/%08x\n",
                      req.cmd, arg, cfg, data_addr, response_len,
                      s->regs[MMC_CMD_RSP / 4]);
    }

    if (cfg & CMD_DATA_IO) {
        if (!(cfg & CMD_BLOCK_MODE)) {
            block_len = blocks;
            blocks = 1;
        }
        if (!blocks) {
            blocks = 1;
        }
        length = (size_t)blocks * block_len;
        if (length > 128 * MiB) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "gxl-p211: refusing implausible MMC DMA length %zu\n",
                          length);
        } else {
            buf = g_malloc(length);
            if (cfg & CMD_DATA_WR) {
                cpu_physical_memory_read(data_addr & ~3u, buf, length);
                sdbus_write_data(&s->sdbus, buf, length);
            } else {
                sdbus_read_data(&s->sdbus, buf, length);
                cpu_physical_memory_write(data_addr & ~3u, buf, length);
            }
            g_free(buf);
        }
    }

    s->regs[MMC_CMD_CFG / 4] = cfg & ~CMD_OWNER;
    s->regs[MMC_STATUS / 4] |= STATUS_EOC;
    if (!(cfg & CMD_NO_RESP) && response_len <= 0) {
        s->regs[MMC_STATUS / 4] |= STATUS_RESP_TIMEOUT;
    }
    gxl_mmc_update_irq(s);
}

static void gxl_mmc_run_chain(GXLMmcState *s, uint32_t start)
{
    hwaddr addr = start & ~3u;
    unsigned count;

    for (count = 0; count < 1024; count++, addr += 16) {
        uint32_t desc[4];
        cpu_physical_memory_read(addr, desc, sizeof(desc));
        desc[0] = le32_to_cpu(desc[0]);
        desc[1] = le32_to_cpu(desc[1]);
        desc[2] = le32_to_cpu(desc[2]);
        gxl_mmc_transfer(s, desc[0], desc[1], desc[2]);
        desc[0] &= ~CMD_OWNER;
        desc[0] = cpu_to_le32(desc[0]);
        cpu_physical_memory_write(addr, desc, sizeof(uint32_t));
        desc[3] = cpu_to_le32(s->regs[MMC_CMD_RSP / 4]);
        cpu_physical_memory_write(addr + 12, &desc[3], sizeof(desc[3]));
        if (le32_to_cpu(desc[0]) & CMD_END_CHAIN) {
            break;
        }
    }
    s->regs[MMC_START / 4] = start & ~2u;
}

static uint64_t gxl_mmc_read(void *opaque, hwaddr off, unsigned size)
{
    GXLMmcState *s = opaque;
    if (off >= GXL_MMC_SIZE) {
        return 0;
    }
    return s->regs[off / 4];
}

static void gxl_mmc_write(void *opaque, hwaddr off, uint64_t value,
                          unsigned size)
{
    GXLMmcState *s = opaque;
    uint32_t val = value;

    if (off >= GXL_MMC_SIZE) {
        return;
    }
    if (off == MMC_STATUS) {
        s->regs[MMC_STATUS / 4] &= ~val;
        gxl_mmc_update_irq(s);
        return;
    }
    s->regs[off / 4] = val;
    if (off == MMC_CMD_ARG && (s->regs[MMC_CMD_CFG / 4] & CMD_OWNER)) {
        gxl_mmc_transfer(s, s->regs[MMC_CMD_CFG / 4], val,
                         s->regs[MMC_CMD_DAT / 4]);
    } else if (off == MMC_START && (val & 2)) {
        gxl_mmc_run_chain(s, val);
    } else if (off == MMC_IRQ_EN) {
        gxl_mmc_update_irq(s);
    }
}

static const MemoryRegionOps gxl_mmc_ops = {
    .read = gxl_mmc_read,
    .write = gxl_mmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gxl_mmc_reset(DeviceState *dev)
{
    GXLMmcState *s = GXL_MMC(dev);
    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void gxl_mmc_realize(DeviceState *dev, Error **errp)
{
    GXLMmcState *s = GXL_MMC(dev);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, dev, "sd-bus");
}

static void gxl_mmc_init(Object *obj)
{
    GXLMmcState *s = GXL_MMC(obj);
    memory_region_init_io(&s->iomem, obj, &gxl_mmc_ops, s,
                          "gxl-p211.mmc", GXL_MMC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void gxl_mmc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = gxl_mmc_realize;
    dc->reset = gxl_mmc_reset;
}

static const TypeInfo gxl_mmc_type_info = {
    .name = TYPE_GXL_MMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GXLMmcState),
    .instance_init = gxl_mmc_init,
    .class_init = gxl_mmc_class_init,
};

static uint64_t gxl_stub_read(void *opaque, hwaddr off, unsigned size)
{
    uintptr_t base = (uintptr_t)opaque;

    /* AO_SEC_GP_CFG0: 1024 MiB RAM, eMMC boot device. */
    if (base == 0xc8100000 && off == 0x240) {
        return (1024u << 16) | 1u;
    }
    qemu_log_mask(LOG_UNIMP, "gxl-p211: stub read @0x%08" PRIxPTR
                  "+0x%" HWADDR_PRIx " size=%u\n", base, off, size);
    return 0;
}

static void gxl_stub_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "gxl-p211: stub write @0x%08" PRIxPTR
                  "+0x%" HWADDR_PRIx " size=%u value=0x%" PRIx64 "\n",
                  (uintptr_t)opaque, off, size, value);
}

static const MemoryRegionOps gxl_stub_ops = {
    .read = gxl_stub_read,
    .write = gxl_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static void gxl_add_stub(MemoryRegion *sysmem, hwaddr base, hwaddr size,
                         const char *name)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &gxl_stub_ops, (void *)(uintptr_t)base,
                          name, size);
    memory_region_add_subregion_overlap(sysmem, base, mr, -10);
}

static void gxl_cpu_reset(void *opaque)
{
    GXLResetState *s = opaque;
    CPUState *cs = CPU(s->cpu);
    cpu_reset(cs);
    cpu_set_pc(cs, GXL_UBOOT_ENTRY);
    s->cpu->psci_conduit = QEMU_PSCI_CONDUIT_SMC;
}

static void gxl_p211_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj = object_new(machine->cpu_type);
    ARMCPU *cpu = ARM_CPU(cpuobj);
    DeviceState *gic;
    DeviceState *mmc;
    DriveInfo *dinfo;
    const char *dtb;
    ssize_t size;
    unsigned priv_base;

    object_property_set_bool(cpuobj, "aarch64", true, &error_fatal);
    /* Keep BL33 at EL3 so U-Boot can perform its ARM64 -> ARM32 eret. */
    object_property_set_bool(cpuobj, "has_el3", true, &error_fatal);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    gic = qdev_new(gic_class_name());
    qdev_prop_set_uint32(gic, "num-cpu", 1);
    qdev_prop_set_uint32(gic, "num-irq", GXL_GIC_NUM_IRQ);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 0, GXL_GIC_DIST_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 1, GXL_GIC_CPU_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(gic), 0,
                       qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(gic), 1,
                       qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));
    priv_base = GXL_GIC_NUM_IRQ - 32;
    qdev_connect_gpio_out(DEVICE(cpu), GTIMER_SEC,
                          qdev_get_gpio_in(gic, priv_base + 29));
    qdev_connect_gpio_out(DEVICE(cpu), GTIMER_PHYS,
                          qdev_get_gpio_in(gic, priv_base + 30));
    qdev_connect_gpio_out(DEVICE(cpu), GTIMER_VIRT,
                          qdev_get_gpio_in(gic, priv_base + 27));
    qdev_connect_gpio_out(DEVICE(cpu), GTIMER_HYP,
                          qdev_get_gpio_in(gic, priv_base + 26));

    memory_region_add_subregion(sysmem, GXL_RAM_BASE, machine->ram);

    if (!machine->kernel_filename) {
        error_report("gxl-p211: -kernel must point to a Meson GXL BL33");
        exit(1);
    }
    size = load_image_targphys(machine->kernel_filename, GXL_UBOOT_ENTRY,
                               GXL_RAM_SIZE - GXL_UBOOT_ENTRY);
    if (size < 0) {
        error_report("gxl-p211: cannot load U-Boot '%s'",
                     machine->kernel_filename);
        exit(1);
    }
    info_report("gxl-p211: loaded BL33 '%s' (%zd bytes) at 0x%08" PRIx64,
                machine->kernel_filename, size, (uint64_t)GXL_UBOOT_ENTRY);

    dtb = getenv("GXL_P211_DTB");
    if (!dtb) {
        error_report("gxl-p211: GXL_P211_DTB is required");
        exit(1);
    }
    size = load_image_targphys(dtb, GXL_FACTORY_DTB_ADDR,
                               GXL_RAM_SIZE - GXL_FACTORY_DTB_ADDR);
    if (size < 0) {
        error_report("gxl-p211: cannot load factory DTB '%s'", dtb);
        exit(1);
    }
    info_report("gxl-p211: loaded factory DTB (%zd bytes) at 0x%08" PRIx64,
                size, (uint64_t)GXL_FACTORY_DTB_ADDR);

    {
        GXLResetState *reset = g_new0(GXLResetState, 1);
        reset->cpu = cpu;
        qemu_register_reset(gxl_cpu_reset, reset);
    }

    /* Catch-all hardware windows. Specific devices overlap at priority 0. */
    gxl_add_stub(sysmem, 0xc1100000, 0x100000, "gxl-p211.cbus");
    gxl_add_stub(sysmem, 0xc8100000, 0x100000, "gxl-p211.aobus");
    gxl_add_stub(sysmem, 0xc8830000, 0x10000, "gxl-p211.periphs");
    gxl_add_stub(sysmem, 0xc9000000, 0x100000, "gxl-p211.usb-eth");
    gxl_add_stub(sysmem, 0xd0000000, 0x200000, "gxl-p211.apb");

    {
        GXLUartState *uart = g_new0(GXLUartState, 1);
        MemoryRegion *uart_a_alias = g_new0(MemoryRegion, 1);
        qemu_chr_fe_init(&uart->chr, serial_hd(0), &error_abort);
        qemu_chr_fe_set_handlers(&uart->chr, gxl_uart_can_receive,
                                 gxl_uart_receive, NULL, NULL, uart, NULL,
                                 true);
        if (getenv("GXL_P211_STOP_AUTOBOOT")) {
            uart->rx[0] = ' ';
            uart->rx_count = 1;
        }
        memory_region_init_io(&uart->iomem, NULL, &gxl_uart_ops, uart,
                              "gxl-p211.uart", GXL_UART_SIZE);
        memory_region_add_subregion(sysmem, GXL_UART_BASE, &uart->iomem);
        memory_region_init_alias(uart_a_alias, NULL,
                                 "gxl-p211.uart-a-alias", &uart->iomem,
                                 0, GXL_UART_SIZE);
        memory_region_add_subregion(sysmem, GXL_UART_A_BASE, uart_a_alias);
    }

    mmc = qdev_new(TYPE_GXL_MMC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mmc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(mmc), 0, GXL_MMC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(mmc), 0,
                       qdev_get_gpio_in(gic, GXL_MMC_SPI));

    dinfo = drive_get(IF_SD, 0, 0);
    if (!dinfo) {
        error_report("gxl-p211: attach emmc_full.img with -drive if=sd");
        exit(1);
    }
    {
        DeviceState *card = qdev_new(TYPE_EMMC);
        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
        qdev_realize_and_unref(card, qdev_get_child_bus(mmc, "sd-bus"),
                               &error_fatal);
    }
}

static void gxl_p211_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Amlogic GXLX p271/MXQ Pro research board";
    mc->init = gxl_p211_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->default_ram_size = GXL_RAM_SIZE;
    mc->default_ram_id = "gxl-p211.ram";
    mc->max_cpus = 1;
    mc->ignore_memory_transaction_failures = true;
}

static const TypeInfo gxl_p211_machine_type = {
    .name = MACHINE_TYPE_NAME("gxl-p211"),
    .parent = TYPE_MACHINE,
    .class_init = gxl_p211_machine_class_init,
};

static void gxl_p211_register_types(void)
{
    type_register_static(&gxl_mmc_type_info);
    type_register_static(&gxl_p211_machine_type);
}

type_init(gxl_p211_register_types)
