#ifndef SEMIHOSTING_H
#define SEMIHOSTING_H

void sh_write0(const char *str);
void sh_writec(char c);
void sh_exit(int code) __attribute__((noreturn));

void print_hex32(unsigned int v);
void print_str(const char *s);
void print_line(const char *s);

#endif
