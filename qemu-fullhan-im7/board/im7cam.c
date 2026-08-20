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
#include "qemu/timer.h"
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
/* Was a placeholder ("needs to cover BSS end with margin"), now
 * corroborated by a real boot-log value (section 7): U-Boot prints
 * "DRAM:  " followed by a size pulled from a runtime gd/bd struct field
 * (`ldr r4,[r3,#24]` off a pointer this build keeps in r8 - the classic
 * global_data convention - then a print_size()-style call), not a
 * literal baked into the print itself. That field is set once, early,
 * from this exact board's own compiled-in DRAM config - QEMU has no
 * mechanism this code could be querying instead, so the "64 MiB" this
 * machine's boot log actually shows is this real board's own configured
 * size, not an artifact of this file's IM7CAM_RAM_SIZE choice reflecting
 * back. Confirms the earlier guess was right, doesn't independently
 * re-derive it from first principles. */
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
 * which primed a struct with that exact byte. Other offsets seen in the
 * trace (0x4, 0x8, 0xc - written 0/0x9/0x80/7 etc., presumably baud/line
 * control) are accepted and logged but not modeled for real yet.
 *
 * RX added later (section 8): disassembled the driver's own getc()-style
 * function (`arm-none-eabi-objdump --start-address=0xa8a0
 * --stop-address=0xaa18`) and found a completely ordinary poll-then-read
 * loop - `ldr lr,[base+0x14]; tst lr,#1; beq retry; ldrb r3,[base]` -
 * poll offset 0x14 until bit 0 (RX-data-ready) is set, then read the
 * byte from the *same* offset 0 the TX path writes to (one shared data
 * register for both directions, a very ordinary UART design - not
 * guessed, read straight off the driver's own disassembly). */
#define IM7CAM_UART_BASE      0xF0700000
#define IM7CAM_UART_SIZE      0x1000
#define IM7CAM_UART_REG_DATA   0x00 /* write = TX, read = RX - same address */
#define IM7CAM_UART_REG_TX     IM7CAM_UART_REG_DATA
#define IM7CAM_UART_REG_RX     IM7CAM_UART_REG_DATA
#define IM7CAM_UART_REG_STATUS 0x7c
#define IM7CAM_UART_STATUS_TXRDY (1 << 1)
#define IM7CAM_UART_REG_RX_STATUS 0x14
#define IM7CAM_UART_RXSTATUS_READY (1 << 0)

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
#define IM7CAM_SPI0_LABELED_BASE  0xF0500000  /* fh8852v201-dump's SPI0 label - see IM7CAM_SPI_BASE below, this chip's real one turned out to be elsewhere */
#define IM7CAM_UNK_D_BASE 0xF0D00000
#define IM7CAM_PERIPH_STUB_SIZE 0x10000

/* Free-running timer/counter - found via gdbstub, not the trace log this
 * time (section 6): the SPI status poll (previous comment block) doesn't
 * spin bare - the actual hang was a real, bounded wait-with-timeout loop
 * that calls a get-elapsed-ticks helper each iteration, and that helper
 * reads a hardware counter at this block's offset 0x04. Since the
 * catch-all stub always returns 0 there, elapsed time never advances and
 * the timeout side of the wait never fires - not a true infinite loop in
 * the code, just an infinite wait for a timeout that this skeleton was
 * suppressing. Consistent with earlier trace evidence too: this same
 * block got written 0x5f5e100 (100,000,000 - a very clock-rate-shaped
 * number) at offset 0x0 during the earlier clock/PLL-looking init
 * sequence (section 5) - a free-running counter with a configurable tick
 * rate is exactly what that write would be setting up. */
#define IM7CAM_TIMER_BASE 0xF0C00000
#define IM7CAM_TIMER_SIZE 0x10000
#define IM7CAM_TIMER_REG_COUNT 0x04

/* Reset/clock-management block (GCC-equivalent, guessing at the label
 * qemu-ipq5018/board/mr80x.c uses for the analogous IPQ5018 controller -
 * this device's own real name for it isn't known). Found the same way as
 * the timer: gdbstub-frozen PC, disassembled (`arm-none-eabi-objdump
 * --start-address=0x1d9c0 --stop-address=0x1db00`), landed on a
 * completely ordinary "soft-reset a sub-block, poll for hardware ack"
 * idiom repeated at least twice in the same function: `mvn r3,#N;
 * str r3,[r4,#0x54]; ldr r3,[r4,#0x54]; cmn r3,#1; bne back` - write a
 * masked value (clearing one specific reset bit) to offset 0x54, then
 * spin until the SAME offset reads back as 0xFFFFFFFF (all bits set,
 * i.e. "reset released/acked"). This board's stub previously left
 * 0xF0000000 completely unmapped - genuinely different from every other
 * hang so far in this file, which were all *modeled-but-wrong* stubs;
 * this address had never been touched or logged at all until this
 * session, since ignore_memory_transaction_failures silently eats
 * unmapped reads as 0, which never satisfies the == -1 check. */
#define IM7CAM_RESET_BASE 0xF0000000
#define IM7CAM_RESET_SIZE 0x10000
#define IM7CAM_RESET_REG_ACK 0x54

/* This chip's REAL SPI (flash) controller - confirmed by live disassembly
 * (section 6 of BRINGUP-NOTES.md), not the sibling-chip label above. What
 * used to be the "im7cam.unk-0xf0e00000" catch-all stub turned out, once
 * the guest actually started using it heavily, to be exactly this: a
 * status-poll hang at file offset 0xa140 (`ldr r3,[r4,#0x28]; and
 * r3,r3,#5; cmp r3,#4; bne back`) sitting right next to a byte written to
 * `[r4,#0x60]` matching this region's real trace output exactly
 * (`off=0x60 val=0x9f`) - r4 is this peripheral's base. `[r4,#0x60]` is
 * the TX/RX data FIFO, `[r4,#0x28]` is a status register whose "idle,
 * ready, no error" encoding is `0x4` (bit2 set, bit0 clear per that
 * mask). Everything else in this block (offsets 0x0/0x8/0x10/0x14/0x1c/
 * 0x20/0x2c and a second cluster at 0xf4/0xf8/0xfc/0x100/0x104) is still
 * unconfirmed control/clock-config noise, passed through to the same
 * logging stub every other unmodeled register uses. */
#define IM7CAM_SPI_BASE       0xF0E00000
#define IM7CAM_SPI_SIZE       0x10000
#define IM7CAM_SPI_REG_DATA   0x60
#define IM7CAM_SPI_REG_STATUS 0x28
#define IM7CAM_SPI_STATUS_IDLE 0x4

/* THE actual root cause of the whole "SF: Unsupported manufacturer"
 * saga (BRINGUP-NOTES.md sections 6/8/9) - found by getting real, public
 * U-Boot 2010.06 source for the generic drivers/mtd/spi/spi_flash.c this
 * device's SPI_FLASH_MAX_ID_LEN=5/`idcode[5]` exactly matches (see
 * section 10), then re-reading this file's *own* driver
 * (`0x9d78`) with that as a map instead of guessing blind. `[r4,#0x24]`
 * is read once per "chunk" of a read loop and used directly as the byte
 * count to copy out of the FIFO this pass (`r3 = r5 + *(r4+0x24)`, then
 * `cmp r5,r3; bne copy_loop`) - with the old catch-all stub returning 0
 * here, every single read chunk copied **zero bytes**, every time,
 * regardless of what the FIFO itself (`IM7CAM_SPI_REG_DATA`) or
 * `resp_buf` contained - which is exactly why two separate, real fixes
 * to the response-side state machine (section 9) provably changed
 * nothing: the copy loop that would have consulted them never ran. */
#define IM7CAM_SPI_REG_AVAIL 0x24

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
    bool rx_valid;
    uint8_t rx_byte;
} Im7camUartState;

