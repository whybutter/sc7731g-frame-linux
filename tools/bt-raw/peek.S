@ SPDX-License-Identifier: MIT
@ replaces the 0xfc2d vendor handler at 0x8a825574 (48 bytes available up to 0x8a8255a4)
@ called as handler(r0=byte3, r1=u32@4 src, r2=u32@8 nwords, r3=u32@12 dst)
	.syntax unified
	.thumb
	.org 0
	push	{r4, lr}
	cmp	r2, #0
	beq	2f
1:	ldr	r4, [r1]
	str	r4, [r3]
	adds	r1, #4
	adds	r3, #4
	subs	r2, #1
	bne	1b
2:	ldr	r0, =0xfc2d
	movs	r1, #0
	bl	0x8a83ffbe
	pop	{r4, pc}
	.ltorg
