/*
 *  linux/arch/m32r/mm/init.c
 *
 *  Copyright (c) 2001, 2002  Hitoshi Yamamoto
 *
 *  Some code taken from sh version.
 *    Copyright (C) 1999  Niibe Yutaka
 *    Based on linux/arch/i386/mm/init.c:
 *      Copyright (C) 1995  Linus Torvalds
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/bootmem.h>
#include <linux/swap.h>
#include <linux/highmem.h>
#include <linux/bitops.h>
#include <linux/nodemask.h>
#include <linux/pfn.h>
#include <linux/gfp.h>
#include <asm/types.h>
#include <asm/processor.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/mmu_context.h>
#include <asm/setup.h>
#include <asm/tlb.h>
#include <asm/sections.h>

pgd_t swapper_pg_dir[1024];

/*
 * Cache of MMU context last used.
 */
#ifndef CONFIG_SMP
unsigned long mmu_context_cache_dat;
#else
unsigned long mmu_context_cache_dat[NR_CPUS];
#endif

/*
 * function prototype
 */
void __init paging_init(void);
void __init mem_init(void);
void free_initmem(void);
#ifdef CONFIG_BLK_DEV_INITRD
void free_initrd_mem(unsigned long, unsigned long);
#endif

/* It'd be good if these lines were in the standard header file. */
#define START_PFN(nid)		(NODE_DATA(nid)->bdata->node_min_pfn)
#define MAX_LOW_PFN(nid)	(NODE_DATA(nid)->bdata->node_low_pfn)

#ifndef CONFIG_DISCONTIGMEM
void __init zone_sizes_init(void)
{
	unsigned long  zones_size[MAX_NR_ZONES] = {0, };
	unsigned long  max_dma;
	unsigned long  low;
	unsigned long  start_pfn;

#ifdef CONFIG_MMU
	start_pfn = START_PFN(0);
	max_dma = virt_to_phys((char *)MAX_DMA_ADDRESS) >> PAGE_SHIFT;
	low = MAX_LOW_PFN(0);

	if (low < max_dma){
		zones_size[ZONE_DMA] = low - start_pfn;
		zones_size[ZONE_NORMAL] = 0;
	} else {
		zones_size[ZONE_DMA] = low - start_pfn;
		zones_size[ZONE_NORMAL] = low - max_dma;
	}
#else
	zones_size[ZONE_DMA] = 0 >> PAGE_SHIFT;
	zones_size[ZONE_NORMAL] = __MEMORY_SIZE >> PAGE_SHIFT;
	start_pfn = __MEMORY_START >> PAGE_SHIFT;
#endif /* CONFIG_MMU */

	free_area_init_node(0, zones_size, start_pfn, 0);
}
#else	/* CONFIG_DISCONTIGMEM */
extern void zone_sizes_init(void);
#endif	/* CONFIG_DISCONTIGMEM */

/*======================================================================*
 * paging_init() : sets up the page tables
 *======================================================================*/
void __init paging_init(void)
{
#ifdef CONFIG_MMU
	int  i;
	pgd_t *pg_dir;

	/* We don't need kernel mapping as hardware support that. */
	pg_dir = swapper_pg_dir;

	for (i = 0 ; i < USER_PTRS_PER_PGD * 2 ; i++)
		pgd_val(pg_dir[i]) = 0;
#endif /* CONFIG_MMU */
	zone_sizes_init();
}

/*======================================================================*
 * mem_init() :
 * orig : arch/sh/mm/init.c
 *======================================================================*/
void __init mem_init(void)
{
	int codesize, reservedpages, datasize, initsize;
	int nid;
#ifndef CONFIG_MMU
	extern unsigned long memory_end;
#endif

	num_physpages = 0;
	for_each_online_node(nid)
		num_physpages += MAX_LOW_PFN(nid) - START_PFN(nid) + 1;

	num_physpages -= hole_pages;

#ifndef CONFIG_DISCONTIGMEM
	max_mapnr = num_physpages;
#endif	/* CONFIG_DISCONTIGMEM */

#ifdef CONFIG_MMU
	high_memory = (void *)__va(PFN_PHYS(MAX_LOW_PFN(0)));
#else
	high_memory = (void *)(memory_end & PAGE_MASK);
#endif /* CONFIG_MMU */

	/* clear the zero-page */
	memset(empty_zero_page, 0, PAGE_SIZE);

	/* this will put all low memory onto the freelists */
	for_each_online_node(nid)
		free_all_bootmem_node(NODE_DATA(nid));

	reservedpages = reservedpages_count() - hole_pages;
	codesize = (unsigned long) &_etext - (unsigned long)&_text;
	datasize = (unsigned long) &_edata - (unsigned long)&_etext;
	initsize = (unsigned long) &__init_end - (unsigned long)&__init_begin;

	printk(KERN_INFO "Memory: %luk/%luk available (%dk kernel code, "
		"%dk reserved, %dk data, %dk init)\n",
		nr_free_pages() << (PAGE_SHIFT-10),
		num_physpages << (PAGE_SHIFT-10),
		codesize >> 10,
		reservedpages << (PAGE_SHIFT-10),
		datasize >> 10,
		initsize >> 10);
}

/*======================================================================*
 * free_initmem() :
 * orig : arch/sh/mm/init.c
 *======================================================================*/
void free_initmem(void)
{
	free_initmem_default(-1);
}

#ifdef CONFIG_BLK_DEV_INITRD
/*======================================================================*
 * free_initrd_mem() :
 * orig : arch/sh/mm/init.c
 *======================================================================*/
void free_initrd_mem(unsigned long start, unsigned long end)
{
	free_reserved_area((void *)start, (void *)end, -1, "initrd");
}
#endif
