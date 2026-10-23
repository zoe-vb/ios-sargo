/* Stage 1: minimal C PID 1 for a ramdisk, built static by tools/mkinit.py.
 * No libc, no globals: raw XNU syscalls (svc #0x80, number in x16). */
static long sc3(long n, long a, long b, long c) {
	register long x0 asm("x0")=a, x1 asm("x1")=b, x2 asm("x2")=c, x16 asm("x16")=n, r asm("x0");
	asm volatile("svc #0x80" : "=r"(r) : "r"(x0),"r"(x1),"r"(x2),"r"(x16) : "memory","cc");
	return r;
}
static long slen(const char *s){ long n=0; while(s[n]) n++; return n; }
void start(void) {
	int fd = (int)sc3(5, (long)"/dev/console", 2, 0);		/* open RDWR */
	const char *hi = "SWEET GPU RDTEST: hello from static C init, pid ";
	sc3(4, fd, (long)hi, slen(hi));					/* write */
	char d = (char)('0' + (int)sc3(20, 0, 0, 0));			/* getpid */
	sc3(4, fd, (long)&d, 1);
	const char *nl = "\nSWEET GPU RDTEST: init alive\n";
	sc3(4, fd, (long)nl, slen(nl));
	for (;;) { sc3(93, 0, 0, 0); }					/* select: idle, never exit */
}
