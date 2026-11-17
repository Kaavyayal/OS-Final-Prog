/* Linux x86-64 syscall ABI; no libc or dynamic loader required. */
#define PAGE 4096L
static long syscall6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long result;
    __asm__ volatile("syscall" : "=a"(result)
        : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return result;
}
static void say(const char *s) {
    long len = 0;
    while (s[len]) ++len;
    syscall6(1, 1, (long)s, len, 0, 0, 0);
}
static void finish(long status) {
    syscall6(60, status, 0, 0, 0, 0, 0);
    for (;;) {}
}
static void check(int condition, const char *message) {
    if (!condition) {
        say("*** FAIL: ");
        say(message);
        say("\n");
        finish(1);
    }
}
static volatile unsigned char *map(long fd, long length, long offset) {
    /* mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset) */
    long result = syscall6(9, 0, length, 3, 1, fd, offset);
    check((unsigned long)result < (unsigned long)-4095L, "mmap");
    return (volatile unsigned char *)result;
}
void test_main(void) {
    long fd = syscall6(2, (long)"shared.dat", 2, 0, 0, 0, 0);
    check(fd >= 0, "open shared.dat");
    volatile unsigned char *whole = map(fd, 3 * PAGE, 0);
    volatile unsigned char *alias = map(fd, 2 * PAGE, PAGE);
    /* Fault both mappings before either writes: catches copied pages. */
    check(whole[0] == 'A' && whole[PAGE] == 'B' &&
          whole[2 * PAGE] == 'C' && alias[0] == 'B' &&
          alias[PAGE] == 'C', "initial contents and offset");
    say("*** initial contents and offsets OK\n");
    whole[PAGE + 17] = 'X';
    check(alias[17] == 'X', "first mapping to alias");
    alias[PAGE + 29] = 'Y';
    check(whole[2 * PAGE + 29] == 'Y', "alias to first mapping");
    check(whole[17] == 'A' && whole[PAGE + 29] == 'B',
          "unrelated pages unchanged");
    say("*** shared aliases agree in both directions\n");
    unsigned char byte = 0;
    check(syscall6(8, fd, PAGE + 17, 0, 0, 0, 0) == PAGE + 17,
          "seek to modified byte");
    check(syscall6(0, fd, (long)&byte, 1, 0, 0, 0) == 1 && byte == 'X',
          "read observes mapped store");
    say("*** read observes shared mapping updates\n");
    check(syscall6(11, (long)whole, 3 * PAGE, 0, 0, 0, 0) == 0,
          "unmap first mapping");
    check(alias[17] == 'X' && alias[PAGE + 29] == 'Y',
          "remaining alias survives unmap");
    volatile unsigned char *again = map(fd, 3 * PAGE, 0);
    check(again[PAGE + 17] == 'X' && again[2 * PAGE + 29] == 'Y',
          "new mapping observes cached updates");
    say("*** unmap and remap preserve shared updates\n");
    long other = syscall6(2, (long)"shared.dat", 2, 0, 0, 0, 0);
    check(other >= 0, "reopen");
    volatile unsigned char *separate = map(other, PAGE, PAGE);
    check(separate[17] == 'X', "separate open shares cache");
    byte = 'W';
    check(syscall6(8, other, PAGE + 17, 0, 0, 0, 0) == PAGE + 17, "write seek");
    check(syscall6(1, other, (long)&byte, 1, 0, 0, 0) == 1, "file write");
    check(alias[17] == 'W' && separate[17] == 'W', "write updates mappings");
    long priv = syscall6(9, 0, PAGE, 3, 2, other, PAGE);
    check((unsigned long)priv < (unsigned long)-4095L, "private mmap");
    volatile unsigned char *private = (volatile unsigned char *)priv;
    check(private[17] == 'W', "private initial data");
    private[17] = 'P';
    check(separate[17] == 'W', "private store stays private");
    say("*** file writes and private isolation OK\n");
    long anon = syscall6(9, 0, 2 * PAGE, 3, 33, -1, 0);
    check((unsigned long)anon < (unsigned long)-4095L, "shared anonymous mmap");
    volatile unsigned char *shared = (volatile unsigned char *)anon;
    shared[0] = 7;
    long child = syscall6(57, 0, 0, 0, 0, 0, 0);
    check(child >= 0, "fork");
    if (!child) {
        separate[18] = 'F';
        shared[0] = 8;
        shared[PAGE] = 9;
        private[17] = 'C';
        finish(0);
    }
    int status = -1;
    check(syscall6(61, child, (long)&status, 0, 0, 0, 0) == child && !status,
          "wait child");
    check(alias[18] == 'F' && shared[0] == 8 && shared[PAGE] == 9,
          "fork shares resident and lazy pages");
    check(private[17] == 'P', "fork keeps private mapping isolated");
    check(syscall6(11, anon, 2 * PAGE, 0, 0, 0, 0) == 0, "unmap anonymous");
    check(syscall6(11, priv, PAGE, 0, 0, 0, 0) == 0, "unmap private");
    check(syscall6(11, (long)separate, PAGE, 0, 0, 0, 0) == 0, "unmap separate");
    check(syscall6(3, other, 0, 0, 0, 0, 0) == 0, "close separate");
    say("*** fork shares file and anonymous mappings OK\n");
    check(syscall6(11, (long)alias, 2 * PAGE, 0, 0, 0, 0) == 0,
          "unmap alias");
    check(syscall6(11, (long)again, 3 * PAGE, 0, 0, 0, 0) == 0,
          "unmap replacement");
    check(syscall6(3, fd, 0, 0, 0, 0, 0) == 0, "close");
    say("*** MAP_SHARED test passed\n");
    finish(0);
}
__asm__(".global _start\n_start:\nxor %rbp,%rbp\nandq $-16,%rsp\ncall test_main\nud2\n");
