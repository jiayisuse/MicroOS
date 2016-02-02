#ifndef MM_H
#define MM_H

#include <yalnix.h>
#include <hardware.h>
#include <list.h>

#define PAGE_KINDEX(addr) ((addr) ? (unsigned long)(addr) >> PAGESHIFT : 0)
#define PAGE_UINDEX(addr) ((addr) ? \
		((unsigned long)(addr) - VMEM_0_LIMIT) >> PAGESHIFT : 0)
#define PAGE_NR(addr) ((addr) ? (unsigned long)(addr) >> PAGESHIFT : 0)

#define PAGE_KADDR(index) ((index) << PAGESHIFT)
#define PAGE_UADDR(index) (((index) << PAGESHIFT) + VMEM_1_BASE)

struct phy_frame {
	struct list_head list;
	unsigned int index;
};

struct phy_free_frames {
	struct list_head head;
	unsigned int n;
};

struct my_pte {
	u_long valid	: 1;	/* page mapping is valid */
	u_long prot	: 3;	/* page protection bits */
	u_long cow	: 1;	/* copy_on_write bit */
	u_long swap	: 1;	/* swap flage */
	u_long reserved	: 2;	/* reserved; currently unused */
	u_long pfn	: 24;	/* page frame number */
};

extern unsigned long _text_start, _text_end, _data_end, _kbrk, _kstack_base;
extern unsigned int total_pages;
extern struct my_pte *page_table_0;

struct phy_frame *get_free_frame();
void remove_free_frame(struct phy_frame *frame);
int add_free_frame(unsigned int index);

int map_pages(struct my_pte *, unsigned int, unsigned int, int);
int map_pages_and_copy(struct my_pte *, struct my_pte *, unsigned long,
		unsigned int, unsigned int);
int get_free_pages(unsigned int *record, unsigned int n_page);
int get_free_pages_and_copy(unsigned int *, struct my_pte *,
		unsigned long, unsigned int, unsigned int);
#ifdef COW
int page_cow_copy(struct my_pte *d_table, struct my_pte *s_table,
		unsigned long source_brk, unsigned int page_index);
#endif
int update_pages_prot(struct my_pte *, unsigned int, unsigned int, int);
int update_pages_indexes(struct my_pte *, unsigned int, unsigned int,
		unsigned int *);
#ifdef COW
int update_pages_cow(struct my_pte *table, unsigned int start_index,
		unsigned int n_page, int cow);
#endif
int unmap_pages(struct my_pte *, unsigned int, unsigned int);
int collect_back_pages(unsigned int *, unsigned int);

#endif
