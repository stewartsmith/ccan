#include "read_write_all.h"
#include <unistd.h>
#include <errno.h>

bool write_all(int fd, const void *data, size_t size)
{
	while (size) {
		ssize_t done;

		done = write(fd, data, size);
		if (done < 0 && errno == EINTR)
			continue;
		if (done <= 0)
			return false;
		data += done;
		size -= done;
	}

	return true;
}

bool read_all(int fd, void *data, size_t size)
{
	while (size) {
		ssize_t done;

		done = read(fd, data, size);
		if (done < 0 && errno == EINTR)
			continue;
		if (done <= 0)
			return false;
		data += done;
		size -= done;
	}

	return true;
}

