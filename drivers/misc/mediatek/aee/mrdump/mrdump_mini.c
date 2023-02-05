/*
 * Copyright (C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/memblock.h>
#include <linux/elf.h>
#include <linux/kdebug.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/printk.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/stacktrace.h>
#include <asm/stacktrace.h>
#include <asm/pgtable.h>
#include <asm-generic/percpu.h>
#include <asm-generic/sections.h>
#include <asm/page.h>
#include <asm/irq.h>
#include <asm/kexec.h>
#include <mrdump.h>
#include <mt-plat/aee.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include "../../../../kernel/sched/sched.h"
#include "mrdump_mini.h"
#include "mrdump_private.h"

#define LOG_DEBUG(fmt, ...)			\
	do {	\
		if (aee_in_nested_panic())			\
			aee_nested_printf(fmt, ##__VA_ARGS__);	\
		else						\
			pr_debug(fmt, ##__VA_ARGS__);	\
	} while (0)

#define LOG_NOTICE(fmt, ...)			\
	do {	\
		if (aee_in_nested_panic())			\
			aee_nested_printf(fmt, ##__VA_ARGS__);	\
		else						\
			pr_notice(fmt, ##__VA_ARGS__);	\
	} while (0)

#define LOGV(fmt, msg...)
#define LOGD LOG_DEBUG
#define LOGI LOG_DEBUG
#define LOGW LOG_NOTICE
#define LOGE LOG_NOTICE

static struct mrdump_mini_elf_header *mrdump_mini_ehdr;
#ifdef CONFIG_MODULES
static char modules_info_buf[MODULES_INFO_BUF_SIZE];
#endif

__weak void get_gz_log_buffer(unsigned long *addr, unsigned long *paddr,
			unsigned long *size, unsigned long *start)
{
	*addr = *paddr = *size = *start = 0;
}

__weak void get_disp_err_buffer(unsigned long *addr, unsigned long *size,
		unsigned long *start)
{
}


__weak void get_disp_fence_buffer(unsigned long *addr, unsigned long *size,
		unsigned long *start)
{
}

__weak void get_disp_dbg_buffer(unsigned long *addr, unsigned long *size,
		unsigned long *start)
{
}

__weak void get_disp_dump_buffer(unsigned long *addr, unsigned long *size,
		unsigned long *start)
{
}

__weak void get_kernel_log_buffer(unsigned long *addr, unsigned long *size,
		unsigned long *start)
{
}

__weak void aee_rr_get_desc_info(unsigned long *addr, unsigned long *size,
		unsigned long *start)
{
}

__weak void get_pidmap_aee_buffer(unsigned long *addr, unsigned long *size)
{
}

#ifdef __aarch64__
#define MIN_MARGIN KIMAGE_VADDR
#else
#define MIN_MARGIN PAGE_OFFSET
#endif

#ifdef __aarch64__
static unsigned long virt_2_pfn(unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k(addr), _pgd_val = {0};
	pud_t *pud, _pud_val = {{0} };
	pmd_t *pmd, _pmd_val = {0};
	pte_t *ptep, _pte_val = {0};
	unsigned long pfn = ~0UL;

#ifdef CONFIG_ARM64
	if (addr < VA_START)
		goto OUT;
#endif
	if (probe_kernel_address(pgd, _pgd_val) || pgd_none(_pgd_val))
		goto OUT;
	pud = pud_offset(pgd, addr);
	if (probe_kernel_address(pud, _pud_val) || pud_none(_pud_val))
		goto OUT;
	if (pud_sect(_pud_val)) {
		pfn = pud_pfn(_pud_val) + ((addr&~PUD_MASK) >> PAGE_SHIFT);
	} else if (pud_table(_pud_val)) {
		pmd = pmd_offset(pud, addr);
		if (probe_kernel_address(pmd, _pmd_val) || pmd_none(_pmd_val))
			goto OUT;
		if (pmd_sect(_pmd_val)) {
			pfn = pmd_pfn(_pmd_val) +
				((addr&~PMD_MASK) >> PAGE_SHIFT);
		} else if (pmd_table(_pmd_val)) {
			ptep = pte_offset_map(pmd, addr);
			if (probe_kernel_address(ptep, _pte_val)
				|| !pte_present(_pte_val)) {
				pte_unmap(ptep);
				goto OUT;
			}
			pfn = pte_pfn(_pte_val);
			pte_unmap(ptep);
		}
	}
OUT:
	return pfn;

}
#else
#ifndef pmd_sect
#define pmd_sect(pmd)	(pmd & PMD_TYPE_SECT)
#endif
#ifndef pmd_table
#define pmd_table(pmd)	(pmd & PMD_TYPE_TABLE)
#endif
#ifndef pmd_pfn
#define pmd_pfn(pmd)	(((pmd_val(pmd) & PMD_MASK) & PHYS_MASK) >> PAGE_SHIFT)
#endif
static unsigned long virt_2_pfn(unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k(addr), _pgd_val = {0};
#ifdef CONFIG_ARM_LPAE
	pud_t *pud, _pud_val = {0};
#else
	pud_t *pud, _pud_val = {{0} };
#endif
	pmd_t *pmd, _pmd_val = 0;
	pte_t *ptep, _pte_val = 0;
	unsigned long pfn = ~0UL;

	if (probe_kernel_address(pgd, _pgd_val) || pgd_none(_pgd_val))
		goto OUT;
	pud = pud_offset(pgd, addr);
	if (probe_kernel_address(pud, _pud_val) || pud_none(_pud_val))
		goto OUT;
	pmd = pmd_offset(pud, addr);
	if (probe_kernel_address(pmd, _pmd_val) || pmd_none(_pmd_val))
		goto OUT;
	if (pmd_sect(_pmd_val)) {
		pfn = pmd_pfn(_pmd_val) + ((addr&~PMD_MASK) >> PAGE_SHIFT);
	} else if (pmd_table(_pmd_val)) {
		ptep = pte_offset_map(pmd, addr);
		if (probe_kernel_address(ptep, _pte_val)
			|| !pte_present(_pte_val)) {
			pte_unmap(ptep);
			goto OUT;
		}
		pfn = pte_pfn(_pte_val);
		pte_unmap(ptep);
	}
OUT:
	return pfn;
}
#endif

/* copy from fs/binfmt_elf.c */
static void fill_elf_header(struct elfhdr *elf, int segs)
{
	memcpy(elf->e_ident, ELFMAG, SELFMAG);
	elf->e_ident[EI_CLASS] = ELF_CLASS;
	elf->e_ident[EI_DATA] = ELF_DATA;
	elf->e_ident[EI_VERSION] = EV_CURRENT;
	elf->e_ident[EI_OSABI] = ELF_OSABI;

	elf->e_type = ET_CORE;
	elf->e_machine = ELF_ARCH;
	elf->e_version = EV_CURRENT;
	elf->e_phoff = sizeof(struct elfhdr);
#ifndef ELF_CORE_EFLAGS
#define ELF_CORE_EFLAGS	0
#endif
	elf->e_flags = ELF_CORE_EFLAGS;
	elf->e_ehsize = sizeof(struct elfhdr);
	elf->e_phentsize = sizeof(struct elf_phdr);
	elf->e_phnum = segs;

}