static uint64_t im7cam_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    Im7camUartState *s = opaque;

    if (offset == IM7CAM_UART_REG_STATUS) {
        return IM7CAM_UART_STATUS_TXRDY;
    }
    if (offset == IM7CAM_UART_REG_RX_STATUS) {
        return s->rx_valid ? IM7CAM_UART_RXSTATUS_READY : 0;
    }
    if (offset == IM7CAM_UART_REG_RX) {
        uint8_t c = s->rx_byte;
        s->rx_valid = false;
        return c;
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

/* Single-byte RX buffer is enough for interactive typing (autoboot
 * interrupt, u-boot console input) - a real FIFO would matter for
 * pasted/bulk input, not needed yet. can_receive backpressures the
 * chardev (returns 0 = "not ready") while a byte is still waiting to be
 * read by the guest, so nothing gets silently dropped. */
static int im7cam_uart_can_receive(void *opaque)
{
    Im7camUartState *s = opaque;

    return s->rx_valid ? 0 : 1;
}

static void im7cam_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    Im7camUartState *s = opaque;

    if (size > 0) {
        s->rx_byte = buf[0];
        s->rx_valid = true;
    }
}

static void im7cam_add_uart(MemoryRegion *sysmem)
{
    Im7camUartState *s = g_new0(Im7camUartState, 1);

    memory_region_init_io(&s->iomem, NULL, &im7cam_uart_ops, s,
                           "im7cam.uart", IM7CAM_UART_SIZE);
    memory_region_add_subregion(sysmem, IM7CAM_UART_BASE, &s->iomem);
    qemu_chr_fe_init(&s->chr, serial_hd(0), &error_abort);
    qemu_chr_fe_set_handlers(&s->chr, im7cam_uart_can_receive,
                              im7cam_uart_receive, NULL, NULL, s, NULL,
                              true);
}

