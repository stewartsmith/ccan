#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ccan/tap/tap.h>

#define printf saved_printf
static int saved_printf(const char *fmt, ...);

#define fprintf saved_fprintf
static int saved_fprintf(FILE *ignored, const char *fmt, ...);

/* Include the C files directly. */
#include <ccan/failtest/failtest.c>

static char *output = NULL;

static int saved_vprintf(const char *fmt, va_list ap)
{
	int ret = vsnprintf(NULL, 0, fmt, ap);
	int len = 0;

	if (output)
		len = strlen(output);

	output = realloc(output, len + ret + 1);
	return vsprintf(output + len, fmt, ap);
}

static int saved_printf(const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = saved_vprintf(fmt, ap);
	va_end(ap);
	return ret;
}	

static int saved_fprintf(FILE *ignored, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = saved_vprintf(fmt, ap);
	va_end(ap);
	return ret;
}	

int main(void)
{
	struct failtest_call *call;
	struct calloc_call calloc_call;
	struct malloc_call malloc_call;
	struct realloc_call realloc_call;
	struct open_call open_call;
	struct pipe_call pipe_call;
	struct read_call read_call;
	struct write_call write_call;
	char buf[20];
	unsigned int i;

	/* This is how many tests you plan to run */
	plan_tests(47);

	calloc_call.ret = calloc(1, 2);
	calloc_call.nmemb = 1;
	calloc_call.size = 2;
	call = add_history(FAILTEST_CALLOC, "run-history.c", 1, &calloc_call);
	ok1(call->type == FAILTEST_CALLOC);
	ok1(strcmp(call->file, "run-history.c") == 0);
	ok1(call->line == 1);
	ok1(call->u.calloc.ret == calloc_call.ret);
	ok1(call->u.calloc.nmemb == calloc_call.nmemb);
	ok1(call->u.calloc.size == calloc_call.size);

	malloc_call.ret = malloc(2);
	malloc_call.size = 2;
	call = add_history(FAILTEST_MALLOC, "run-history.c", 2, &malloc_call);
	ok1(call->type == FAILTEST_MALLOC);
	ok1(strcmp(call->file, "run-history.c") == 0);
	ok1(call->line == 2);
	ok1(call->u.malloc.ret == malloc_call.ret);
	ok1(call->u.malloc.size == malloc_call.size);

	realloc_call.ret = realloc(malloc_call.ret, 3);
	realloc_call.ptr = malloc_call.ret;
	realloc_call.size = 3;
	call = add_history(FAILTEST_REALLOC, "run-history.c",
			   3, &realloc_call);
	ok1(call->type == FAILTEST_REALLOC);
	ok1(strcmp(call->file, "run-history.c") == 0);
	ok1(call->line == 3);
	ok1(call->u.realloc.ret == realloc_call.ret);
	ok1(call->u.realloc.ptr == realloc_call.ptr);
	ok1(call->u.realloc.size == realloc_call.size);

	open_call.ret = open("test/run_history.c", O_RDONLY);
	open_call.pathname = "test/run_history.c";
	open_call.flags = O_RDONLY;
	open_call.mode = 0;
	call = add_history(FAILTEST_OPEN, "run-history.c", 4, &open_call);
	ok1(call->type == FAILTEST_OPEN);
	ok1(strcmp(call->file, "run-history.c") == 0);
	ok1(call->line == 4);
	ok1(call->u.open.ret == open_call.ret);
	ok1(strcmp(call->u.open.pathname, open_call.pathname) == 0);
	ok1(call->u.open.flags == open_call.flags);
	ok1(call->u.open.mode == open_call.mode);

	pipe_call.ret = pipe(pipe_call.fds);
	call = add_history(FAILTEST_PIPE, "run-history.c", 5, &pipe_call);
	ok1(call->type == FAILTEST_PIPE);
	ok1(strcmp(call->file, "run-history.c") == 0);
	ok1(call->line == 5);
	ok1(call->u.pipe.ret == pipe_call.ret);
	ok1(call->u.pipe.fds[0] == pipe_call.fds[0]);
	ok1(call->u.pipe.fds[1] == pipe_call.fds[1]);

	read_call.ret = read(open_call.ret, buf, 20);
	read_call.buf = buf;
	read_call.fd = open_call.ret;
	read_call.count = 20;
	call = add_history(FAILTEST_READ, "run-history.c", 6, &read_call);
	ok1(call->type == FAILTEST_READ);
	ok1(strcmp(call->file, "run-history.c") == 0);
	ok1(call->line == 6);
	ok1(call->u.read.ret == read_call.ret);
	ok1(call->u.read.buf == read_call.buf);
	ok1(call->u.read.fd == read_call.fd);
	ok1(call->u.read.count == read_call.count);

	write_call.ret = 20;
	write_call.buf = buf;
	write_call.fd = open_call.ret;
	write_call.count = 20;
	call = add_history(FAILTEST_WRITE, "run-history.c", 7, &write_call);
	ok1(call->type == FAILTEST_WRITE);
	ok1(strcmp(call->file, "run-history.c") == 0);
	ok1(call->line == 7);
	ok1(call->u.write.ret == write_call.ret);
	ok1(call->u.write.buf == write_call.buf);
	ok1(call->u.write.fd == write_call.fd);
	ok1(call->u.write.count == write_call.count);

	ok1(history_num == 7);

	for (i = 0; i < history_num; i++)
		history[i].fail = false;

	print_reproduce();
	ok1(strcmp(output, "To reproduce: --failpath=cmeoprw\n") == 0);
	free(output);
	output = NULL;

	for (i = 0; i < history_num; i++)
		history[i].fail = true;

	print_reproduce();
	ok1(strcmp(output, "To reproduce: --failpath=CMEOPRW\n") == 0);
	free(output);
	output = NULL;

	return exit_status();
}