static void fill_elf_note_phdr(struct elf_phdr *phdr, int sz, loff_t offset)
{
	phdr->p_type = PT_NOTE;
	phdr->p_offset = offset;
	phdr->p_vaddr = 0;
	phdr->p_paddr = 0;
	phdr->p_filesz = sz;
	phdr->p_memsz = 0;
	phdr->p_flags = 0;
	phdr->p_align = 0;
}

static noinline void fill_note(struct elf_note *note, const char *name,
		int type, unsigned int sz, unsigned int namesz)
{
	char *n_name = (char *)note + sizeof(struct elf_note);

	note->n_namesz = namesz;
	note->n_type = type;
	note->n_descsz = sz;
	strncpy(n_name, name, note->n_namesz);
}

static void fill_note_L(struct elf_note *note, const char *name, int type,
		unsigned int sz)
{
	fill_note(note, name, type, sz, NOTE_NAME_LONG);
}

static void fill_note_S(struct elf_note *note, const char *name, int type,
		unsigned int sz)
{
	fill_note(note, name, type, sz, NOTE_NAME_SHORT);
}

/*
 * fill up all the fields in prstatus from the given task struct, except
 * registers which need to be filled up separately.
 */
static void fill_prstatus(struct elf_prstatus *prstatus, struct pt_regs *regs,
			  struct task_struct *p, unsigned long pid)
{
	elf_core_copy_regs(&prstatus->pr_reg, regs);
	prstatus->pr_pid = pid;
	prstatus->pr_ppid = AEE_MTK_CPU_NUMS;
	prstatus->pr_sigpend = (uintptr_t)p;
}