/* ============================================================
 * Real (if still evidence-light past the two registers section 6 of
 * BRINGUP-NOTES.md actually confirmed) SPI/flash controller model - the
 * qemu-ipq5018/board/mr80x.c equivalent of its --nand-image real-flash
 * backing, for the same reason: without real data behind flash reads,
 * U-Boot can get the controller "working" (status always idle) but would
 * only ever read back zeroes/garbage for the actual kernel/rootfs it's
 * trying to load - can't get further than a bootloader banner without
 * this.
 *
 * Protocol model (heuristic, NOT disassembly-confirmed the way the two
 * registers below are - expect this part to need iteration once real
 * trace output shows it's wrong):
 *   - a write of 0 to offset 0x08 resets the byte accumulator (matches
 *     the disassembled open/close bracket at file offset 0xa120/0xa130:
 *     [r4+8]=0 before a burst, =1 after - read as "transaction
 *     start/end", not confirmed as literally an accumulator reset, but
 *     it's the only observed signal that brackets each burst).
 *   - each byte written to the FIFO (offset 0x60, IM7CAM_SPI_REG_DATA)
 *     appends to that accumulator. Once 4 bytes are in and the first is
 *     a standard SPI NOR opcode (0x03 READ or 0x0B FAST_READ), the
 *     remaining 3 are taken as a big-endian 24-bit flash address and
 *     latched - completely standard SPI NOR protocol, not chip-specific,
 *     but WHETHER this controller's driver actually uses that standard
 *     4-byte-header shape hasn't been independently confirmed the way
 *     the UART's protocol was (section 5) - this is inferred from the
 *     opcode value alone, not cross-referenced against the driver's own
 *     disassembly yet.
 *   - once an address is latched, each FIFO *read* returns the next byte
 *     from the backing image at that address (auto-incrementing) - or
 *     0xFF (typical erased-NOR-flash value) if no backing image was
 *     given.
 *   - status (offset 0x28, IM7CAM_SPI_REG_STATUS) always reads as
 *     IM7CAM_SPI_STATUS_IDLE (0x4) - this part *is* disassembly-confirmed
 *     (section 6): file offset 0xa140's poll is exactly
 *     `ldr r3,[r4,#0x28]; and r3,r3,#5; cmp r3,#4; bne back`, so 0x4
 *     satisfies it on the very first read, no busy/wait cycle modeled.
 * ============================================================ */

