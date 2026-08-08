/*
 * Stand-in for handle_upfile()'s call frame. Mirrors the real function's
 * shape closely enough to matter: it pushes lr (its own return address)
 * onto the stack, calls something that - on the real device - is the
 * vendor's memcpy() writing attacker-controlled network bytes into a
 * buffer that eventually reaches this exact stack slot, then pops pc to
 * return. If the saved lr has been overwritten by then, "returning"
 * actually jumps wherever the attacker placed.
 */
	.arm
	.section .text.vulnerable

	.global vulnerable_fn
vulnerable_fn:
	push {lr}
	mov r0, sp              @ r0 = &saved_lr, the "reachable stack slot"
	bl network_write_sim    @ stand-in for the real per-packet memcpy() loop
	pop {pc}                @ returns to caller, OR to wherever got planted
