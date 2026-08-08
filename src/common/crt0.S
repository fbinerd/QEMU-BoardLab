/* Minimal ARM (A32) startup: set the stack pointer, call main(), then exit
 * cleanly via semihosting with main()'s return value as the exit code. */

	.arm
	.section .text.start
	.global _start
_start:
	ldr sp, =_stack_top
	bl main
	mov r1, r0
	b sh_exit
