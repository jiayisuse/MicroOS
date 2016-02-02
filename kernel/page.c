#include <page.h>
#include <sys.h>
#include "internal.h"

unsigned long _text_start = 0, _text_end = 0, _data_end = 0,
	      _kbrk = 0, _kstack_base = 0;
unsigned int total_pages = 0;
struct my_pte *page_table_0 = NULL;

static struct phy_free_frames phy_free_frames = {
	.head = { &phy_free_frames.head, &phy_free_frames.head },
	.n = 0,
};


/*
 * try to get a free physical frame.
 * if there is no available frames call swap_out() to get
 * more free frames and return one of them
 */
inline struct phy_frame *get_free_frame()
{
	struct phy_frame *frame;
	int ret;

	if (list_empty(&phy_free_frames.head)) {
		ret = swap_out();
		if (ret || list_empty(&phy_free_frames.head))
			return NULL;
	}

	frame = (struct phy_frame *)list_first(&phy_free_frames.head);

	return frame;
}

/*
 * remove the frame from free list and free it
 */
inline void remove_free_frame(struct phy_frame *frame)
{
	list_del(&frame->list);
	phy_free_frames.n--;
	free(frame);

	return;
}

/**
 * allocate and add a new physical frame to the free list
 * @index: the new available physical frame number
 */
inline int add_free_frame(unsigned int index)
{
	struct phy_frame *frame;
	int ret = 0;

	frame = (void *)calloc(1, sizeof(struct phy_frame));
	if (frame == NULL) {
		_error("Allocating free physical page failed!!!\n");
		ret = ENOMEM;
		goto out;
	}
	frame->index = index;
	list_add(&phy_free_frames.head, &frame->list);
	phy_free_frames.n++;

out:
	return ret;
}

inline static void flush_TLB(struct my_pte *table)
{
	if (table == page_table_0)
		WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_0);
	else
		WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

	return;
}

/**
 * map some virtual pages to physical frames
 * @table: the page table to be manipulated
 * @start_index: the start page index in the page table to be mapped
 * @n_page: the number of pages to be mapped
 * @prot: page property in the page table entry
 */
int map_pages(struct my_pte *table, unsigned int start_index,
		unsigned int n_page, int prot)
{
	int ret = 0;
	unsigned int i, end_index = start_index + n_page;
	struct my_pte pte;

	bzero(&pte, sizeof(struct my_pte));
	pte.valid = 1;
	pte.prot = prot;
	for (i = start_index; i < end_index; i++) {
		struct phy_frame *frame;

		frame = get_free_frame();
		if (frame == NULL) {
			_error("No more physical frames available now!\n");
			ret = ENOMEM;
			goto out;
		}
		pte.pfn = frame->index;
		table[i] = pte;
		remove_free_frame(frame);
	}

out:
	flush_TLB(table);
	return ret;
}

#ifdef COW
/**
 * copy page on copy-on-write
 * @d_table: the page table to be copied to
 * @s_table: the page table to be copiied from
 * @source_brk: the brk address of the source process
 * @page_index: which page to be made a copy
 */
int page_cow_copy(struct my_pte *d_table, struct my_pte *s_table,
		  unsigned long source_brk, unsigned int page_index)
{
	int ret = 0;
	unsigned int dest_index;

	if (d_table[page_index].pfn == s_table[page_index].pfn) {
		struct phy_frame *frame;
		frame = get_free_frame();
		if (frame == NULL) {
			_error("No more physical frames available now!\n");
			ret = ENOMEM;
			goto out;
		}

		source_brk = UP_TO_PAGE(source_brk);
		if (s_table == page_table_0)
			dest_index = PAGE_KINDEX(source_brk);
		else
			dest_index = PAGE_UINDEX(source_brk);

		d_table[page_index].pfn = frame->index;
		d_table[page_index].prot = PROT_READ | PROT_WRITE;
		d_table[page_index].cow = 0;
		s_table[dest_index].pfn = frame->index;
		s_table[dest_index].valid = 1;
		s_table[dest_index].prot = PROT_READ | PROT_WRITE;
		WriteRegister(REG_TLB_FLUSH, source_brk);

		if (s_table == page_table_0)
			memcpy(source_brk, PAGE_KADDR(page_index), PAGESIZE);
		else
			memcpy(source_brk, PAGE_UADDR(page_index), PAGESIZE);

		bzero(s_table + dest_index, sizeof(struct my_pte));
		WriteRegister(REG_TLB_FLUSH, source_brk);

		remove_free_frame(frame);
	}
out:
	return ret;
}
#endif

