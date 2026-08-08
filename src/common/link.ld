/* QEMU's arm "virt" machine maps RAM starting at 0x40000000. */
ENTRY(_start)

MEMORY
{
	RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 64M
}

SECTIONS
{
	. = 0x40000000;

	.text.start : { *(.text.start) } > RAM
	.text : { *(.text.semihosting) *(.text*) } > RAM
	.rodata : { *(.rodata*) } > RAM
	.data : { *(.data*) } > RAM
	.bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM

	. = ALIGN(16);
	.stack (NOLOAD) : {
		. += 0x10000;
		_stack_top = .;
	} > RAM
}
