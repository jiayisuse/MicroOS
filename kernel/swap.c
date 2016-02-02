#include <hardware.h>
#include <swap.h>
#include <process.h>
#include <sys.h>
#include <fcntl.h>
#include <unistd.h>

#define SWAP_PARTITION "_SWAP/"

static inline struct task_struct *pick_up_victim_task()
{
	int i;
	struct task_struct *task;

	hash_for_each(process_hash_table, i, task, hlist) {
		if (task->pid <= 1 || task->pid == current->pid)
			continue;
		if (task->swapped)
			continue;

		return task;
	}

	return NULL;
}

/**
 * swap pages out to disk
 * @table: the page table to be manipulated
 * @start_index: the start page index in the page table to be swapped out
 * @n_page: the number of pages to be swapped out
 * @fd: the file descriptor of the swapping file
 */
static int pages_swap_out(struct my_pte *table, unsigned int start_index, 
			  unsigned int n_page, int fd)
{
	int i, ret = 0;
	struct my_pte *ptep;

	for (i = start_index; i < start_index + n_page; i++) {
		ptep = table + i;
#ifdef COW
		if (ptep->cow)
			continue;
#endif

		ret = write(fd, (void *)PAGE_UADDR(i), PAGESIZE);
		if (ret != PAGESIZE) {
			ret = ERROR;
			goto out;
		}
		ptep->swap = 1;
		ptep->valid = 0;

		ret = add_free_frame(ptep->pfn);
		if (ret)
			goto out;
	}

out:
	return ret;
}

/*
 * try to find a victim process and swap it out to disk
 */
int swap_out(void)
{
	int fd;
	char file_name[16];
	struct task_struct *task = NULL;
	struct phy_free_page *page;
	int ret = 0;

	_enter("pid = %u", current->pid);

	task = pick_up_victim_task();
	if (task == NULL) {
		ret = ERROR;
		goto out;
	}

	mkdir(SWAP_PARTITION, S_IRUSR | S_IWUSR | S_IXUSR);
	sprintf(file_name, "%s%u", SWAP_PARTITION, task->pid);
	fd = open(file_name, O_RDWR | O_CREAT | O_TRUNC | O_APPEND,
			S_IRUSR | S_IWUSR);
	if (fd < 0) {
		_error("Could not open swap file %s\n", file_name);
		ret = ERROR;
		goto out;
	}

	UPDATE_VM1_AND_FLUSH_TLB(task->page_table);

	/* swap text segment out */
	ret = pages_swap_out(task->page_table, task->code_start,
			     task->code_pgn, fd);
	if (ret) {
		_error("swap #%u text segment out to \"%s\" failed!\n",
				task->pid, file_name);
		ret = EIO;
		goto swap_text_error;
	}

	/* swap data and heap segment out */
	ret = pages_swap_out(task->page_table, task->data_start,
			     PAGE_UINDEX(task->brk) - task->data_start, fd);
	if (ret) {
		_error("swap #%u text segment out to \"%s\" failed!\n",
				task->pid, file_name);
		ret = EIO;
		goto swap_data_error;
	}

swap_data_error:
	task->swapped = true;
swap_text_error:
	UPDATE_VM1_AND_FLUSH_TLB(current->page_table);
	close(fd);
out:
	_leave("swap to \"%s\", ret = %d", file_name, ret);
	return ret;
}

/**
 * swap pages in from disk
 * @table: the page table to be manipulated
 * @start_index: the start page index in the page table to be swapped in
 * @n_page: the number of pages to be swapped in
 * @fd: the file descriptor of the swapping file
 */
static int pages_swap_in(struct my_pte *table, unsigned int start_index, 
			 unsigned int n_page, int fd)
{
	int i;
	struct my_pte *ptep;
	struct phy_frame *frame;
	int ret = 0;

	for (i = start_index; i < start_index + n_page; i++) {
		ptep = table + i;
		if (!ptep->swap || ptep->valid)
			continue;

		frame = get_free_frame();
		if (frame == NULL) {
			_error("No more physical frames available now!\n");
			ret = ENOMEM;
			goto out;
		}

		ptep->pfn = frame->index;
		ptep->valid = 1;
		ptep->swap = 0;
		remove_free_frame(frame);

		ret = read(fd, (void *)PAGE_UADDR(i), PAGESIZE);
		if (ret != PAGESIZE) {
			_error("Swap_in page #%u failed! ret = %d\n", i, ret);
			add_free_frame(ptep->pfn);
			ptep->valid = 0;
			ptep->swap = 1;
			ret = ERROR;
			goto out;
		}
	}

	ret = 0;
out:
	return ret;
}

/**
 * try to swap a process in from disk
 * @task: the process to be swapped in
 */
int swap_in(struct task_struct *task)
{
	char file_name[16];
	int fd, ret = 0;

	_enter("pid = %u", task->pid);

	if (task == NULL) {
		ret = ERROR;
		goto out;
	}

	if (!task->swapped) {
		ret = ERROR;
		_error("task #%u is not swapped!\n", task->pid);
		goto out;
	}

	ret = sprintf(file_name, "%s%d", SWAP_PARTITION, task->pid);
	if (ret < 0)
		goto out;

	fd = open(file_name, O_RDONLY);
	if (fd < 0) {
		_error("swap file \"%s\" open error!\n");
		ret = ERROR;
		goto out;
	}

	UPDATE_VM1_AND_FLUSH_TLB(task->page_table);

	/* swap text segment in */
	ret = pages_swap_in(task->page_table, task->code_start,
			task->code_pgn, fd);
	if (ret) {
		_error("Swap in text segment for #%u failed!\n", task->pid);
		ret = EIO;
		goto swap_error;
	}

	/* swap data segment and heap segment in */
	ret = pages_swap_in(task->page_table, task->data_start,
			PAGE_UINDEX(task->brk) - task->data_start, fd);
	if (ret) {
		_error("Swap in data and heap segment for #%u failed!\n",
				task->pid);
		ret = EIO;
		goto swap_error;
	}

swap_error:
	UPDATE_VM1_AND_FLUSH_TLB(current->page_table);
	task->swapped = false;
	close(fd);
	unlink(file_name);
out:
	_leave("ret = %d", ret);
	return ret;
}