typedef struct Im7camSpiState {
    MemoryRegion iomem;
    uint8_t *flash_data;
    size_t flash_size;
    uint8_t cmd_buf[4];
    unsigned cmd_len;
    bool addr_latched;
    uint32_t read_addr;
    const uint8_t *resp_buf;
    unsigned resp_len;
    unsigned resp_pos;
} Im7camSpiState;

/* Real chip is an Eon/cFeon EN25QH64A (confirmed by the device-level
 * investigation - see qemu-fullhan-im7/BRINGUP-NOTES.md's "where the
 * firmware came from" section). JEDEC ID: manufacturer 0x1C (Eon) is
 * datasheet-confirmed; the 0x7017 memory-type+capacity pair is
 * independently cross-checked against flashrom's own flashchips.h
 * database entry for EN25QH64 (the same real-hardware EN25QH64A this
 * board's flash was actually dumped from was identified via flashrom
 * during the physical extraction, in the sibling openwrt-build-tools
 * investigation) - not a guess. Still unconfirmed whether U-Boot's probe
 * on *this* board checks it against a known-part table or just logs it
 * either way. */
static const uint8_t IM7CAM_SPI_JEDEC_ID[3] = { 0x1C, 0x70, 0x17 };
/* SPI NOR status register 1, all bits clear: not busy (WIP=0), write
 * enable latch clear, no block protection, no error - the standard idle
 * encoding for essentially any SPI NOR part, not chip-specific. */
static const uint8_t IM7CAM_SPI_STATUS_REG1[1] = { 0x00 };

static void im7cam_spi_load_image(Im7camSpiState *s, const char *path)
{
    gchar *buf = NULL;
    gsize len = 0;
    GError *gerr = NULL;

    if (!path) {
        info_report("im7cam: no SPI flash image given (IM7CAM_SPI_IMAGE) - "
                     "reads from the SPI controller will return 0xFF "
                     "(erased-flash value), U-Boot will not find a real "
                     "kernel/rootfs");
        return;
    }
    if (!g_file_get_contents(path, &buf, &len, &gerr)) {
        error_report("im7cam: could not read SPI image '%s': %s", path,
                      gerr->message);
        exit(1);
    }
    s->flash_data = (uint8_t *)buf;
    s->flash_size = len;
    info_report("im7cam: SPI flash backed by '%s' (%zu bytes)", path, len);
}

/* Pull one byte from whichever source is active: a canned command
 * response (RDID/RDSR) first, else the latched flash address, else the
 * erased-flash default. Shared by both 1-byte and multi-byte reads
 * below - a real 4-byte LDR against this FIFO pulls 4 *sequential* new
 * bytes, not the same byte replicated/zero-padded (found the hard way:
 * the JEDEC ID probe uses 32-bit-wide FIFO reads, ldr not ldrb, to drain
 * 4 bytes at a time - the original size-blind version of this function
 * returned only one real byte per access, zero-padded, and the guest
 * read back garbage IDs like "b0 e4 83 a0" as a result). */
static uint8_t im7cam_spi_next_byte(Im7camSpiState *s)
{
    if (s->resp_buf && s->resp_pos < s->resp_len) {
        return s->resp_buf[s->resp_pos++];
    }
    if (s->addr_latched) {
        uint8_t byte = 0xFF;

        if (s->flash_data && s->read_addr < s->flash_size) {
            byte = s->flash_data[s->read_addr];
        }
        s->read_addr++;
        return byte;
    }
    return 0xFF;
}

