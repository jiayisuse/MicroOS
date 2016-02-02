#include <hardware.h>
#include <interrupt.h>
#include <page.h>
#include <sys.h>

static void init_interupt_vector()
{
	interupt_vector = (void *)calloc(TRAP_VECTOR_SIZE,
			sizeof(*interupt_vector));

	interupt_vector[TRAP_KERNEL] = trap_kernel_handler;
	interupt_vector[TRAP_CLOCK] = trap_clock_handler;
	interupt_vector[TRAP_ILLEGAL] = trap_illegal_handler;
	interupt_vector[TRAP_MEMORY] = trap_memory_handler;
	interupt_vector[TRAP_MATH] = trap_math_handler;
	interupt_vector[TRAP_TTY_RECEIVE] = trap_tty_receive_handler;
	interupt_vector[TRAP_TTY_TRANSMIT] = trap_tty_transmit_handler;
	interupt_vector[TRAP_DISK] = trap_disk_handler;

	return;
}

/*
 * initialize kernel page table entries
 */
static void init_kernel_ptes(unsigned int page_index_start,
			     unsigned int page_index_end,
			     unsigned int valid, unsigned int prot)
{
	int i;
	struct my_pte pte;

	pte.valid = valid;
	pte.prot = prot;
	for (i = page_index_start; i < page_index_end; i++) {
		pte.pfn = i;
		page_table_0[i] = pte;
	}

	return;
}

/*
 * initialize kernel page table
 */
static void init_kernel_page_table()
{
	unsigned int page_index_start, page_index_end;
	unsigned int i;

	page_table_0 = (void *)calloc(PAGE_NR(VMEM_0_SIZE),
			sizeof(struct my_pte));

	/* initialize page table for kernel text segment */
	page_index_start = PAGE_KINDEX(_text_start);
	page_index_end = PAGE_KINDEX(_text_end);
	init_kernel_ptes(page_index_start, page_index_end, 1,
			PROT_READ | PROT_EXEC);

	/* initialize page table for kernel data segment */
	page_index_start = PAGE_KINDEX(_text_end);
	page_index_end = PAGE_KINDEX(_kbrk);
	init_kernel_ptes(page_index_start, page_index_end, 1,
			PROT_READ | PROT_WRITE);

	/* link free frames between brk and stack base */
	page_index_start = PAGE_KINDEX(_kbrk);
	page_index_end = PAGE_KINDEX(KERNEL_STACK_BASE);
	for (i = page_index_start; i < page_index_end; i++)
		add_free_frame(i);

	/* initialize page table for kernel stack */
	page_index_start = PAGE_KINDEX(KERNEL_STACK_BASE);
	page_index_end = PAGE_KINDEX(KERNEL_STACK_LIMIT);
	init_kernel_ptes(page_index_start, page_index_end, 1,
			PROT_READ | PROT_WRITE);

	/* link free frames higher than KERNEL_STACK_LIMIT */
	page_index_start = PAGE_KINDEX(KERNEL_STACK_LIMIT);
	page_index_end = PAGE_KINDEX(UP_TO_PAGE(PMEM_BASE)) + total_pages;
	for (i = page_index_start; i < page_index_end; i++)
		add_free_frame(i);

	return;
}

/*
 * callback of KernelContextSwitch() to get pages for kernel stack
 */
static KernelContext *get_kernel_context(KernelContext *kernel_ctx,
					 void *a, void *b)
{
	struct task_struct *task = a;
	task->kcontext = *kernel_ctx;
	get_free_pages_and_copy(task->stack_phy_pages,
				page_table_0, _kbrk,
				PAGE_KINDEX(KERNEL_STACK_BASE),
				PAGE_NR(KERNEL_STACK_MAXSIZE));
	return kernel_ctx;
}

void SetKernelData _PARAMS((void *data_start, void *data_end))
{
	_text_start = UP_TO_PAGE(PMEM_BASE);
	_text_end = DOWN_TO_PAGE((unsigned long)data_start);
	_data_end = DOWN_TO_PAGE((unsigned long)data_end);
	_kstack_base = KERNEL_STACK_LIMIT - 4;

	return;
}

void KernelStart _PARAMS((char **argv, unsigned int pmem_size,
			 UserContext *user_ctx))
{
	struct my_pte *init_page_table;

	if (argv[0] == NULL)
		argv[0] = "init";

	/* initialize interupt vector */
	init_interupt_vector();
	WriteRegister(REG_VECTOR_BASE, (unsigned int)interupt_vector);

	/* initialize page tables */
	total_pages = PAGE_NR(pmem_size);
	init_kernel_page_table();
	init_page_table = (void *)calloc(PAGE_NR(VMEM_1_SIZE),
					 sizeof(struct my_pte));
	WriteRegister(REG_PTBR0, (unsigned int)page_table_0);
	WriteRegister(REG_PTLR0, PAGE_NR(VMEM_0_SIZE));
	WriteRegister(REG_PTBR1, (unsigned int)init_page_table);
	WriteRegister(REG_PTLR1, PAGE_NR(VMEM_1_SIZE));

	/* enable VM */
	WriteRegister(REG_VM_ENABLE, 1);

	/* initialize idle, init and information about scheduling */
	initialize_processes_at_boot();

	/* initialie idle process and do ready_enqueue() */
	task_wake_up(&idle_task);

	/* load init process */
	init_task.page_table = init_page_table;
	sys_load(argv[0], argv, &init_task);

	current = &init_task;

	/* get kernel stack information for idle task */
	KernelContextSwitch(get_kernel_context,
			(void *)&idle_task, (void *)&idle_task);

	*user_ctx = current->ucontext;
	return;
}

int SetKernelBrk _PARAMS((void *addr))
{
	int ret = 0;
	unsigned long new_brk = (unsigned long)addr;

	_enter("_kbrk = %p", _kbrk);

	/* if has not enabled VM, just record the new brk */
	if (!ReadRegister(REG_VM_ENABLE)) {
		_kbrk = new_brk;
		return ret;
	}

	/* if has enabled VM, expand or shrink heap here */
	if (new_brk > _kbrk) {
		ret = map_pages(page_table_0,
				PAGE_KINDEX(_kbrk),
				PAGE_NR(UP_TO_PAGE(new_brk) - _kbrk),
				PROT_READ | PROT_WRITE);
		if (ret) {
			_error("Expanding kernel Brk error!\n");
			return ret;
		}
		_kbrk = UP_TO_PAGE(new_brk);
	} else if (PAGE_NR(_kbrk - new_brk) > 0) {
		ret = unmap_pages(page_table_0,
				  PAGE_KINDEX(UP_TO_PAGE(new_brk)),
				  PAGE_NR(_kbrk - UP_TO_PAGE(new_brk)));
		if (ret) {
			_error("Shrinking kernel Brk error!\n");
			return ret;
		}
		_kbrk = UP_TO_PAGE(new_brk);
	}

	_leave("_kbrk = %p", _kbrk);
	return ret;
}
