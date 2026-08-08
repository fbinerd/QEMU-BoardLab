#include "../common/semihosting.h"

/*
 * Validates the control-flow mechanics of a redirect-via-stack fix for the
 * real APPSBL RAM-boot bug, without touching the cache-coherency question
 * experiment 01 shows QEMU can't model anyway.
 *
 * On the real device, the crash dump shows `sp` sitting inside our
 * writable upload range, and reached *before* the .text region the
 * self-modifying-code crash happens in. That means we can, in principle,
 * plant a crafted return address at the exact stack offset handle_upfile's
 * saved lr lives at, redirecting execution to our own (barrier-protected)
 * code the moment handle_upfile's current call returns - well before the
 * transfer ever reaches the address range that crashes.
 *
 * This experiment proves the *mechanic* works: a function whose saved
 * return address gets overwritten mid-call does jump to the new target
 * instead of its real caller when it returns - not whether the specific
 * offset math against a real firmware image is correct (that still needs
 * the real device or a disassembly-derived offset).
 */

extern void vulnerable_fn(void);

static int reached_original_return;
static int reached_stub;

void stub(void);

/* Called from vulnerable.S with r0 = &saved_lr on the stack. Stands in for
 * the vendor's memcpy() loop: instead of copying attacker bytes packet by
 * packet, it goes straight to the end state - the saved return address
 * has been overwritten with the address of `stub`. */
void network_write_sim(unsigned int *saved_lr_slot)
{
	print_str("network_write_sim: saved lr slot at ");
	print_hex32((unsigned int)saved_lr_slot);
	sh_writec('\n');
	print_str("network_write_sim: original saved lr was ");
	print_hex32(*saved_lr_slot);
	sh_writec('\n');

	*saved_lr_slot = (unsigned int)stub;

	print_line("network_write_sim: overwrote it with &stub");
}

__attribute__((noinline))
void stub(void)
{
	reached_stub = 1;
	print_line("stub: redirect worked - this is our own code running");
	print_line("stub: on the real device, this is where a barrier-protected");
	print_line("stub: copy of the remaining overlay would run, safely.");
	sh_exit(reached_stub && !reached_original_return ? 0 : 1);
}

int main(void)
{
	print_line("arm-selfmod-lab: experiment 02");
	print_line("Stack-redirect control-flow mechanics check");

	vulnerable_fn();

	/* Only reached if the redirect did NOT happen (i.e. the original
	 * saved lr survived) - vulnerable_fn() returned normally here. */
	reached_original_return = 1;
	print_line("RESULT: FAIL - vulnerable_fn() returned normally.");
	print_line("The stack overwrite did not redirect control flow.");
	return 1;
}
