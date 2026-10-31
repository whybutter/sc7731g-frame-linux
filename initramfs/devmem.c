// SPDX-License-Identifier: GPL-2.0-only
/*
 * devmem ADDRESS [32 [VALUE]] -- read or write one 32-bit word of physical
 * memory through /dev/mem, busybox's syntax (width must be 32).
 *
 * Alpine's busybox-static is built without devmem, and the first probe boot
 * (2026-10-24) printed "devmem: not found" for every register it meant to
 * read. The build image has the kernel's cross gcc but no ARM libc, so this
 * is freestanding: raw ARM EABI syscalls, no headers, -nostdlib (see
 * build/kernel.sh).
 */

#define O_RDONLY	0
#define O_RDWR		2
#define O_SYNC		04010000
#define PROT_READ	1
#define PROT_WRITE	2
#define MAP_SHARED	1

static long sys(long nr, long a, long b, long c, long d, long e, long f)
{
	register long r7 asm("r7") = nr;
	register long r0 asm("r0") = a;
	register long r1 asm("r1") = b;
	register long r2 asm("r2") = c;
	register long r3 asm("r3") = d;
	register long r4 asm("r4") = e;
	register long r5 asm("r5") = f;

	asm volatile("svc #0" : "+r"(r0)
		     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
		     : "memory");
	return r0;
}

#define sys_exit(c)		sys(1, c, 0, 0, 0, 0, 0)
#define sys_write(fd, b, n)	sys(4, fd, (long)(b), n, 0, 0, 0)
#define sys_open(p, fl)		sys(5, (long)(p), fl, 0, 0, 0, 0)
/* mmap2 takes the offset in 4 KiB pages */
#define sys_mmap2(len, prot, fl, fd, pg) sys(192, 0, len, prot, fl, fd, pg)

static void say(int fd, const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	sys_write(fd, s, n);
}

static unsigned long num(const char *s)
{
	unsigned long v = 0, base = 10;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	for (; *s; s++) {
		int d = *s >= '0' && *s <= '9' ? *s - '0' :
			*s >= 'a' && *s <= 'f' ? *s - 'a' + 10 :
			*s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : 99;
		if (d >= (int)base)
			break;
		v = v * base + d;
	}
	return v;
}

static int main(int argc, char **argv)
{
	unsigned long addr, val;
	volatile unsigned int *p;
	char out[12] = "0x00000000\n";
	long fd, map;
	int i, w;

	if (argc < 2 || (argc > 2 && num(argv[2]) != 32)) {
		say(2, "usage: devmem ADDRESS [32 [VALUE]]\n");
		return 2;
	}
	w = argc > 3;
	addr = num(argv[1]);
	fd = sys_open("/dev/mem", (w ? O_RDWR : O_RDONLY) | O_SYNC);
	if (fd < 0) {
		say(2, "devmem: cannot open /dev/mem\n");
		return 1;
	}
	map = sys_mmap2(0x1000, PROT_READ | (w ? PROT_WRITE : 0), MAP_SHARED,
			fd, addr >> 12);
	if ((unsigned long)map >= -4095UL) {
		say(2, "devmem: mmap failed\n");
		return 1;
	}
	p = (volatile unsigned int *)(map + (addr & 0xfff));
	if (w) {
		*p = num(argv[3]);
		return 0;
	}
	val = *p;
	for (i = 9; i >= 2; i--, val >>= 4)
		out[i] = "0123456789ABCDEF"[val & 0xf];
	say(1, out);
	return 0;
}

/* the kernel's initial stack: argc, then argv[] */
void __attribute__((naked, noreturn)) _start(void)
{
	asm volatile("ldr r0, [sp]\n\t"
		     "add r1, sp, #4\n\t"
		     "bl start_c");
}

void __attribute__((noreturn, used)) start_c(int argc, char **argv)
{
	sys_exit(main(argc, argv));
	for (;;)
		;
}