static int fill_psinfo(struct elf_prpsinfo *psinfo)
{
	unsigned int i;

	strncpy(psinfo->pr_psargs, saved_command_line, ELF_PRARGSZ - 1);
	for (i = 0; i < ELF_PRARGSZ - 1; i++)
		if (psinfo->pr_psargs[i] == 0)
			psinfo->pr_psargs[i] = ' ';
	psinfo->pr_psargs[ELF_PRARGSZ - 1] = 0;
	strncpy(psinfo->pr_fname, "vmlinux", sizeof(psinfo->pr_fname));
	return 0;
}

#ifndef __pa_nodebug
#ifdef __pa_symbol_nodebug
#define __pa_nodebug __pa_symbol_nodebug
#else
#define __pa_nodebug __pa
#endif
#endif
void mrdump_mini_add_misc_pa(unsigned long va, unsigned long pa,
		unsigned long size, unsigned long start, char *name)
{
	int i;
	struct elf_note *note;

	if (!mrdump_mini_ehdr)
		return;

	for (i = 0; i < MRDUMP_MINI_NR_MISC; i++) {
		note = &mrdump_mini_ehdr->misc[i].note;
		if (note->n_type == NT_IPANIC_MISC) {
			if (strncmp(name, MRDUMP_MINI_MISC_LOAD, 4) == 0)
				continue;
			if (strncmp(mrdump_mini_ehdr->misc[i].name, name, 16)
					!= 0)
				continue;
		}
		mrdump_mini_ehdr->misc[i].data.vaddr = va;
		mrdump_mini_ehdr->misc[i].data.paddr = pa;
		mrdump_mini_ehdr->misc[i].data.size = size;
		mrdump_mini_ehdr->misc[i].data.start =
		    mrdump_virt_addr_valid((void *)start) ?
			__pa_nodebug(start) : 0;
		fill_note_L(note, name, NT_IPANIC_MISC,
				sizeof(struct mrdump_mini_elf_misc));
		break;
	}
}
EXPORT_SYMBOL(mrdump_mini_add_misc_pa);

void mrdump_mini_add_misc(unsigned long addr, unsigned long size,
		unsigned long start, char *name)
{
	if (!mrdump_virt_addr_valid((void *)addr))
		return;
	mrdump_mini_add_misc_pa(addr, __pa_nodebug(addr), size, start, name);
}

int kernel_addr_valid(unsigned long addr)
{
	if (addr < MIN_MARGIN)
		return 0;

	return pfn_valid(virt_2_pfn(addr));
}

