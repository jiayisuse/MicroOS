#include "yalnix.h"
#include "hardware.h"

//#define CVAR_TEST
#define PIPE_TEST
//#define COW_TEST
//#define TTY_TEST
//#define SWAP_TEST

#define CONSOLE 0
#define MAX_TERMINAL 3

#define NOFORK (-1)
#define NOEXEC (-2)

int condition = 0;

static void incursive_func(int n)
{
	if (n == 2 * PAGESIZE + 1)
		return;
	incursive_func(++n);
}

int main(int argc, char **argv)
{
	int pid, i, j, ret;
	int exit_status;
	char *exec_argv[] = { "haha", NULL };
	int *a = (int *)calloc(3, sizeof(int));
	int num_a = 100;
	char *str;
	unsigned int delay_ticks = 2;
	char buf[2 * TERMINAL_MAX_LINE + 2];
	char pipe_buf[1025];
	int pipe_fd;
	int lock_fd;
	int cvar_fd;

#ifdef SWAP_TEST
	free(a);
	a = (void *)calloc(num_a, PAGESIZE);
	if (a == NULL)
		goto loop;

	if (Fork() == 0) {
		while (1) {
			Delay(2);
			a[0] = 1000;
			TtyPrintf(CONSOLE, "pid = %u, a[0] = %u\n", GetPid(), a[0]);
		}
	}
	for (i = 0; i < num_a * PAGESIZE / sizeof(int); i += (PAGESIZE / sizeof(int)))
		a[i] = 100;

	if (Fork() == 0) {
		while (1) {
			Delay(2);
			a[0] = 2000;
			TtyPrintf(CONSOLE, "pid = %u, a[0] = %u\n", GetPid(), a[0]);
		}
	}
	for (i = 0; i < num_a * PAGESIZE / sizeof(int); i += (PAGESIZE / sizeof(int)))
		a[i] = 100;

	if (Fork() == 0) {
		while (1) {
			Delay(2);
			a[0] = 3000;
			TtyPrintf(CONSOLE, "pid = %u, a[0] = %u\n", GetPid(), a[0]);
		}
	}
	for (i = 0; i < num_a * PAGESIZE / sizeof(int); i += (PAGESIZE / sizeof(int)))
		a[i] = 100;

	if (Fork() == 0) {
		while (1) {
			Delay(2);
			a[0] = 4000;
			TtyPrintf(CONSOLE, "pid = %u, a[0] = %u\n", GetPid(), a[0]);
		}
	}
	for (i = 0; i < num_a * PAGESIZE / sizeof(int); i += (PAGESIZE / sizeof(int)))
		a[i] = 100;

	if (Fork() == 0) {
		while (1) {
			Delay(2);
			a[0] = 5000;
			TtyPrintf(CONSOLE, "pid = %u, a[0] = %u\n", GetPid(), a[0]);
		}
	}
	for (i = 0; i < num_a * PAGESIZE / sizeof(int); i += (PAGESIZE / sizeof(int)))
		a[i] = 100;
#endif

#ifdef PIPE_TEST
	ret = PipeInit(&pipe_fd);
	pid = Fork();
	bzero(pipe_buf, sizeof(pipe_buf));
	if (pid != ERROR && !pid) {
		TtyPrintf(CONSOLE, "pipe_fd = %u\n", pipe_fd);
		j = 0;
		Delay(1);
		while (1) {
			for (i = 0; i < sizeof(pipe_buf); i++)
				pipe_buf[i] = (j % 26) + 'a';
			TtyPrintf(CONSOLE, ">>>>>>>>>>> write pipe\n");
			ret = PipeWrite(pipe_fd, pipe_buf, sizeof(pipe_buf));
			TtyPrintf(CONSOLE, "write pipe ret = %d, pid = %u\n", ret, GetPid());
			j++;
		}
		Exit(0);
	}
	while (1) {
		bzero(pipe_buf, sizeof(pipe_buf));
		TtyPrintf(CONSOLE, ">>>>>>>>>>> read pipe\n");
		ret = PipeRead(pipe_fd, pipe_buf, sizeof(pipe_buf) - 7);
		TtyPrintf(CONSOLE, "<<<<<<<<<<< read pipe ret = %d, pid = %u, %s\n", ret, GetPid(), pipe_buf);
	}
	Reclaim(pipe_fd);
#endif

#ifdef CVAR_TEST
	ret = LockInit(&lock_fd);
	ret = CvarInit(&cvar_fd);
	pid = Custom0(0, 0, 0, 0);
	if (pid != ERROR && !pid) {
		Acquire(lock_fd);
		while (!condition)
			CvarWait(cvar_fd, lock_fd);
		Delay(2);
		Release(lock_fd);
		Exit(7);
	}
	Acquire(lock_fd);
	condition = 1;
	CvarSignal(cvar_fd);
	Release(lock_fd);
	ret = Reclaim(lock_fd);
	if (ret)
		Exit(-1);
#endif

#ifdef TTY_TEST
	for (i = 0; i < sizeof(buf) - 1; i++)
		buf[i] = '9';
	buf[i] = '\0';
	TtyPrintf(CONSOLE, buf);
	TtyPrintf(CONSOLE, "\n");

	a[0] = 10;
	a[2] = 100;

	TtyPrintf(CONSOLE, "Enter somthing:\n");
	bzero(buf, sizeof(buf));
	ret = TtyRead(CONSOLE, buf, sizeof(buf));
	TtyPrintf(CONSOLE, "You just entered: %s (len = %d)\n", buf, ret);
#endif

#ifdef COW_TEST
	if (argc == 2)
		delay_ticks = atoi(argv[1]);

	pid = Fork();
	if (pid == ERROR) {
		Delay(2);
		return ERROR;
	} else if (!pid) {
		GetPid();
		delay_ticks = 5;

		TtyPrintf(CONSOLE, " delay_ticks = %u, pid = %u\n", delay_ticks, GetPid());
		pid = Fork();
		if (pid != ERROR && !pid) {
			GetPid();
			delay_ticks = 8;
			TtyPrintf(CONSOLE, " delay_ticks = %u, pid = %u\n", delay_ticks, GetPid());
			Delay(delay_ticks);
			Exec("exec_test", exec_argv);
		}
		pid = Wait(&exit_status);
		TtyPrintf(CONSOLE, " delay_ticks = %u, pid = %u\n", delay_ticks, GetPid());
		TtyPrintf(CONSOLE, " wait child = %u, status = %d\n", pid, exit_status);
		Delay(delay_ticks);
		Exit(10);
	}

	pid = Fork();
	if (pid != ERROR && !pid) {
		incursive_func(0);
		GetPid();
		delay_ticks = 9;
		TtyPrintf(CONSOLE, " delay_ticks = %u, pid = %u\n", delay_ticks, GetPid());
		Delay(delay_ticks);
		Exit(100);
	}

	TtyPrintf(CONSOLE, " delay_ticks = %u, pid = %u\n", delay_ticks, GetPid());

	Delay(delay_ticks);
	GetPid();
	Wait(&exit_status);
	Wait(&exit_status);
	GetPid();
#endif

loop:
	while (1)
		Delay(delay_ticks);

	return 0;
}
