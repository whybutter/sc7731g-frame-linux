// SPDX-License-Identifier: GPL-2.0-only
/*
 * reboot-to [ARG] -- sync and restart through reboot(2)'s RESTART2, passing
 * ARG to the kernel's restart handlers (no ARG: a plain restart).
 *
 * Why: busybox's reboot takes no argument, and the way back to the stock
 * Android kept in the recovery partition (docs/install-plan.md, Phase 2) is a
 * restart with "recovery": the sprd-adi restart handler records it as the
 * PMIC reset reason (HWRST_STATUS_RECOVERY), and the vendor U-Boot's cboot
 * then boots the recovery partition -- once; the next restart is a normal
 * boot again. Writing "boot-recovery" into misc would also work, but stock
 * Android never clears it, so the frame would stay in Android.
 *
 * Freestanding like devmem.c (no ARM libc in the build image).
 */

#define LINUX_REBOOT_MAGIC1	0xfee1dead
#define LINUX_REBOOT_MAGIC2	672274793
#define LINUX_REBOOT_CMD_RESTART	0x01234567
#define LINUX_REBOOT_CMD_RESTART2	0xa1b2c3d4

static long sys(long nr, long a, long b, long c, long d)
{
	register long r7 asm("r7") = nr;
	register long r0 asm("r0") = a;
	register long r1 asm("r1") = b;
	register long r2 asm("r2") = c;
	register long r3 asm("r3") = d;

	asm volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2), "r"(r3) : "memory");
	return r0;
}

#define sys_exit(c)	sys(1, c, 0, 0, 0)
#define sys_write(b, n)	sys(4, 2, (long)(b), n, 0)
#define sys_sync()	sys(36, 0, 0, 0, 0)
#define sys_reboot(cmd, arg) \
	sys(88, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, cmd, (long)(arg))

static int main(int argc, char **argv)
{
	sys_sync();
	if (argc > 1)
		sys_reboot(LINUX_REBOOT_CMD_RESTART2, argv[1]);
	else
		sys_reboot(LINUX_REBOOT_CMD_RESTART, 0);
	sys_write("reboot-to: reboot(2) failed\n", 28);
	return 1;
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
