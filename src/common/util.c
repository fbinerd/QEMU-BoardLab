#include "semihosting.h"

void print_str(const char *s)
{
	sh_write0(s);
}

void print_line(const char *s)
{
	sh_write0(s);
	sh_writec('\n');
}

void print_hex32(unsigned int v)
{
	char buf[11];
	const char *digits = "0123456789abcdef";
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 8; i++)
		buf[2 + i] = digits[(v >> ((7 - i) * 4)) & 0xf];
	buf[10] = '\0';
	sh_write0(buf);
}