static void mrdump_mini_build_task_info(struct pt_regs *regs)
{
#define MAX_STACK_TRACE_DEPTH 64
	unsigned long ipanic_stack_entries[MAX_STACK_TRACE_DEPTH];
	char symbol[96] = {'\0'};
	int sz;
	int off, plen;
	struct stack_trace trace;
	int i;
	struct task_struct *tsk, *cur;
	struct task_struct *previous;
	struct aee_process_info *cur_proc;

	if (!mrdump_virt_addr_valid(current_thread_info())) {
		LOGE("current thread info invalid\n");
		return;
	}
	cur = current;
	tsk = cur;
	if (!mrdump_virt_addr_valid(tsk)) {
		LOGE("tsk invalid\n");
		return;
	}
	cur_proc = (struct aee_process_info *)((void *)mrdump_mini_ehdr +
			MRDUMP_MINI_HEADER_SIZE);
	/* Current panic user tasks */
	sz = 0;
	do {
		if (!tsk) {
			LOGE("No tsk info\n");
			memset_io(cur_proc, 0x0,
				sizeof(struct aee_process_info));
			break;
		}
		/* FIXME: Check overflow ? */
		sz += snprintf(symbol + sz, 96 - sz, "[%s, %d]", tsk->comm,
				tsk->pid);
		previous = tsk;
		tsk = tsk->real_parent;
		if (!mrdump_virt_addr_valid(tsk)) {
			LOGE("tsk(%p) invalid (previous: [%s, %d])\n", tsk,
					previous->comm, previous->pid);
			break;
		}
	} while (tsk && (tsk->pid != 0) && (tsk->pid != 1));
	if (strncmp(cur_proc->process_path, symbol, sz) == 0) {
		LOGE("same process path\n");
		return;
	}

	memset_io(cur_proc, 0, sizeof(struct aee_process_info));
	memcpy(cur_proc->process_path, symbol, sz);

	/* Grab kernel task stack trace */
	trace.nr_entries = 0;
	trace.max_entries = MAX_STACK_TRACE_DEPTH;
	trace.entries = ipanic_stack_entries;
	/* the value is only from experience and without strict rules
	 * need to pay attention to the value
	 */
	trace.skip = 4;
	save_stack_trace_tsk(cur, &trace);
	if (regs) {
		cur_proc->ke_frame.pc = (__u64) regs->reg_pc;
		cur_proc->ke_frame.lr = (__u64) regs->reg_lr;
	} else {
		/* in case panic() is called without die */
		/* Todo: a UT for this */
		cur_proc->ke_frame.pc = ipanic_stack_entries[0];
		cur_proc->ke_frame.lr = ipanic_stack_entries[1];
	}
	/* Skip the entries -
	 * ipanic_save_current_tsk_info/save_stack_trace_tsk
	 */
	for (i = 0; i < trace.nr_entries; i++) {
		off = strlen(cur_proc->backtrace);
		plen = AEE_BACKTRACE_LENGTH - ALIGN(off, 8);
		if (plen > 16) {
			if (ipanic_stack_entries[i] != cur_proc->ke_frame.pc)
				ipanic_stack_entries[i] -= 4;
			sz = snprintf(symbol, 96, "[<%px>] %pS\n",
				      (void *)ipanic_stack_entries[i],
				      (void *)ipanic_stack_entries[i]);
			if (ALIGN(sz, 8) - sz) {
				memset_io(symbol + sz - 1, ' ',
						ALIGN(sz, 8) - sz);
				memset_io(symbol + ALIGN(sz, 8) - 1, '\n', 1);
			}
			if (ALIGN(sz, 8) <= plen)
				memcpy(cur_proc->backtrace + ALIGN(off, 8),
						symbol, ALIGN(sz, 8));
		}
	}
	snprintf(cur_proc->ke_frame.pc_symbol, AEE_SZ_SYMBOL_S, "[<%px>] %pS",
		 (void *)(unsigned long)cur_proc->ke_frame.pc,
		 (void *)(unsigned long)cur_proc->ke_frame.pc);
	snprintf(cur_proc->ke_frame.lr_symbol, AEE_SZ_SYMBOL_L, "[<%px>] %pS",
		 (void *)(unsigned long)cur_proc->ke_frame.lr,
		 (void *)(unsigned long)cur_proc->ke_frame.lr);
}

__weak int save_modules(char *mbuf, int mbufsize)
{
	LOGE("%s weak function\n", __func__);
	return 0;
}

int mrdump_modules_info(unsigned char *buffer, size_t sz_buf)
{
#ifdef CONFIG_MODULES
	int sz;

	sz = save_modules(modules_info_buf, MODULES_INFO_BUF_SIZE);
	if (sz_buf < sz || buffer == NULL)
		return -1;
	memcpy(buffer, modules_info_buf, sz);
	return sz;
#else
	return -1;
#endif
}

static void mrdump_mini_clear_loads(void);
void mrdump_mini_add_hang_raw(unsigned long vaddr, unsigned long size)
{
	LOGE("mrdump: hang data 0x%lx size:0x%lx\n", vaddr, size);
	mrdump_mini_add_misc(vaddr, size, 0, "_HANG_DETECT_");
	/* hang only remove mini rdump loads info to save storage space */
	mrdump_mini_clear_loads();
}

#define EXTRA_MISC(func, name, max_size) \
	__weak void func(unsigned long *vaddr, unsigned long *size) \
	{ \
		if (size != NULL) \
			*size = 0; \
	}
#include "mrdump_mini_extra_misc.h"

