#include <ccan/typesafe_cb/typesafe_cb.h>
#include <stdlib.h>

static void _register_callback(void (*cb)(void *arg, int x), void *arg)
{
}
#define register_callback(cb, arg)				\
	_register_callback(typesafe_cb_postargs(void, (cb), (arg), int), (arg))

static void my_callback(char *p, int x)
{
}

int main(int argc, char *argv[])
{
#ifdef FAIL
	int *p;
#if !HAVE_TYPEOF||!HAVE_BUILTIN_CHOOSE_EXPR||!HAVE_BUILTIN_TYPES_COMPATIBLE_P
#error "Unfortunately we don't fail if cast_if_type is a noop."
#endif
#else
	char *p;
#endif
	p = NULL;
	register_callback(my_callback, p);
	return 0;
}
