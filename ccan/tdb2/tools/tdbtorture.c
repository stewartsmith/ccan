/* this tests tdb by doing lots of ops from several simultaneous
   writers - that stresses the locking code. 
*/

#include <ccan/tdb2/tdb2.h>
#include <stdlib.h>
#include <err.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>

//#define REOPEN_PROB 30
#define DELETE_PROB 8
#define STORE_PROB 4
#define APPEND_PROB 6
#define TRANSACTION_PROB 10
#define TRANSACTION_PREPARE_PROB 2
#define LOCKSTORE_PROB 5
#define TRAVERSE_PROB 20
#define TRAVERSE_READ_PROB 20
#define TRAVERSE_MOD_PROB 100
#define TRAVERSE_ABORT_PROB 500
#define CULL_PROB 100
#define KEYLEN 3
#define DATALEN 100

static struct tdb_context *db;
static int in_transaction;
static int in_traverse;
static int error_count;
#if TRANSACTION_PROB
static int always_transaction = 0;
#endif
static int loopnum;
static int count_pipe;
static union tdb_attribute log_attr;
static union tdb_attribute seed_attr;

#ifdef PRINTF_FMT
static void tdb_log(struct tdb_context *tdb, enum tdb_debug_level level, void *private, const char *format, ...) PRINTF_FMT(4,5);
#endif
static void tdb_log(struct tdb_context *tdb, enum tdb_debug_level level, void *private, const char *format, ...)
{
	va_list ap;

	if (level != TDB_DEBUG_TRACE)
		error_count++;

	va_start(ap, format);
	vfprintf(stdout, format, ap);
	va_end(ap);
	fflush(stdout);
#if 0
	if (level != TDB_DEBUG_TRACE) {
		char *ptr;
		signal(SIGUSR1, SIG_IGN);
		asprintf(&ptr,"xterm -e gdb /proc/%d/exe %d", getpid(), getpid());
		system(ptr);
		free(ptr);
	}
#endif	
}

#include "../private.h"

static void segv_handler(int signal, siginfo_t *info, void *p)
{
	char string[100];

	sprintf(string, "%u: death at %p (map_ptr %p, map_size %llu)\n",
		getpid(), info->si_addr, db->map_ptr, db->map_size);
	write(2, string, strlen(string));
	sleep(60);
	_exit(11);
}	

static void fatal(const char *why)
{
	perror(why);
	error_count++;
}

static char *randbuf(int len)
{
	char *buf;
	int i;
	buf = (char *)malloc(len+1);

	for (i=0;i<len;i++) {
		buf[i] = 'a' + (rand() % 26);
	}
	buf[i] = 0;
	return buf;
}

static void addrec_db(void);
static int modify_traverse(struct tdb_context *tdb, TDB_DATA key, TDB_DATA dbuf,
			   void *state)
{
#if CULL_PROB
	if (random() % CULL_PROB == 0) {
		tdb_delete(tdb, key);
	}
#endif

#if TRAVERSE_MOD_PROB
	if (random() % TRAVERSE_MOD_PROB == 0) {
		addrec_db();
	}
#endif

#if TRAVERSE_ABORT_PROB
	if (random() % TRAVERSE_ABORT_PROB == 0)
		return 1;
#endif

	return 0;
}

