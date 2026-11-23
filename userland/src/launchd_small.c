/*
 * /sbin/launchd of the small image when it is the root itself (rd=md0, no system image).
 *
 * The bring-up normally boots the big system image as md0 and this image as md1, mounted on
 * /redwood by stage0 (see stage0.c).  A run that only exercises the kernel's drivers has no use
 * for 2.4 GB of upload, so it boots this image alone -- and then the kernel's init program is
 * /sbin/launchd of this image, which was not there: the run died at "Process 1 exec of
 * /sbin/launchd failed" 80 ms in, before the drivers' threads had done anything.
 *
 * What process 1 must do here, and what it must not do -- each of these cost a run:
 *   - it must not exit: the kernel panics with "initproc failed to start", which ends the run;
 *   - it must not reboot() or halt: the kernel is brought back to the loader by the loader's own
 *     timer (loader_return_after, XL_SECS in xl), and halting from here means the loader never
 *     collects the log.  The run then looks exactly like a kernel that hung -- no answer, no log;
 *   - it must not call select(): a select() from process 1 here dies with a coredump (SIGBUS) a
 *     few milliseconds in, taking the run with it -- which is also why hello.c never waits as long
 *     as its hello_wait asks;
 *   - it must not spin without ever blocking: a busy "yield" loop kept the phone from coming back
 *     to the loader at all, so the run again looked hung with no log;
 *   - it must not call sysctlbyname() on the drivers' names: those handlers touch hardware (the
 *     battery's reads the fuel gauge over SPMI) and can stop the phone dead early in the boot.
 *
 * So: one open("/dev/console"), one write, then block forever in a read() on it.  Written exactly
 * like hello.c -- a static arm64 Mach-O, no libc, no dyld, no libmini, raw system calls, entry
 * point start() -- because that is the one program known to run as process 1 here.
 */
typedef unsigned long u64;

/* XNU arm64 system call: number in x16, arguments in x0.., "svc #0x80"; carry set on error */
static long
syscall3(long num, long a, long b, long c)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2) : "memory", "cc");
	return x0;
}

#define SYS_read    3
#define SYS_write   4
#define SYS_open    5
#define O_RDWR      2

static unsigned
length(const char *s)
{
	unsigned n = 0;

	while (s[n]) {
		n++;
	}
	return n;
}

static void
put(int fd, const char *s)
{
	syscall3(SYS_write, fd, (long)s, length(s));
}

void
start(void)
{
	int fd = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);

	put(fd, "\n=== launchd of the small image is here and will stay ===\n");

	/*
	 * The run ends on the loader's timer, not here, so process 1 has to stay alive and asleep.  A read on the
	 * console blocks until a character arrives, and on a serial console with nobody typing that is forever.
	 */
	for (;;) {
		char c;

		if (syscall3(SYS_read, fd, (long)&c, 1) < 0) {
			/* A console that does not block: sleep by spinning, but not forever in one go. */
			for (unsigned long i = 0; i < 5000000ul; i++) {
				__asm__ volatile ("yield");
			}
		}
	}
}