static uint64_t im7cam_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    Im7camSpiState *s = opaque;

    if (offset == IM7CAM_SPI_REG_STATUS) {
        return IM7CAM_SPI_STATUS_IDLE;
    }
    if (offset == IM7CAM_SPI_REG_AVAIL) {
        /* Always "1 byte available this chunk" - the simplest value that
         * makes the driver's own copy loop always progress by exactly
         * one real byte per poll and naturally terminate after the
         * right number of iterations (whatever the caller actually
         * asked for), with no risk of it ever reading further than
         * intended the way returning a large/guessed count could. Real
         * hardware almost certainly reports something closer to "how
         * full the FIFO actually is right now" - fine to refine later
         * if a real multi-byte-per-chunk case ever needs modeling, not
         * needed for anything seen so far. */
        return 1;
    }
    if (offset == IM7CAM_SPI_REG_DATA) {
        /* Exactly ONE FIFO byte per access, regardless of `size` - found
         * the hard way (section 10): the real byte-mode read loop
         * (file offset 0x9e80, `ldr r0,[fp]; strb r0,[r5],#1`) does a
         * full 32-bit `ldr` per byte but only ever keeps the low 8 bits,
         * discarding the other 24 and issuing a fresh `ldr` for the next
         * real byte - not one word-sized access consuming 4 new FIFO
         * bytes at once the way the *other* read path (file offset
         * 0x9e14, `sl==32`, genuinely 32-bit-wide FIFO draining) does.
         * The old code called im7cam_spi_next_byte() `size` times per
         * access unconditionally, silently burning 3 real response bytes
         * per guest read in exactly this byte-mode case - the actual
         * cause of the JEDEC ID coming back as `1c ff ff ff ff` instead
         * of `1c 70 17 ff ff` even after the response buffer itself was
         * confirmed correct (section 9). Upper bytes are irrelevant
         * here (the guest never reads them), so zero-filling instead of
         * replicating is an arbitrary but harmless choice. */
        return im7cam_spi_next_byte(s);
    }
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented READ  region=im7cam.spi "
                  "off=0x%" HWADDR_PRIx " size=%u\n", offset, size);
    return 0;
}

/* One byte shifted into the command/address accumulator. Split out from
 * im7cam_spi_write() so multi-byte writes (the ID-probe path uses a
 * plain 32-bit `str`, not `strb` - see the big comment above
 * IM7CAM_SPI_JEDEC_ID's declaration site) feed this the same way a real
 * byte-wide FIFO exposed through a wider register would: one new byte
 * per 8 bits of the access, LSB first. */
static void im7cam_spi_write_byte(Im7camSpiState *s, uint8_t byte)
{
    if (s->cmd_len == 0) {
        /* First byte of a new transaction: the opcode. RDID (0x9F) and
         * RDSR (0x05) have no address phase at all - the flash starts
         * shifting a canned response back on the very next clock, unlike
         * READ/FAST_READ's 3-byte address phase below. Both confirmed
         * needed by real trace output: without this, U-Boot's own flash
         * probe read garbage left over from a previous READ command's
         * address-latched state and reported a bogus manufacturer ID. */
        s->cmd_buf[s->cmd_len++] = byte;
        switch (byte) {
        case 0x9F:
            s->resp_buf = IM7CAM_SPI_JEDEC_ID;
            s->resp_len = sizeof(IM7CAM_SPI_JEDEC_ID);
            s->resp_pos = 0;
            qemu_log_mask(LOG_UNIMP, "im7cam: spi RDID opcode received\n");
            break;
        case 0x05:
            s->resp_buf = IM7CAM_SPI_STATUS_REG1;
            s->resp_len = sizeof(IM7CAM_SPI_STATUS_REG1);
            s->resp_pos = 0;
            qemu_log_mask(LOG_UNIMP, "im7cam: spi RDSR opcode received\n");
            break;
        default:
            /* Deliberately does NOT clear a pending resp_buf here
             * anymore (an earlier version of this comment/code did).
             * Live gdbstub tracing (single-stepped ~90 instructions past
             * the confirmed opcode write) found the real caller is a
             * loop that re-invokes this same byte-transmit path multiple
             * times per probe, toggling the offset-8 open/close bracket
             * (which resets cmd_len) between calls - so a *later*,
             * unrelated byte in the same overall probe was hitting this
             * default case and wiping the RDID response before U-Boot's
             * own read-back loop ran, same root problem as the offset-8
             * fix below just reached through a different call, not
             * fixed by that change alone. Still unconfirmed whether this
             * fully resolves the "SF: Unsupported manufacturer" symptom
             * - see BRINGUP-NOTES.md section 8/9 for the current status. */
            qemu_log_mask(LOG_UNIMP,
                          "im7cam: spi first-byte opcode=0x%02x "
                          "(unrecognized)\n", byte);
            break;
        }
        return;
    }
    if (!s->addr_latched && s->cmd_len < sizeof(s->cmd_buf)) {
        s->cmd_buf[s->cmd_len++] = byte;
        if (s->cmd_len == sizeof(s->cmd_buf) &&
            (s->cmd_buf[0] == 0x03 || s->cmd_buf[0] == 0x0B)) {
            s->read_addr = ((uint32_t)s->cmd_buf[1] << 16) |
                            ((uint32_t)s->cmd_buf[2] << 8) |
                            s->cmd_buf[3];
            s->addr_latched = true;
            qemu_log_mask(LOG_UNIMP,
                          "im7cam: spi read command, opcode=0x%02x "
                          "addr=0x%06x\n", s->cmd_buf[0], s->read_addr);
        }
    }
}