#undef EXTRA_MISC
#define EXTRA_MISC(func, name, max_size) \
	{func, name, max_size},

static struct mrdump_mini_extra_misc extra_members[] = {
	#include "mrdump_mini_extra_misc.h"
};
#define EXTRA_TOTAL_NUM ((sizeof(extra_members)) / (sizeof(extra_members[0])))
static size_t __maybe_unused dummy_check(void)
{
	size_t dummy;

	dummy = BUILD_BUG_ON_ZERO(EXTRA_TOTAL_NUM > 10);
	return dummy;
}

static int _mrdump_mini_add_extra_misc(unsigned long vaddr, unsigned long size,
	const char *name)
{
	char name_buf[SZ_128];

	if (mrdump_mini_ehdr == NULL ||
		size == 0 ||
		size > SZ_512K ||
		name == NULL)
		return -1;
	snprintf(name_buf, SZ_128, "_EXTRA_%s_", name);
	mrdump_mini_add_misc(vaddr, size, 0, name_buf);
	return 0;
}

void mrdump_mini_add_extra_misc(void)
{
	static int once;
	int i;
	unsigned long vaddr = 0;
	unsigned long size = 0;
	int ret;

	if (once == 0) {
		once = 1;
		for (i = 0; i < EXTRA_TOTAL_NUM; i++) {
			extra_members[i].dump_func(&vaddr, &size);
			if (size > extra_members[i].max_size)
				continue;
			ret = _mrdump_mini_add_extra_misc(vaddr, size,
					extra_members[i].dump_name);
			if (ret < 0)
				LOGE("mrdump: add %s:0x%lx sz:0x%lx failed\n",
					extra_members[i].dump_name,
					vaddr, size);
		}
	}
}

void mrdump_mini_ke_cpu_regs(struct pt_regs *regs)
{
	struct pt_regs context;

	if (!regs) {
		regs = &context;
		crash_setup_regs(regs, NULL);
	}
	mrdump_mini_build_task_info(regs);
	mrdump_mini_add_extra_misc();
}
EXPORT_SYMBOL(mrdump_mini_ke_cpu_regs);

static void mrdump_mini_fatal(const char *str)
{
	LOGE("minirdump: FATAL:%s\n", str);
	BUG();
}

static unsigned int mrdump_mini_addr;
static unsigned int mrdump_mini_size;
void mrdump_mini_set_addr_size(unsigned int addr, unsigned int size)
{
	mrdump_mini_addr = addr;
	mrdump_mini_size = size;
}

