#ifndef CCAN_FAILTEST_PROTO_H
#define CCAN_FAILTEST_PROTO_H
#include <stdlib.h>

/* Potentially-failing versions of routines; #defined in failtest.h */
void *failtest_calloc(size_t nmemb, size_t size,
		      const char *file, unsigned line);
void *failtest_malloc(size_t size, const char *file, unsigned line);
void *failtest_realloc(void *ptr, size_t size,
		       const char *file, unsigned line);
int failtest_open(const char *pathname, int flags,
		  const char *file, unsigned line, ...);
int failtest_pipe(int pipefd[2], const char *file, unsigned line);
ssize_t failtest_read(int fd, void *buf, size_t count,
		      const char *file, unsigned line);
ssize_t failtest_write(int fd, const void *buf, size_t count,
		       const char *file, unsigned line);
int failtest_close(int fd);

#endif /* CCAN_FAILTEST_PROTO_H */