static void im7cam_spi_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    Im7camSpiState *s = opaque;

    if (offset == 0x08 && value == 0) {
        /* NOT a full protocol reset, despite looking like one at first
         * (see the big comment above im7cam_spi_write_byte(), which
         * originally cleared resp_buf here too). Live gdbstub tracing
         * (a write watchpoint on the FIFO address, single-stepped)
         * caught the real sequence: opcode 0x9F genuinely reaches this
         * device (confirmed - "im7cam: spi RDID opcode received" logs),
         * but offset 8 gets toggled 0-then-1 again immediately after,
         * as a routine open/close bracket around a buffer op unrelated
         * to the pending RDID response - clearing resp_buf here wiped
         * the JEDEC ID before U-Boot's own read-back loop ever ran,
         * which is exactly why "SF: Unsupported manufacturer" kept
         * reporting stale/garbage bytes even after the opcode handling
         * above was added. cmd_len/addr_latched (the READ/FAST_READ
         * address-phase accumulator) still reset here - only resp_buf's
         * lifetime changed. */
        s->cmd_len = 0;
        s->addr_latched = false;
        return;
    }
    if (offset == IM7CAM_SPI_REG_DATA) {
        /* Same fix as the read side (see the big comment in
         * im7cam_spi_read()): one real FIFO byte per access, not `size`.
         * The confirmed byte-mode write loop (file offset 0xa18c:
         * `ldrb r1,[r2],#1; str r1,[r4,#0x60]`) zero-extends one real
         * byte into r1 and does a plain 32-bit `str` per real byte -
         * treating that as 4 accumulator bytes (1 real + 3 phantom
         * zeros) hadn't visibly broken anything yet only because the ID
         * probe's opcode byte happens to survive 3 harmless trailing
         * zero-bytes in the accumulator, but a real multi-byte
         * READ/FAST_READ command (opcode + 3 address bytes, each its
         * own separate `str` this same way) would have filled cmd_len
         * to its 4-byte cap after just the *first* real byte, silently
         * dropping bytes 2-4 (the actual address) - not yet observed
         * because nothing has exercised a real flash READ command this
         * way yet, but the exact same bug class as the one that broke
         * RDID above, so fixed here too rather than waiting to hit it
         * for real. */
        im7cam_spi_write_byte(s, (uint8_t)value);
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented WRITE region=im7cam.spi "
                  "off=0x%" HWADDR_PRIx " size=%u val=0x%" PRIx64 "\n",
                  offset, size, value);
}

