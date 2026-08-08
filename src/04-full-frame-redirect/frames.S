/*
 * Faithful reproduction of the real vendor frame shapes, unlike
 * experiment 02's simplified single-register push{lr}/pop{pc}. The real
 * memcpy() does `push {r4,r5,lr}` and returns via `pop {r4,r5,pc}`; the
 * real handle_upfile() does `push {r4,r5,r6,r7,r8,lr}` and returns via
 * `pop {r4,r5,r6,r7,r8,pc}` - see the appsbl project's gen_payload.py
 * comment for the disassembly-derived offsets this mirrors.
 *
 * The open question experiment 02 didn't answer: does clobbering
 * handle_upfile's saved r4-r8 (the 20-byte gap between memcpy's own
 * saved-lr and handle_upfile's saved-pc, left as 0xFF filler in the real
 * payload) break the redirect - e.g. because something downstream of the
 * redirect target expects those callee-saved registers to hold sane
 * values, per AAPCS.
 */
	.arm
	.section .text.frames

	.global memcpy_sim
memcpy_sim:
	push {r4, r5, lr}
	mov r0, sp              @ r0 = &memcpy's own saved-lr-region (sp here == "sp" in the crash dump)
	bl network_write_sim    @ stands in for the per-packet memcpy() loop
	pop {r4, r5, pc}

	.global handle_upfile_sim
handle_upfile_sim:
	push {r4, r5, r6, r7, r8, lr}
	bl memcpy_sim
	pop {r4, r5, r6, r7, r8, pc}
