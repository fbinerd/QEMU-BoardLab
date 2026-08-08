#include "../common/semihosting.h"

/*
 * Demonstrates (and documents) a QEMU TCG behavior that matters for every
 * other experiment in this repo: QEMU's default CPU emulation backend
 * (TCG, not KVM) translates guest code into host instructions in blocks,
 * and automatically invalidates + re-translates a block if the guest
 * writes into the memory it was translated from. That means self-modifying
 * code "just works" under QEMU TCG - the emulator always executes the
 * latest bytes - regardless of whether real hardware would need explicit
 * DSB/ISB/I-cache-invalidate barriers to see the update. Real ARM Cortex-A
 * cores do NOT do this for free: a write to memory the CPU has already
 * fetched/decoded/is mid-executing is not required to be visible to
 * in-flight instruction fetch without those barriers.
 *
 * This program overwrites a function's own following instruction with a
 * *different* instruction (not the crash-path "identical bytes" case -
 * this is the maximally-detectable case) immediately before falling
 * through to it, with zero cache maintenance. On real hardware this is
 * exactly the kind of thing that can execute stale/undefined behavior. In
 * QEMU TCG, expect it to correctly execute the *new* instruction every
 * time - proving this environment cannot be used, by itself, to validate
 * whether a fix to the real self-modifying-code hazard actually works.
 */

/* Two instructions: the first one gets overwritten right before we reach
 * it. `mov r0, #1` (return "unpatched") is patched in-place to `mov r0,
 * #2` (return "patched") using a plain store - no barriers at all. */
__attribute__((naked, aligned(4)))
static int victim(void)
{
	__asm__ volatile(
		"mov r0, #1\n"
		"bx lr\n"
	);
}

int main(void)
{
	unsigned int *victim_insn = (unsigned int *)victim;
	unsigned int patched_insn;
	int result;

	print_line("arm-selfmod-lab: experiment 01");
	print_line("QEMU TCG self-modifying-code auto-invalidation check");
	print_str("victim() first instruction before patch: ");
	print_hex32(victim_insn[0]);
	sh_writec('\n');

	/* Encode "mov r0, #2" (A32): same form as "mov r0, #1" with the
	 * immediate field changed. Both are simple MOV (immediate)
	 * encodings, differing only in the 8-bit immediate. */
	patched_insn = (victim_insn[0] & ~0xffu) | 0x02u;

	/* Plain store. No flush_dcache_range(), no invalidate_icache_all(),
	 * no DSB/ISB - deliberately, to mirror the vendor's unprotected
	 * memcpy() on the real hardware path. */
	victim_insn[0] = patched_insn;

	print_str("victim() first instruction after patch:  ");
	print_hex32(victim_insn[0]);
	sh_writec('\n');

	result = victim();

	print_str("victim() returned: ");
	print_hex32((unsigned int)result);
	sh_writec('\n');

	if (result == 2) {
		print_line("RESULT: QEMU TCG saw the patched instruction (expected).");
		print_line("This confirms QEMU alone cannot validate the real hardware hazard -");
		print_line("see README.md. Use this environment for control-flow logic only.");
	} else {
		print_line("RESULT: QEMU TCG executed the *stale* instruction.");
		print_line("Unexpected for this QEMU version/config - worth investigating,");
		print_line("it would mean this environment can partially model the hazard.");
	}

	return 0;
}