static void mrdump_mini_build_elf_misc(void)
{
	struct mrdump_mini_elf_misc misc;
	unsigned long task_info_va =
	    (unsigned long)((void *)mrdump_mini_ehdr + MRDUMP_MINI_HEADER_SIZE);
	unsigned long task_info_pa = 0;
	unsigned long gz_log_pa = 0;

	memset_io(&misc, 0, sizeof(struct mrdump_mini_elf_misc));
	get_gz_log_buffer(&misc.vaddr, &gz_log_pa, &misc.size, &misc.start);
	if (gz_log_pa != 0)
		mrdump_mini_add_misc_pa(misc.vaddr, gz_log_pa, misc.size,
					misc.start, "_GZ_LOG_");
	if (mrdump_mini_addr != 0
		&& mrdump_mini_size != 0
		&& MRDUMP_MINI_HEADER_SIZE < mrdump_mini_size) {
		task_info_pa = (unsigned long)(mrdump_mini_addr +
				MRDUMP_MINI_HEADER_SIZE);
	} else {
		LOGE("minirdump: unexpected addr:0x%x, size:0x%x(0x%x)\n",
			mrdump_mini_addr, mrdump_mini_size,
			(unsigned int)MRDUMP_MINI_HEADER_SIZE);
		mrdump_mini_fatal("illegal addr size");
	}
	mrdump_mini_add_misc_pa(task_info_va, task_info_pa,
			sizeof(struct aee_process_info), 0, "PROC_CUR_TSK");
	memset_io(&misc, 0, sizeof(struct mrdump_mini_elf_misc));
	get_kernel_log_buffer(&misc.vaddr, &misc.size, &misc.start);
	mrdump_mini_add_misc(misc.vaddr, misc.size, misc.start, "_KERNEL_LOG_");
	memset_io(&misc, 0, sizeof(struct mrdump_mini_elf_misc));
	aee_rr_get_desc_info(&misc.vaddr, &misc.size, &misc.start);
	mrdump_mini_add_misc(misc.vaddr, misc.size, misc.start, "_RR_DESC_");
	memset_io(&misc, 0, sizeof(struct mrdump_mini_elf_misc));
	get_disp_err_buffer(&misc.vaddr, &misc.size, &misc.start);
	mrdump_mini_add_misc(misc.vaddr, misc.size, misc.start, "_DISP_ERR_");
	memset_io(&misc, 0, sizeof(struct mrdump_mini_elf_misc));
	get_disp_dump_buffer(&misc.vaddr, &misc.size, &misc.start);
	mrdump_mini_add_misc(misc.vaddr, misc.size, misc.start, "_DISP_DUMP_");
	memset_io(&misc, 0, sizeof(struct mrdump_mini_elf_misc));
	get_disp_fence_buffer(&misc.vaddr, &misc.size, &misc.start);
	mrdump_mini_add_misc(misc.vaddr, misc.size, misc.start, "_DISP_FENCE_");
	memset_io(&misc, 0, sizeof(struct mrdump_mini_elf_misc));
	get_disp_dbg_buffer(&misc.vaddr, &misc.size, &misc.start);
	mrdump_mini_add_misc(misc.vaddr, misc.size, misc.start, "_DISP_DBG_");
#ifdef CONFIG_MODULES
	mrdump_mini_add_misc_pa((unsigned long)modules_info_buf,
		(unsigned long)__pa_nodebug((unsigned long)modules_info_buf),
		MODULES_INFO_BUF_SIZE, 0, "SYS_MODULES_INFO");
#endif

	memset_io(&misc, 0, sizeof(struct mrdump_mini_elf_misc));
	get_pidmap_aee_buffer(&misc.vaddr, &misc.size);
	misc.start = 0;
	mrdump_mini_add_misc(misc.vaddr, misc.size, misc.start, "_PIDMAP_");

	memset_io(&misc, 0, sizeof(struct mrdump_mini_elf_misc));
	misc.vaddr = (unsigned long)(void *)linux_banner;
	misc.size = strlen(linux_banner);
	misc.start = 0;
	mrdump_mini_add_misc(misc.vaddr, misc.size, misc.start, "_VERSION_BR");
}

static void mrdump_mini_clear_loads(void)
{
	struct elf_phdr *phdr;
	int i;

	for (i = 0; i < MRDUMP_MINI_NR_SECTION; i++) {
		phdr = &mrdump_mini_ehdr->phdrs[i];
		if (phdr->p_type == PT_NULL)
			continue;
		if (phdr->p_type == PT_LOAD)
			phdr->p_type = PT_NULL;
	}
}

static void *remap_lowmem(phys_addr_t start, phys_addr_t size)
{
	struct page **pages;
	phys_addr_t page_start;
	unsigned int page_count;
	unsigned int i;
	void *vaddr;

	page_start = start - offset_in_page(start);
	page_count = DIV_ROUND_UP(size + offset_in_page(start), PAGE_SIZE);

	pages = kmalloc_array(page_count, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		LOGE("%s: Failed to allocate array for %u pages\n",
				__func__, page_count);
		return NULL;
	}

	for (i = 0; i < page_count; i++) {
		phys_addr_t addr = page_start + i * PAGE_SIZE;

		pages[i] = pfn_to_page(addr >> PAGE_SHIFT);
	}
	vaddr = vmap(pages, page_count, VM_MAP, PAGE_KERNEL);
	kfree(pages);
	if (!vaddr) {
		LOGE("%s: Failed to map %u pages\n", __func__, page_count);
		return NULL;
	}

	return vaddr + offset_in_page(start);
}

