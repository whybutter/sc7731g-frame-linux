// SPDX-License-Identifier: GPL-2.0-only
/*
 * i2cxfer BUS ADDR REG LEN -- write the register byte REG to I2C address
 * ADDR on /dev/i2c-BUS, then read LEN (1-16) bytes back (one I2C_RDWR, so a
 * repeated start between them); prints the bytes, or "error N" (-errno).
 * `i2cxfer BUS ADDR scan` instead tries a one-byte read alone (no register
 * write first), which a present chip ACKs.
 *
 * Why: out-4 (2026-10-27) had vdd28 on and the gslX680 still NACKed
 * (-ENXIO) while Android, on the same bus, has it bound. Busybox-static has
 * no i2c applets; this asks both i2c1 (the touch chip, 0x40) and i2c2 (an
 * accelerometer Android has bound at 0x26), which separates "our I2C driver
 * is wrong" from "the touch chip is off or in reset".
 *
 * Freestanding like devmem.c (no ARM libc in the build image).
 */

#define O_RDWR		2
#define I2C_RDWR	0x0707
#define I2C_M_RD	0x0001

struct i2c_msg {
	unsigned short addr;
	unsigned short flags;
	unsigned short len;
	unsigned char *buf;
};

struct i2c_rdwr_ioctl_data {
	struct i2c_msg *msgs;
	unsigned int nmsgs;
};

static long sys(long nr, long a, long b, long c)
{
	register long r7 asm("r7") = nr;
	register long r0 asm("r0") = a;
	register long r1 asm("r1") = b;
	register long r2 asm("r2") = c;

	asm volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
	return r0;
}

#define sys_exit(c)		sys(1, c, 0, 0)
#define sys_write(fd, b, n)	sys(4, fd, (long)(b), n)
#define sys_open(p, fl)		sys(5, (long)(p), fl, 0)
#define sys_ioctl(fd, r, a)	sys(54, fd, r, (long)(a))

static void say(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	sys_write(1, s, n);
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

static void hex(unsigned int v, int digits)
{
	char b[9];
	int i;

	for (i = digits - 1; i >= 0; i--, v >>= 4)
		b[i] = "0123456789abcdef"[v & 0xf];
	b[digits] = 0;
	say(b);
}

static int main(int argc, char **argv)
{
	char dev[16] = "/dev/i2c-";
	unsigned char reg, buf[16];
	struct i2c_msg m[2];
	struct i2c_rdwr_ioctl_data d = { m, 2 };
	unsigned int len, i;
	long fd, r;
	int scan;

	if (argc < 4 || (argc < 5 && !(argv[3][0] == 's'))) {
		say("usage: i2cxfer BUS ADDR REG LEN | i2cxfer BUS ADDR scan\n");
		return 2;
	}
	for (i = 0; argv[1][i] && i < 4; i++)
		dev[9 + i] = argv[1][i];
	scan = argv[3][0] == 's';
	reg = num(argv[3]);
	len = scan ? 1 : num(argv[4]);
	if (len < 1 || len > 16)
		len = 1;

	fd = sys_open(dev, O_RDWR);
	if (fd < 0) {
		say("error open\n");
		return 1;
	}
	m[0].addr = num(argv[2]);
	m[0].flags = 0;
	m[0].len = 1;
	m[0].buf = &reg;
	m[1].addr = m[0].addr;
	m[1].flags = I2C_M_RD;
	m[1].len = len;
	m[1].buf = buf;
	if (scan) {
		d.msgs = &m[1];
		d.nmsgs = 1;
	}
	r = sys_ioctl(fd, I2C_RDWR, &d);
	if (r < 0) {
		say("error ");
		hex(-r, 2);
		say("\n");
		return 1;
	}
	for (i = 0; i < len; i++) {
		hex(buf[i], 2);
		say(i + 1 < len ? " " : "\n");
	}
	return 0;
}

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
