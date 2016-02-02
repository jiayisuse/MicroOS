#include <yalnix.h>
#include <fcntl.h>
#include <unistd.h>
#include <hardware.h>
#include <load_info.h>
#include <process.h>
#include <page.h>
#include <sys.h>
#include "internal.h"

/*
 * Load a program into an existing address space.  The program comes from
 * the Linux file named "name", and its arguments come from the array at
 * "args", which is in standard argv format.  The argument "proc" points
 * to the process or PCB structure for the process into which the program
 * is to be loaded. 
 */
int sys_load(char *filename, char **args, struct task_struct *task) 
{
	int fd;
	int (*entry)();
	struct load_info li;
	int i;
	int ret;
	char *cp;
	char **cpp;
	char *cp2;
	int argcount;
	int size;
	int text_pg1;
	int data_pg1;
	int data_npg;
	int stack_npg;
	long segment_size;
	char *argbuf;
	struct my_pte *page_table;

	/* open the executable file */
	if ((fd = open(filename, O_RDONLY)) < 0) {
		TracePrintf(0, "LoadProgram: can't open file '%s'\n", filename);
		return ERROR;
	}

	if (LoadInfo(fd, &li) != LI_NO_ERROR) {
		TracePrintf(0, "LoadProgram: '%s' not in Yalnix format\n", filename);
		close(fd);
		return (-1);
	}

	if (li.entry < VMEM_1_BASE) {
		TracePrintf(0, "LoadProgram: '%s' not linked for Yalnix\n", filename);
		close(fd);
		return ERROR;
	}

	/* Figure out in what region 1 page the different program sections
	 * start and end */
	text_pg1 = (li.t_vaddr - VMEM_1_BASE) >> PAGESHIFT;
	data_pg1 = (li.id_vaddr - VMEM_1_BASE) >> PAGESHIFT;
	data_npg = li.id_npg + li.ud_npg;

	/* Figure out how many bytes are needed to hold the arguments on
	 * the new stack that we are building. Also count the number of
	 * arguments, to become the argc that the new "main" gets called
	 * with. */
	size = 0;
	for (i = 0; args[i] != NULL; i++) {
		_debug("counting arg %d = '%s'\n", i, args[i]);
		size += strlen(args[i]) + 1;
	}
	argcount = i;

	TracePrintf(2, "LoadProgram: argsize %d, argcount %d\n", size, argcount);
  
	/* The arguments will get copied starting at "cp", and the argv
	 * pointers to the arguments (and the argc value) will get built
	 * starting at "cpp".  The value for "cpp" is computed by subtracting
	 * off space for the number of arguments (plus 3, for the argc value,
	 * a NULL pointer terminating the argv pointers, and a NULL pointer
	 * terminating the envp pointers) times the size of each,
	 * and then rounding the value *down* to a double-word boundary. */
	cp = ((char *)VMEM_1_LIMIT) - size;

	cpp = (char **)
		(((int)cp - 
		  ((argcount + 3 + POST_ARGV_NULL_SPACE) * sizeof(void *))) 
		 & ~7);

	/* Compute the new stack pointer, leaving INITIAL_STACK_FRAME_SIZE bytes
	 * reserved above the stack pointer, before the arguments. */
	cp2 = (caddr_t)cpp - INITIAL_STACK_FRAME_SIZE;



	TracePrintf(1, "prog_size %d, text %d data %d bss %d pages\n",
			li.t_npg + data_npg, li.t_npg, li.id_npg, li.ud_npg);


	/* Compute how many pages we need for the stack */
	stack_npg = PAGE_NR(VMEM_1_LIMIT - DOWN_TO_PAGE(cp2));

	TracePrintf(1, "LoadProgram: heap_size %d, stack_size %d\n",
			li.t_npg + data_npg, stack_npg);


	/* leave at least one page between heap and stack */
	if (stack_npg + data_pg1 + data_npg >= MAX_PT_LEN) {
		close(fd);
		return ERROR;
	}

	/* This completes all the checks before we proceed to actually load
	 * the new program.  From this point on, we are committed to either
	 * loading succesfully or killing the process */

	/* Set the new stack pointer value in the process's exception frame */
	task->ucontext.sp = cp2;

	/* Now save the arguments in a separate buffer in region 0, since
	 * we are about to blow away all of region 1 */
	cp2 = argbuf = (char *)malloc(size);
	if (cp2 == NULL) {
		_error("LoadProgram: memory out!\n");
		return ERROR;
	}

	for (i = 0; args[i] != NULL; i++) {
		TracePrintf(3, "saving arg %d = '%s'\n", i, args[i]);
		strcpy(cp2, args[i]);
		cp2 += strlen(cp2) + 1;
	}

	/* allocate page table for this process */
	if (task->page_table == NULL) {
		task->page_table = (void *)calloc(PAGE_NR(VMEM_1_SIZE),
				sizeof(struct my_pte));
		if (task->page_table == NULL) {
			_error("LoadProgram: page table memory out!\n");
			return ERROR;
		}
	} else
		task_address_space_unmap(task);

	page_table = task->page_table;

	/* allocate memory for user text */
	ret = map_pages(page_table, text_pg1, li.t_npg, PROT_READ | PROT_WRITE);
	if (ret) {
		_error("Map pages for text segment of %s error!\n", filename);
		return ret;
	}
	task->code_start = text_pg1;
	task->code_pgn = li.t_npg;

	/* allocate memory for user data */
	ret = map_pages(page_table, data_pg1, data_npg, PROT_READ | PROT_WRITE);
	if (ret) {
		_error("Map pages for data segment of %s error!\n", filename);
		return ret;
	}
	task->data_start = data_pg1;
	task->data_pgn = data_npg;
	task->brk = PAGE_UADDR(data_pg1 + data_npg);

	/* allocate memory for the user stack too */
	ret = map_pages(page_table, PAGE_UINDEX(cpp), stack_npg,
			PROT_READ | PROT_WRITE);
	if (ret) {
		_error("Map pages for data segment of %s error!\n", filename);
		return ret;
	}
	task->stack_start = PAGE_UINDEX(cpp);
	task->stack_pgn = stack_npg;

	/* change page table and flush TLB for VM_1 */
	UPDATE_VM1_AND_FLUSH_TLB(page_table);

	/* read the text from the file into memory */
	lseek(fd, li.t_faddr, SEEK_SET);
	segment_size = li.t_npg << PAGESHIFT;
	if (read(fd, (void *)li.t_vaddr, segment_size) != segment_size) {
		_error("LoadProgram: Read text segment ERROR!\n");
		close(fd);
		return EIO;
	}

	/* read the data from the file into memory */
	lseek(fd, li.id_faddr, 0);
	segment_size = li.id_npg << PAGESHIFT;
	if (read(fd, (void *)li.id_vaddr, segment_size) != segment_size) {
		_error("LoadProgram: Read text segment ERROR!\n");
		close(fd);
		return EIO;
	}

	close(fd);

	/* Now set the page table entries for the program text to be readable
	 * and executable, but not writable */
	ret = update_pages_prot(page_table, text_pg1, li.t_npg,
				PROT_READ | PROT_EXEC);
	if (ret) {
		_error("LoadProgram: update text segement for \"%s\" ERROR!\n",
				filename);
		return ERROR;
	}

	/* zero out the uninitialized data area */
	bzero(li.id_end, li.ud_end - li.id_end);

	/* set the entry point in the exception frame */
	task->ucontext.pc = (caddr_t)li.entry;

	/* now, finally, build the argument list on the new stack */
#ifdef LINUX
	memset(cpp, 0x00, VMEM_1_LIMIT - ((int)cpp));
#endif
	*cpp++ = (char *)argcount;		/* the first value at cpp is argc */
	cp2 = argbuf;
	for (i = 0; i < argcount; i++) {	/* copy each argument and set argv */
		*cpp++ = cp;
		strcpy(cp, cp2);
		cp += strlen(cp) + 1;
		cp2 += strlen(cp2) + 1;
	}
	free(argbuf);
	*cpp++ = NULL;			/* the last argv is a NULL pointer */
	*cpp++ = NULL;			/* a NULL pointer for an empty envp */

	task->state = TASK_READY;

	return 0;
}
