#include <ccan/daemonize/daemonize.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/* This code is based on Stevens Advanced Programming in the UNIX
 * Environment. */
bool daemonize(void)
{
	pid_t pid;

	/* Separate from our parent via fork, so init inherits us. */
	if ((pid = fork()) < 0)
		return false;
	if (pid != 0)
		exit(0);

	/* Don't hold files open. */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	/* Many routines write to stderr; that can cause chaos if used
	 * for something else, so set it here. */
	if (open("/dev/null", O_WRONLY) != 0)
		return false;
	if (dup2(0, STDERR_FILENO) != STDERR_FILENO)
		return false;
	close(0);

	/* Session leader so ^C doesn't whack us. */
	setsid();
	/* Move off any mount points we might be in. */
	if (chdir("/") != 0)
		return false;

	/* Discard our parent's old-fashioned umask prejudices. */
	umask(0);
	return true;
}
