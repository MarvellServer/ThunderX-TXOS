/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/const.h>
#include <asm/page.h>

	.globl vdso_ilp32_start, vdso_ilp32_end
	.section .rodata
	.balign PAGE_SIZE
vdso_ilp32_start:
	.incbin "arch/arm64/kernel/vdso_ilp32/vdso.so"
	.balign PAGE_SIZE
vdso_ilp32_end:

	.previous