static void addrec_db(void)
{
	int klen, dlen;
	char *k, *d;
	TDB_DATA key, data;

	klen = 1 + (rand() % KEYLEN);
	dlen = 1 + (rand() % DATALEN);

	k = randbuf(klen);
	d = randbuf(dlen);

	key.dptr = (unsigned char *)k;
	key.dsize = klen+1;

	data.dptr = (unsigned char *)d;
	data.dsize = dlen+1;

#if REOPEN_PROB
	if (in_traverse == 0 && in_transaction == 0 && random() % REOPEN_PROB == 0) {
		tdb_reopen_all(0);
		goto next;
	} 
#endif

#if TRANSACTION_PROB
	if (in_traverse == 0 && in_transaction == 0 && (always_transaction || random() % TRANSACTION_PROB == 0)) {
		if (tdb_transaction_start(db) != 0) {
			fatal("tdb_transaction_start failed");
		}
		in_transaction++;
		goto next;
	}
	if (in_traverse == 0 && in_transaction && random() % TRANSACTION_PROB == 0) {
		if (random() % TRANSACTION_PREPARE_PROB == 0) {
			if (tdb_transaction_prepare_commit(db) != 0) {
				fatal("tdb_transaction_prepare_commit failed");
			}
		}
		if (tdb_transaction_commit(db) != 0) {
			fatal("tdb_transaction_commit failed");
		}
		in_transaction--;
		goto next;
	}

	if (in_traverse == 0 && in_transaction && random() % TRANSACTION_PROB == 0) {
		tdb_transaction_cancel(db);
		in_transaction--;
		goto next;
	}
#endif

#if DELETE_PROB
	if (random() % DELETE_PROB == 0) {
		tdb_delete(db, key);
		goto next;
	}
#endif

#if STORE_PROB
	if (random() % STORE_PROB == 0) {
		if (tdb_store(db, key, data, TDB_REPLACE) != 0) {
			fatal("tdb_store failed");
		}
		goto next;
	}
#endif

#if APPEND_PROB
	if (random() % APPEND_PROB == 0) {
		if (tdb_append(db, key, data) != 0) {
			fatal("tdb_append failed");
		}
		goto next;
	}
#endif

#if LOCKSTORE_PROB
	if (random() % LOCKSTORE_PROB == 0) {
		tdb_chainlock(db, key);
		data = tdb_fetch(db, key);
		if (tdb_store(db, key, data, TDB_REPLACE) != 0) {
			fatal("tdb_store failed");
		}
		if (data.dptr) free(data.dptr);
		tdb_chainunlock(db, key);
		goto next;
	} 
#endif

#if TRAVERSE_PROB
	/* FIXME: recursive traverses break transactions? */
	if (in_traverse == 0 && random() % TRAVERSE_PROB == 0) {
		in_traverse++;
		tdb_traverse(db, modify_traverse, NULL);
		in_traverse--;
		goto next;
	}
#endif

#if TRAVERSE_READ_PROB
	if (in_traverse == 0 && random() % TRAVERSE_READ_PROB == 0) {
		in_traverse++;
		tdb_traverse_read(db, NULL, NULL);
		in_traverse--;
		goto next;
	}
#endif

	data = tdb_fetch(db, key);
	if (data.dptr) free(data.dptr);

next:
	free(k);
	free(d);
}

static int traverse_fn(struct tdb_context *tdb, TDB_DATA key, TDB_DATA dbuf,
                       void *state)
{
	tdb_delete(tdb, key);
	return 0;
}

static void usage(void)
{
	printf("Usage: tdbtorture"
#if TRANSACTION_PROB
	       " [-t]"
#endif
	       " [-k] [-n NUM_PROCS] [-l NUM_LOOPS] [-s SEED]\n");
	exit(0);
}

static void send_count_and_suicide(int sig)
{
	/* This ensures our successor can continue where we left off. */
	write(count_pipe, &loopnum, sizeof(loopnum));
	/* This gives a unique signature. */
	kill(getpid(), SIGUSR2);
}

static int run_child(int i, int seed, unsigned num_loops, unsigned start)
{
	struct sigaction act = { .sa_sigaction = segv_handler,
				 .sa_flags = SA_SIGINFO };
	sigaction(11, &act, NULL);	

	db = tdb_open("torture.tdb", TDB_DEFAULT, O_RDWR | O_CREAT, 0600,
		      &log_attr);
	if (!db) {
		fatal("db open failed");
	}

#if 0
	if (i == 0) {
		printf("pid %i\n", getpid());
		sleep(9);
	} else
		sleep(10);
#endif

	srand(seed + i);
	srandom(seed + i);

	/* Set global, then we're ready to handle being killed. */
	loopnum = start;
	signal(SIGUSR1, send_count_and_suicide);

	for (;loopnum<num_loops && error_count == 0;loopnum++) {
		addrec_db();
	}

	if (error_count == 0) {
		tdb_traverse_read(db, NULL, NULL);
#if TRANSACTION_PROB
		if (always_transaction) {
			while (in_transaction) {
				tdb_transaction_cancel(db);
				in_transaction--;
			}
			if (tdb_transaction_start(db) != 0)
				fatal("tdb_transaction_start failed");
		}
#endif
		tdb_traverse(db, traverse_fn, NULL);
		tdb_traverse(db, traverse_fn, NULL);

#if TRANSACTION_PROB
		if (always_transaction) {
			if (tdb_transaction_commit(db) != 0)
				fatal("tdb_transaction_commit failed");
		}
#endif
	}

	tdb_close(db);

	return (error_count < 100 ? error_count : 100);
}

