#ifndef CCAN_FOREACH_H
#define CCAN_FOREACH_H
#include "config.h"
#include <stddef.h>
#include <assert.h>
#include <stdbool.h>

#if HAVE_COMPOUND_LITERALS
#if HAVE_FOR_LOOP_DECLARATION
/**
 * foreach_int - iterate over a fixed series of integers
 * @i: the int-compatible iteration variable
 * @val: one or more integer-compatible values
 *
 * This is a convenient wrapper function for setting a variable to one or
 * more explicit values in turn.  continue and break work as expected.
 *
 * Example:
 *	int i;
 *	foreach_int(i, 0, -1, 100, 0, -99) {
 *		printf("i is %i\n", i);
 *	}
 */
#define foreach_int(i, val, ...)					\
	for (unsigned _foreach_i = ((i) = val, 0);			\
	     _foreach_i < sizeof((int[]) { val, __VA_ARGS__ })/sizeof(val); \
	     (i) = (int[]) { val, __VA_ARGS__, 0 }[++_foreach_i])

/**
 * foreach_ptr - iterate over a non-NULL series of pointers
 * @i: the pointer iteration variable
 * @val: one or more compatible pointer values
 *
 * This is a convenient wrapper function for setting a variable to one
 * or more explicit values in turn.  None of the values can be NULL;
 * that is the termination condition (ie. @i will be NULL on
 * completion).  continue and break work as expected.
 *
 * Example:
 *	const char *p;
 *	foreach_ptr(p, "Hello", "world") {
 *		printf("p is %s\n", p);
 *	}
 */
#define foreach_ptr(i, val, ...)					\
	for (unsigned _foreach_i = (unsigned long)((i) = (val), 0);	\
	     (i);							\
	     (i) = ((FOREACH_TYPEOF(val)[]){(val), __VA_ARGS__, NULL})	\
		     [++_foreach_i])					\
		_foreach_no_nullval(_foreach_i, i,			\
				    ((void *[]){ val, __VA_ARGS__})))
#else /* !HAVE_FOR_LOOP_DECLARATION */
/* GCC in C89 mode still has compound literals, but no for-declarations */
#define foreach_int(i, val, ...)					\
	for ((i) = (val), _foreach_iter_init(&(i));			\
	     _foreach_iter(&(i)) < sizeof((int[]) { (val), __VA_ARGS__ }) \
			   / sizeof(int);				\
	     (i) = (int[]) { (val), __VA_ARGS__, 0 }[_foreach_iter_inc(&(i))])

#define foreach_ptr(i, val, ...)					\
	for ((i) = (val), _foreach_iter_init(&(i));			\
	     (i);							\
	     (i) = ((FOREACH_TYPEOF(val)[]){ (val), __VA_ARGS__, 0 })	\
		     [_foreach_iter_inc(&(i))],				\
		 _foreach_no_nullval(_foreach_iter(&(i)), i,		\
				     ((void *[]){ val, __VA_ARGS__})))

void _foreach_iter_init(const void *i);
unsigned int _foreach_iter(const void *i);
unsigned int _foreach_iter_inc(const void *i);

#endif /* !HAVE_FOR_LOOP_DECLARATION */

/* Make sure they don't put NULL values into array! */
#define _foreach_no_nullval(i, p, arr)				\
	assert((i) >= sizeof(arr)/sizeof(arr[0]) || (p))

#if HAVE_TYPEOF
#define FOREACH_TYPEOF(val) __typeof__(&*(val))
#else
#define FOREACH_TYPEOF(val) void *
#endif

#else /* !HAVE_COMPOUND_LITERALS */

/* No compound literals, but it's still (just) possible. */
#define foreach_int(i, val, ...)					\
	for (i = _foreach_intval_init(&(i), val, __VA_ARGS__,	\
					  _foreach_term);		\
	     !_foreach_intval_done(&i);				\
	     i = _foreach_intval_next(&(i), val, __VA_ARGS__,	\
					  _foreach_term))

#define foreach_ptr(i, val, ...)					\
	for (i = _foreach_ptrval_init(&(i), val, __VA_ARGS__, NULL);	\
	     (i);							\
	     i = _foreach_ptrval_next(&(i), val, __VA_ARGS__, NULL))

extern int _foreach_term;
int _foreach_intval_init(const void *i, int val, ...);
bool _foreach_intval_done(const void *i);
int _foreach_intval_next(const void *i, int val, ...);
void *_foreach_ptrval_init(const void *i, const void *val, ...);
void *_foreach_ptrval_next(const void *i, const void *val, ...);
#endif /* !HAVE_COMPOUND_LITERALS */

#endif /* CCAN_FOREACH_H */
