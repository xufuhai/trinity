#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ptrace.h>

#include "trinity.h"
#include "shm.h"
#include "files.h"
#include "syscall.h"

static void regenerate()
{
	if (syscallcount >= shm->regenerate)
		return;

	shm->regenerating = TRUE;

	sleep(1);	/* give children time to finish with fds. */

	shm->regenerate = 0;

	output("[%d] Regenerating random pages, fd's etc.\n", getpid());

	regenerate_fds();

	destroy_maps();
	setup_maps();

	regenerate_random_page();

	shm->regenerating = FALSE;
}

unsigned char do_check_tainted;

int check_tainted(void)
{
	int fd;
	int ret;
	char buffer[4];

	fd = open("/proc/sys/kernel/tainted", O_RDONLY);
	if (!fd)
		return -1;
	ret = read(fd, buffer, 3);
	close(fd);
	ret = atoi(buffer);

	return ret;
}

int find_pid_slot(pid_t mypid)
{
	unsigned int i;

	for (i = 0; i < shm->nr_childs; i++) {
		if (shm->pids[i] == mypid)
			return i;
	}
	return -1;
}

static unsigned char pidmap_empty()
{
	unsigned int i;

	for (i = 0; i < shm->nr_childs; i++) {
		if (shm->pids[i] == -1)
			continue;
		if (shm->pids[i] != 0)
			return FALSE;
	}
	return TRUE;
}


#define debugf if (debug == TRUE) printf

static void fork_children()
{
	int pidslot;
	static char childname[17];

	/* Generate children*/

	while (shm->running_childs < shm->nr_childs) {
		int pid = 0;

		/* Find a space for it in the pid map */
		pidslot = find_pid_slot(-1);
		if (pidslot == -1) {
			printf("[%d] ## Pid map was full!\n", getpid());
			exit(EXIT_FAILURE);
		}
		if ((unsigned int)pidslot >= shm->nr_childs) {
			printf("[%d] ## Pid map was full!\n", getpid());
			exit(EXIT_FAILURE);
		}

		/*
		 * consume some randomness. otherwise each child starts
		 *  with the same random seed, and ends up doing identical syscalls.
		 */
		(void) rand();

		(void)alarm(0);
		fflush(stdout);
		pid = fork();
		if (pid != 0)
			shm->pids[pidslot] = pid;
		else {
			int ret = 0;
			memset(childname, 0, sizeof(childname));
			sprintf(childname, "trinity-child%d", pidslot);
			prctl(PR_SET_NAME, (unsigned long) &childname);

			/* Wait for parent to set our pidslot */
			while (shm->pids[pidslot] != getpid());

			ret = child_process();

			output("child %d exitting\n", getpid());

			_exit(ret);
		}
		shm->running_childs++;
		debugf("[%d] Created child %d [total:%d/%d]\n",
			getpid(), shm->pids[pidslot],
			shm->running_childs, shm->nr_childs);

		if (shm->exit_now == TRUE)
			return;

	}
	debugf("[%d] created enough children\n", getpid());
}

void reap_child(pid_t childpid)
{
	int i;

	i = find_pid_slot(childpid);
	if (i != -1) {
		debugf("[%d] Removing %d from pidmap.\n", getpid(), shm->pids[i]);
		shm->pids[i] = -1;
		shm->running_childs--;
		shm->tv[i].tv_sec = 0;
	}
}

static void handle_children()
{
	int childpid, childstatus;
	unsigned int i;
	int slot;

	childpid = waitpid(-1, &childstatus, WUNTRACED | WCONTINUED);

	switch (childpid) {
	case 0:
		debugf("[%d] Nothing changed. children:%d\n", getpid(), shm->running_childs);
		break;

	case -1:
		if (shm->exit_now == TRUE)
			return;

		if (errno == ECHILD) {
			debugf("[%d] All children exited!\n", getpid());
			for (i = 0; i < shm->nr_childs; i++) {
				if (shm->pids[i] != -1) {
					debugf("[%d] Removing %d from pidmap\n", getpid(), shm->pids[i]);
					shm->pids[i] = -1;
					shm->running_childs--;
				}
			}
			break;
		}
		output("error! (%s)\n", strerror(errno));
		break;

	default:
		debugf("[%d] Something happened to pid %d\n", getpid(), childpid);
again:
		if (WIFEXITED(childstatus)) {

			slot = find_pid_slot(childpid);
			if (slot == -1) {
				printf("[%d] ## Couldn't find pid slot for %d\n", getpid(), childpid);
				shm->exit_now = TRUE;

				for (i = 0; i < shm->nr_childs; i++)
					printf("slot%d: %d\n", i, shm->pids[i]);
			} else
				debugf("[%d] Child %d exited after %d syscalls.\n", getpid(), childpid, shm->total_syscalls[slot]);
			reap_child(childpid);
			break;

		} else if (WIFSIGNALED(childstatus)) {
			/* it's a child */
			switch (WTERMSIG(childstatus)) {
			case SIGFPE:
			case SIGSEGV:
			case SIGKILL:
			case SIGALRM:
			case SIGPIPE:
			case SIGABRT:
				debugf("[%d] got a signal from pid %d (%s)\n", getpid(), childpid, strsignal(WTERMSIG(childstatus)));
				reap_child(childpid);
				break;
			default:
				debugf("[%d] ** Child got an unhandled signal (%d)\n", getpid(), WTERMSIG(childstatus));
					break;
			}
			break;

		} else if (WIFSTOPPED(childstatus)) {
			debugf("[%d] Child %d was stopped (%s).\n", getpid(), childpid, strsignal(WSTOPSIG(childstatus)));
			if (WSTOPSIG(childstatus) == SIGSTOP) {
				debugf("[%d] Sending PTRACE_CONT (and then KILL)\n", getpid());
				ptrace(PTRACE_CONT, childpid, NULL, NULL);
			}
			kill(childpid, SIGKILL);
			reap_child(childpid);
		} else if (WIFCONTINUED(childstatus)) {
			break;
		} else {
			output("erk, wtf\n");
		}
	}

	/* anything else to process ? */
	sleep(1);	/* Give other children a chance to do something. */
	childpid = waitpid(-1, &childstatus, WUNTRACED | WCONTINUED | WNOHANG);
	if (childpid == 0)
		return;
	if (childpid == -1)
		return;

	goto again;

}

void main_loop()
{
	static const char taskname[13]="trinity-main";
	int childstatus;
	pid_t pid;


	fflush(stdout);
	pid = fork();
	if (pid == 0)
		watchdog();	// Never returns.

	/* do an extra fork so that the watchdog and the children don't share a common parent */
	pid = fork();
	if (pid != 0) {
		pid = waitpid(pid, &childstatus, WUNTRACED | WCONTINUED);
		shm->parentpid = getpid(); /* reset */
		return;
	}

	shm->parentpid = getpid();

	prctl(PR_SET_NAME, (unsigned long) &taskname);

	while (shm->watchdog_pid == 0)
		sleep(1);

	printf("[%d] Started watchdog thread %d\n", getpid(), shm->watchdog_pid);

	while (shm->exit_now == FALSE) {
		fork_children();
		handle_children();

		if (shm->regenerate >= REGENERATION_POINT)
			regenerate();

//		output("regenerate:%d\n", shm->regenerate);
	}
	wait_for_watchdog_to_exit();

	while (!(pidmap_empty()))
		handle_children();

	printf("[%d] Bailing main loop\n", getpid());
}