/**
 * map virtual pages to physical frames and copy the content
 * from another process
 *
 * @d_table: the page table to be copied to
 * @s_table: the page table to be copiied from
 * @source_brk: the brk address of the source process
 * @start_index: the start page index in the page table to be mapped
 * @n_page: the number of pages to be mapped
 */
int map_pages_and_copy(struct my_pte *d_table,
		       struct my_pte *s_table,
		       unsigned long source_brk,
		       unsigned int start_index,
		       unsigned int n_page)
{
	int ret = 0;
	unsigned int i, end_index = start_index + n_page;
	unsigned int dest_index;
	struct my_pte pte;

	source_brk = UP_TO_PAGE(source_brk);
	if (s_table == page_table_0)
		dest_index = PAGE_KINDEX(source_brk);
	else
		dest_index = PAGE_UINDEX(source_brk);

	bzero(&pte, sizeof(struct my_pte));
	pte.valid = 1;
	pte.prot = PROT_READ | PROT_WRITE;
	for (i = start_index; i < end_index; i++) {
		struct phy_frame *frame;

		frame = get_free_frame();
		if (frame == NULL) {
			_error("No more physical frames available now!\n");
			ret = ENOMEM;
			goto out;
		}
		_debug("frame index = %u, virtual index = %u\n",
				frame->index, i);
		pte.pfn = frame->index;
		d_table[i] = pte;
		s_table[dest_index] = pte;
		remove_free_frame(frame);

		WriteRegister(REG_TLB_FLUSH, source_brk);
		if (s_table == page_table_0)
			memcpy(source_brk, PAGE_KADDR(i), PAGESIZE);
		else
			memcpy(source_brk, PAGE_UADDR(i), PAGESIZE);
	}

	bzero(s_table + dest_index, sizeof(struct my_pte));
	WriteRegister(REG_TLB_FLUSH, source_brk);
out:
	return ret;
}

/**
 * get some free pages from physical memory
 * @record: the array to record the indexes of physical frames
 * @n_page: the number of pages to get
 */
int get_free_pages(unsigned int *record, unsigned int n_page)
{
	int ret = 0;
	unsigned int i;

	for (i = 0; i < n_page; i++) {
		struct phy_frame *frame;
		frame = get_free_frame();
		if (frame == NULL) {
			_error("No more physical frames available now!\n");
			ret = ENOMEM;
			goto out;
		}

		record[i] = frame->index;
		remove_free_frame(frame);
	}

out:
	return ret;
}

/**
 * get some free pages and get a copy of the content
 * from another process
 *
 * @record: the array to record the indexes of physical frames
 * @s_table: the page table to be copiied from
 * @source_brk: the brk address of the source process
 * @start_index: the start page index in the page table to be get
 * @n_page: the number of pages to get
 *
 * NOTE: This is different from map_pages_and_copy(). map_pages_and_copy()
 * would map virtual address space to physical frames, but this function
 * just get free physical pages and write their contents.
 */
int get_free_pages_and_copy(unsigned int *record,
		struct my_pte *s_table, unsigned long source_brk,
		unsigned int start_index, unsigned int n_page)
{
	int ret = 0;
	unsigned int i, end_index = start_index + n_page;
	unsigned int dest_index;

	if (s_table == page_table_0)
		dest_index = PAGE_KINDEX(source_brk);
	else
		dest_index = PAGE_UINDEX(source_brk);

	s_table[dest_index].valid = 1;
	s_table[dest_index].prot = PROT_READ | PROT_WRITE;
	for (i = start_index; i < end_index; i++) {
		struct phy_frame *frame;
		frame = get_free_frame();
		if (frame == NULL) {
			_error("No more physical frames available now!\n");
			ret = ENOMEM;
			goto out;
		}

		record[i - start_index] = frame->index;
		s_table[dest_index].pfn = frame->index;
		WriteRegister(REG_TLB_FLUSH, source_brk);
		if (s_table == page_table_0)
			memcpy(source_brk, PAGE_KADDR(i), PAGESIZE);
		else
			memcpy(source_brk, PAGE_UADDR(i), PAGESIZE);
		remove_free_frame(frame);
	}

	bzero(s_table + dest_index, sizeof(struct my_pte));
	WriteRegister(REG_TLB_FLUSH, source_brk);
out:
	return ret;
}