static void __init mrdump_mini_elf_header_init(void)
{
	if (mrdump_mini_addr != 0
		&& mrdump_mini_size != 0) {
		mrdump_mini_ehdr =
		    remap_lowmem(mrdump_mini_addr,
				 mrdump_mini_size);
		LOGE("minirdump: [DT] reserved 0x%x+0x%lx->%p\n",
			mrdump_mini_addr,
			(unsigned long)mrdump_mini_size,
			mrdump_mini_ehdr);
	} else {
		LOGE("minirdump: [DT] illegal value 0x%x(0x%x)\n",
				mrdump_mini_addr,
				mrdump_mini_size);
		mrdump_mini_fatal("illegal addr size");
	}
	if (mrdump_mini_ehdr == NULL) {
		LOGE("mrdump mini reserve buffer fail");
		mrdump_mini_fatal("header null pointer");
		return;
	}
	memset_io(mrdump_mini_ehdr, 0, MRDUMP_MINI_HEADER_SIZE +
			sizeof(struct aee_process_info));
	fill_elf_header(&mrdump_mini_ehdr->ehdr, MRDUMP_MINI_NR_SECTION);
}

int mrdump_mini_init(void)
{
	int i, cpu;
	unsigned long size, offset, vaddr;
	struct pt_regs regs;

	mrdump_mini_elf_header_init();

	fill_psinfo(&mrdump_mini_ehdr->psinfo.data);
	fill_note_S(&mrdump_mini_ehdr->psinfo.note, "vmlinux", NT_PRPSINFO,
		    sizeof(struct elf_prpsinfo));

	memset_io(&regs, 0, sizeof(struct pt_regs));
	for (i = 0; i < AEE_MTK_CPU_NUMS; i++) {
		fill_prstatus(&mrdump_mini_ehdr->prstatus[i].data, &regs,
				NULL, i);
		fill_note_S(&mrdump_mini_ehdr->prstatus[i].note, "NA",
				NT_PRSTATUS, sizeof(struct elf_prstatus));
	}

	offset = offsetof(struct mrdump_mini_elf_header, psinfo);
	size = sizeof(mrdump_mini_ehdr->psinfo) +
		sizeof(mrdump_mini_ehdr->prstatus);
	fill_elf_note_phdr(&mrdump_mini_ehdr->phdrs[0], size, offset);

	for (i = 0; i < MRDUMP_MINI_NR_MISC; i++)
		fill_note_L(&mrdump_mini_ehdr->misc[i].note, "NA", 0,
			    sizeof(struct mrdump_mini_elf_misc));
	mrdump_mini_build_elf_misc();
	fill_elf_note_phdr(&mrdump_mini_ehdr->phdrs[1],
			sizeof(mrdump_mini_ehdr->misc),
			offsetof(struct mrdump_mini_elf_header, misc));

	if (mrdump_cblock) {
		mrdump_mini_add_misc_pa((unsigned long)mrdump_cblock,
				mrdump_sram_cb.start_addr,
				mrdump_sram_cb.size,
				0, MRDUMP_MINI_MISC_LOAD);

		vaddr = (unsigned long)&kallsyms_addresses;
		vaddr = round_down(vaddr, PAGE_SIZE);
		size = mrdump_cblock->machdesc.kallsyms.size;
		size = round_up(size, PAGE_SIZE);
		if (vaddr)
			mrdump_mini_add_misc_pa(vaddr, __pa_nodebug(vaddr),
					size, 0, MRDUMP_MINI_MISC_LOAD);
	}

	vaddr = round_down((unsigned long)__per_cpu_offset, PAGE_SIZE);
	mrdump_mini_add_misc_pa(vaddr, __pa_nodebug(vaddr),
			PAGE_SIZE * 2, 0, MRDUMP_MINI_MISC_LOAD);

	for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
		vaddr = (unsigned long)cpu_rq(cpu);
		vaddr = round_down(vaddr, PAGE_SIZE);
		mrdump_mini_add_misc(vaddr, MRDUMP_MINI_SECTION_SIZE,
				0, MRDUMP_MINI_MISC_LOAD);
	}

	return 0;
}

int mini_rdump_reserve_memory(struct reserved_mem *rmem)
{
	pr_info("[memblock]%s: 0x%llx - 0x%llx (0x%llx)\n",
		"mediatek,minirdump",
		 (unsigned long long)rmem->base,
		 (unsigned long long)rmem->base +
		 (unsigned long long)rmem->size,
		 (unsigned long long)rmem->size);
	return 0;
}

RESERVEDMEM_OF_DECLARE(reserve_memory_minirdump, "mediatek,minirdump",
		       mini_rdump_reserve_memory);