int main(int argc, char * const *argv)
{
	int i, seed = -1;
	int num_loops = 5000;
	int num_procs = 3;
	int c, pfds[2];
	extern char *optarg;
	pid_t *pids;
	int kill_random = 0;
	int *done;

	log_attr.base.attr = TDB_ATTRIBUTE_LOG;
	log_attr.base.next = &seed_attr;
	log_attr.log.log_fn = tdb_log;
	seed_attr.base.attr = TDB_ATTRIBUTE_SEED;

	while ((c = getopt(argc, argv, "n:l:s:thk")) != -1) {
		switch (c) {
		case 'n':
			num_procs = strtol(optarg, NULL, 0);
			break;
		case 'l':
			num_loops = strtol(optarg, NULL, 0);
			break;
		case 's':
			seed = strtol(optarg, NULL, 0);
			break;
		case 't':
#if TRANSACTION_PROB
			always_transaction = 1;
#else
			fprintf(stderr, "Transactions not supported\n");
			usage();
#endif
			break;
		case 'k':
			kill_random = 1;
			break;
		default:
			usage();
		}
	}

	unlink("torture.tdb");

	if (seed == -1) {
		seed = (getpid() + time(NULL)) & 0x7FFFFFFF;
	}
	seed_attr.seed.seed = (((uint64_t)seed) << 32) | seed; 

	if (num_procs == 1 && !kill_random) {
		/* Don't fork for this case, makes debugging easier. */
		error_count = run_child(0, seed, num_loops, 0);
		goto done;
	}

	pids = (pid_t *)calloc(sizeof(pid_t), num_procs);
	done = (int *)calloc(sizeof(int), num_procs);

	if (pipe(pfds) != 0) {
		perror("Creating pipe");
		exit(1);
	}
	count_pipe = pfds[1];

	for (i=0;i<num_procs;i++) {
		if ((pids[i]=fork()) == 0) {
			close(pfds[0]);
			if (i == 0) {
				printf("testing with %d processes, %d loops, seed=%d%s\n", 
				       num_procs, num_loops, seed, 
#if TRANSACTION_PROB
				       always_transaction ? " (all within transactions)" : ""
#else
				       ""
#endif
					);
			}
			exit(run_child(i, seed, num_loops, 0));
		}
	}

	while (num_procs) {
		int status, j;
		pid_t pid;

		if (error_count != 0) {
			/* try and stop the test on any failure */
			for (j=0;j<num_procs;j++) {
				if (pids[j] != 0) {
					kill(pids[j], SIGTERM);
				}
			}
		}

		pid = waitpid(-1, &status, kill_random ? WNOHANG : 0);
		if (pid == 0) {
			struct timespec ts;

			/* Sleep for 1/10 second. */
			ts.tv_sec = 0;
			ts.tv_nsec = 100000000;
			nanosleep(&ts, NULL);

			/* Kill someone. */
			kill(pids[random() % num_procs], SIGUSR1);
			continue;
		}

		if (pid == -1) {
			perror("failed to wait for child\n");
			exit(1);
		}

		for (j=0;j<num_procs;j++) {
			if (pids[j] == pid) break;
		}
		if (j == num_procs) {
			printf("unknown child %d exited!?\n", (int)pid);
			exit(1);
		}
		if (WIFSIGNALED(status)) {
			if (WTERMSIG(status) == SIGUSR2
			    || WTERMSIG(status) == SIGUSR1) {
				/* SIGUSR2 means they wrote to pipe. */
				if (WTERMSIG(status) == SIGUSR2) {
					read(pfds[0], &done[j],
					     sizeof(done[j]));
				}
				pids[j] = fork();
				if (pids[j] == 0)
					exit(run_child(j, seed, num_loops,
						       done[j]));
				printf("Restarting child %i for %u-%u\n",
				       j, done[j], num_loops);
				continue;
			}
			printf("child %d exited with signal %d\n",
			       (int)pid, WTERMSIG(status));
			error_count++;
		} else {
			if (WEXITSTATUS(status) != 0) {
				printf("child %d exited with status %d\n",
				       (int)pid, WEXITSTATUS(status));
				error_count++;
			}
		}
		memmove(&pids[j], &pids[j+1],
			(num_procs - j - 1)*sizeof(pids[0]));
		num_procs--;
	}

	free(pids);

done:
	if (error_count == 0) {
		db = tdb_open("torture.tdb", TDB_DEFAULT, O_RDWR | O_CREAT,
			      0600, &log_attr);
		if (!db) {
			fatal("db open failed");
		}
		if (tdb_check(db, NULL, NULL) == -1) {
			printf("db check failed");
			exit(1);
		}
		tdb_close(db);
		printf("OK\n");
	}

	return error_count;
}
