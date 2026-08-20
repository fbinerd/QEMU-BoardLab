/* Minimal PID 1 for the IM7 reset-button recovery initramfs. */

#define SYS_READ 3
#define SYS_WRITE 4
#define SYS_SYNC 36
#define SYS_REBOOT 88

#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793
#define LINUX_REBOOT_CMD_RESTART 0x01234567

static long syscall3(long number, long arg0, long arg1, long arg2)
{
    register long r0 asm("r0") = arg0;
    register long r1 asm("r1") = arg1;
    register long r2 asm("r2") = arg2;
    register long r7 asm("r7") = number;

    asm volatile("svc 0"
                 : "+r"(r0)
                 : "r"(r1), "r"(r2), "r"(r7)
                 : "memory");
    return r0;
}

static unsigned int text_length(const char *text)
{
    unsigned int length = 0;

    while (text[length]) {
        length++;
    }
    return length;
}

static void print(const char *text)
{
    syscall3(SYS_WRITE, 1, (long)text, text_length(text));
}

static int text_equal(const char *left, const char *right)
{
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

void _start(void)
{
    char command[64];
    unsigned int used = 0;

    print("\nIM7 RECOVERY INITRAMFS: boot pelo botao RESET confirmado\n");
    print("Kernel GPL Fullhan 3.0.8; console de recuperacao independente.\n");
    print("Digite help para ver os comandos.\n\nrecovery> ");

    for (;;) {
        char byte;
        long count = syscall3(SYS_READ, 0, (long)&byte, 1);

        if (count != 1) {
            continue;
        }
        if (byte != '\n' && byte != '\r') {
            if (used + 1 < sizeof(command)) {
                command[used++] = byte;
            }
            continue;
        }
        if (!used) {
            continue;
        }
        command[used] = '\0';
        print("\n");
        if (text_equal(command, "help")) {
            print("help    mostra esta ajuda\n");
            print("info    mostra a origem deste ambiente\n");
            print("reboot  reinicia a placa\n");
        } else if (text_equal(command, "info")) {
            print("U-Boot OEM modificado somente no caminho GPIO23/TFTP.\n");
            print("Servidor TFTP: 192.168.2.10; cliente: 192.168.2.108.\n");
        } else if (text_equal(command, "reboot") ||
                   text_equal(command, "reset")) {
            print("Reiniciando...\n");
            syscall3(SYS_SYNC, 0, 0, 0);
            syscall3(SYS_REBOOT, LINUX_REBOOT_MAGIC1,
                     LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART);
        } else {
            print("Comando desconhecido. Digite help.\n");
        }
        used = 0;
        print("recovery> ");
    }
}