/**
 * update the physical frame pointer of certain page table entries
 * @table: the page table to be manipulated
 * @start_index: the start page index in the page table to be updated
 * @n_page: the number of pages to be updated
 */
int update_pages_indexes(struct my_pte *table, unsigned int start_index,
			 unsigned int n_page, unsigned int *indexes)
{
	int ret = 0;
	unsigned int i, end_index = start_index + n_page;
	for (i = start_index; i < end_index; i++) {
		if (!table[i].valid) {
			_error("%s: page [%d->%u] is invalid, DO NOT touch it!\n",
					__func__, i, table[i].pfn);
			ret = ERROR;
			goto out;
		}
		table[i].pfn = indexes[i - start_index];
		if (table == page_table_0)
			WriteRegister(REG_TLB_FLUSH, PAGE_KADDR(i));
		else
			WriteRegister(REG_TLB_FLUSH, PAGE_UADDR(i));
	}

out:
	return ret;
}

/**
 * update the property of certain page table entries
 * @table: the page table to be manipulated
 * @start_index: the start page index in the page table to be updated
 * @n_page: the number of pages to be updated
 */
int update_pages_prot(struct my_pte *table, unsigned int start_index, unsigned int n_page, int prot)
{
	int ret = 0;
	unsigned int i, end_index = start_index + n_page;
	for (i = start_index; i < end_index; i++) {
		if (!table[i].valid) {
			_error("%s: page is invalid, DO NOT touch it!\n", __func__);
			ret = ERROR;
			goto out;
		}
		table[i].prot = prot;
		if (table == page_table_0)
			WriteRegister(REG_TLB_FLUSH, PAGE_KADDR(i));
		else
			WriteRegister(REG_TLB_FLUSH, PAGE_UADDR(i));
	}

out:
	return ret;
}

#ifdef COW
/**
 * update the cow flag of certain page table entries
 * @table: the page table to be manipulated
 * @start_index: the start page index in the page table to be updated
 * @n_page: the number of pages to be updated
 */
int update_pages_cow(struct my_pte *table, unsigned int start_index, unsigned int n_page, int cow)
{
	int ret = 0;
	unsigned int i, end_index = start_index + n_page;
	for (i = start_index; i < end_index; i++) {
		if (!table[i].valid) {
			_error("%s: page is invalid, DO NOT touch it!\n", __func__);
			ret = ERROR;
			goto out;
		}
		table[i].cow = cow;
		if (table == page_table_0)
			WriteRegister(REG_TLB_FLUSH, PAGE_KADDR(i));
		else
			WriteRegister(REG_TLB_FLUSH, PAGE_UADDR(i));
	}

out:
	return ret;
}
#endif

/**
 * unmap virtual pages from physical frames and turn back
 * physical frames to free
 *
 * @table: the page table to be manipulated
 * @start_index: the start page index in the page table to be unmapped
 * @n_page: the number of pages to be unmapped
 */
int unmap_pages(struct my_pte *table, unsigned int start_index, unsigned int n_page)
{
	int ret = 0;
	unsigned int i, end_index = start_index + n_page;
	struct my_pte *pte;

	for (i = start_index; i < end_index; i++) {
		pte = table + i;
#ifdef COW
		if (!pte->cow) {
#endif
			ret = add_free_frame(pte->pfn);
			if (ret)
				goto out;
#ifdef COW
		}
#endif
		bzero(pte, sizeof(struct my_pte));
		if (table == page_table_0)
			WriteRegister(REG_TLB_FLUSH, PAGE_KADDR(i));
		else
			WriteRegister(REG_TLB_FLUSH, PAGE_UADDR(i));
	}

out:
	flush_TLB(table);
	return ret;
}

/**
 * turn back physical frames to free list without touching the
 * virtual address space
 *
 * @indexes: the physical frame indexes to be turned back to free
 * @n_page: the number of physical frmes to be turned back to free
 */
int collect_back_pages(unsigned int *indexes, unsigned int n_page)
{
	int ret = 0;
	unsigned int i;

	for (i = 0; i < n_page; i++) {
		ret = add_free_frame(indexes[i]);
		if (ret)
			goto out;
	}

out:
	return ret;
}
