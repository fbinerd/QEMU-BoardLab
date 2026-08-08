/* ARM semihosting helpers (A32). QEMU implements the semihosting spec
 * regardless of "real hardware" - these calls trap to the emulator, not to
 * any guest OS, so they work fine in a bare-metal image. */

	.arm
	.section .text.semihosting

/* void sh_write0(const char *str)  -- str in r0 */
	.global sh_write0
sh_write0:
	mov r1, r0
	mov r0, #0x04        @ SYS_WRITE0
	svc 0x123456
	bx lr

/* void sh_writec(char c) -- c in r0 (low byte) */
	.global sh_writec
sh_writec:
	push {r0}
	mov r1, sp
	mov r0, #0x03         @ SYS_WRITEC
	svc 0x123456
	pop {r0}
	bx lr

/* noreturn void sh_exit(int code) -- code in r1 (crt0 puts it there) */
	.global sh_exit
sh_exit:
	ldr r2, =exit_block
	str r1, [r2, #4]       @ exit_block[1] = code
	mov r1, r2
	mov r0, #0x18          @ SYS_EXIT
	svc 0x123456
1:	b 1b                   @ should never return; spin if it does

	.section .data
	.align 2
exit_block:
	.word 0x20026          @ ADP_Stopped_ApplicationExit
	.word 0                @ exit code goes here
