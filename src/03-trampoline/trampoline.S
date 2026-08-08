/*
 * Redirect target for the appsbl RAM-boot stack-smash: a tiny, position-
 * independent-enough (fixed load address, embedded at UPFILE_BUFFER_ADDRESS
 * + 0x14, inside the unused "product" header field) trampoline that sets
 * up the arguments run_command() expects and jumps straight into it - the
 * *unmodified* vendor function, at its address in the reference build,
 * never overwritten by anything we do. No self-modifying code involved:
 * this never touches currently-executing memory, it only gets *jumped to*
 * once, from a hijacked return address.
 */
	.arm
	.text
	.global start
start:
	ldr r0, =cmd_string
	mov r1, #0
	ldr pc, =0x4a9396c8    @ run_command(), reference-build address
cmd_string:
	.asciz "bootm 0x44001014"