static const MemoryRegionOps im7cam_spi_ops = {
    .read = im7cam_spi_read,
    .write = im7cam_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void im7cam_add_spi(MemoryRegion *sysmem, const char *image_path)
{
    Im7camSpiState *s = g_new0(Im7camSpiState, 1);

    im7cam_spi_load_image(s, image_path);
    memory_region_init_io(&s->iomem, NULL, &im7cam_spi_ops, s,
                           "im7cam.spi", IM7CAM_SPI_SIZE);
    memory_region_add_subregion(sysmem, IM7CAM_SPI_BASE, &s->iomem);
}

/* ============================================================
 * Free-running counter, backed by QEMU's own virtual clock so it's
 * always genuinely advancing - just enough for guest code's own
 * elapsed-time/timeout math to eventually see time pass, not a real
 * modeled reload/prescaler/IRQ timer. Nothing here is chip-specific;
 * this is the generic "any wait-with-timeout loop just needs *a*
 * monotonically increasing value" fix, applicable regardless of what
 * this register block's real tick rate or width turns out to be.
 * ============================================================ */

static uint64_t im7cam_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    if (offset == IM7CAM_TIMER_REG_COUNT) {
        return (uint32_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented READ  region=im7cam.timer "
                  "off=0x%" HWADDR_PRIx " size=%u\n", offset, size);
    return 0;
}

static void im7cam_timer_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented WRITE region=im7cam.timer "
                  "off=0x%" HWADDR_PRIx " size=%u val=0x%" PRIx64 "\n",
                  offset, size, value);
}

static const MemoryRegionOps im7cam_timer_ops = {
    .read = im7cam_timer_read,
    .write = im7cam_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void im7cam_add_timer(MemoryRegion *sysmem)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);

    memory_region_init_io(mr, NULL, &im7cam_timer_ops, NULL, "im7cam.timer",
                           IM7CAM_TIMER_SIZE);
    memory_region_add_subregion(sysmem, IM7CAM_TIMER_BASE, mr);
}

/* ============================================================
 * Reset/clock-management block - see the big comment above
 * IM7CAM_RESET_BASE. Only one register confirmed: writes to the ack
 * register are accepted (and logged, in case a real driver cares what
 * gets written), reads from it always report "acked" (0xFFFFFFFF) -
 * every sub-block reset this file has seen so far just needs to look
 * instantly complete, no real reset sequencing modeled. Every other
 * register in this block falls through to the same logging stub as
 * every other unmodeled peripheral.
 * ============================================================ */

static uint64_t im7cam_reset_ctrl_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    if (offset == IM7CAM_RESET_REG_ACK) {
        return 0xFFFFFFFF;
    }
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented READ  region=im7cam.reset-ctrl "
                  "off=0x%" HWADDR_PRIx " size=%u\n", offset, size);
    return 0;
}

static void im7cam_reset_ctrl_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    if (offset == IM7CAM_RESET_REG_ACK) {
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "im7cam: unimplemented WRITE region=im7cam.reset-ctrl "
                  "off=0x%" HWADDR_PRIx " size=%u val=0x%" PRIx64 "\n",
                  offset, size, value);
}

static const MemoryRegionOps im7cam_reset_ctrl_ops = {
    .read = im7cam_reset_ctrl_read,
    .write = im7cam_reset_ctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void im7cam_add_reset_ctrl(MemoryRegion *sysmem)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);

    memory_region_init_io(mr, NULL, &im7cam_reset_ctrl_ops, NULL,
                           "im7cam.reset-ctrl", IM7CAM_RESET_SIZE);
    memory_region_add_subregion(sysmem, IM7CAM_RESET_BASE, mr);
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
    im7cam_add_spi(sysmem, getenv("IM7CAM_SPI_IMAGE"));
    im7cam_add_timer(sysmem);
    im7cam_add_reset_ctrl(sysmem);
    im7cam_add_unimp_region(sysmem, "im7cam.i2c0-guess", IM7CAM_I2C0_BASE,
                             IM7CAM_PERIPH_STUB_SIZE);
    im7cam_add_unimp_region(sysmem, "im7cam.gpio0-guess", IM7CAM_GPIO0_BASE,
                             IM7CAM_PERIPH_STUB_SIZE);
    im7cam_add_unimp_region(sysmem, "im7cam.spi0-guess-unused",
                             IM7CAM_SPI0_LABELED_BASE,
                             IM7CAM_PERIPH_STUB_SIZE);
    im7cam_add_unimp_region(sysmem, "im7cam.unk-0xf0d00000",
                             IM7CAM_UNK_D_BASE, IM7CAM_PERIPH_STUB_SIZE);

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
