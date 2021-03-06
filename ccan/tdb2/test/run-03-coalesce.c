#include <ccan/tdb2/tdb.c>
#include <ccan/tdb2/free.c>
#include <ccan/tdb2/lock.c>
#include <ccan/tdb2/io.c>
#include <ccan/tdb2/hash.c>
#include <ccan/tdb2/check.c>
#include <ccan/tdb2/transaction.c>
#include <ccan/tap/tap.h>
#include "logging.h"
#include "layout.h"

static tdb_len_t free_record_length(struct tdb_context *tdb, tdb_off_t off)
{
	struct tdb_free_record f;

	if (tdb_read_convert(tdb, off, &f, sizeof(f)) != 0)
		return TDB_OFF_ERR;
	if (frec_magic(&f) != TDB_FREE_MAGIC)
		return TDB_OFF_ERR;
	return frec_len(&f);
}

int main(int argc, char *argv[])
{
	tdb_off_t b_off;
	struct tdb_context *tdb;
	struct tdb_layout *layout;
	struct tdb_data data, key;
	tdb_len_t len;

	/* FIXME: Test TDB_CONVERT */

	plan_tests(38);
	data.dptr = (void *)"world";
	data.dsize = 5;
	key.dptr = (void *)"hello";
	key.dsize = 5;

	/* No coalescing can be done due to EOF */
	layout = new_tdb_layout(NULL);
	tdb_layout_add_freetable(layout);
	len = 1024;
	tdb_layout_add_free(layout, len, 0);
	tdb = tdb_layout_get(layout);
	ok1(tdb_check(tdb, NULL, NULL) == 0);
	ok1(free_record_length(tdb, layout->elem[1].base.off) == len);

	/* Figure out which bucket free entry is. */
	b_off = bucket_off(tdb->ftable_off, size_to_bucket(len));
	/* Lock and fail to coalesce. */
	ok1(tdb_lock_free_bucket(tdb, b_off, TDB_LOCK_WAIT) == 0);
	ok1(coalesce(tdb, layout->elem[1].base.off, b_off, len) == 0);
	tdb_unlock_free_bucket(tdb, b_off);
	ok1(free_record_length(tdb, layout->elem[1].base.off) == len);
	ok1(tdb_check(tdb, NULL, NULL) == 0);
	tdb_close(tdb);

	/* No coalescing can be done due to used record */
	layout = new_tdb_layout(NULL);
	tdb_layout_add_freetable(layout);
	tdb_layout_add_free(layout, 1024, 0);
	tdb_layout_add_used(layout, key, data, 6);
	tdb = tdb_layout_get(layout);
	ok1(free_record_length(tdb, layout->elem[1].base.off) == 1024);
	ok1(tdb_check(tdb, NULL, NULL) == 0);

	/* Figure out which bucket free entry is. */
	b_off = bucket_off(tdb->ftable_off, size_to_bucket(1024));
	/* Lock and fail to coalesce. */
	ok1(tdb_lock_free_bucket(tdb, b_off, TDB_LOCK_WAIT) == 0);
	ok1(coalesce(tdb, layout->elem[1].base.off, b_off, 1024) == 0);
	tdb_unlock_free_bucket(tdb, b_off);
	ok1(free_record_length(tdb, layout->elem[1].base.off) == 1024);
	ok1(tdb_check(tdb, NULL, NULL) == 0);
	tdb_close(tdb);

	/* Coalescing can be done due to two free records, then EOF */
	layout = new_tdb_layout(NULL);
	tdb_layout_add_freetable(layout);
	tdb_layout_add_free(layout, 1024, 0);
	tdb_layout_add_free(layout, 2048, 0);
	tdb = tdb_layout_get(layout);
	ok1(free_record_length(tdb, layout->elem[1].base.off) == 1024);
	ok1(free_record_length(tdb, layout->elem[2].base.off) == 2048);
	ok1(tdb_check(tdb, NULL, NULL) == 0);

	/* Figure out which bucket (first) free entry is. */
	b_off = bucket_off(tdb->ftable_off, size_to_bucket(1024));
	/* Lock and coalesce. */
	ok1(tdb_lock_free_bucket(tdb, b_off, TDB_LOCK_WAIT) == 0);
	ok1(coalesce(tdb, layout->elem[1].base.off, b_off, 1024) == 1);
	ok1(tdb->allrecord_lock.count == 0 && tdb->num_lockrecs == 0);
	ok1(free_record_length(tdb, layout->elem[1].base.off)
	    == 1024 + sizeof(struct tdb_used_record) + 2048);
	ok1(tdb_check(tdb, NULL, NULL) == 0);
	tdb_close(tdb);

	/* Coalescing can be done due to two free records, then data */
	layout = new_tdb_layout(NULL);
	tdb_layout_add_freetable(layout);
	tdb_layout_add_free(layout, 1024, 0);
	tdb_layout_add_free(layout, 512, 0);
	tdb_layout_add_used(layout, key, data, 6);
	tdb = tdb_layout_get(layout);
	ok1(free_record_length(tdb, layout->elem[1].base.off) == 1024);
	ok1(free_record_length(tdb, layout->elem[2].base.off) == 512);
	ok1(tdb_check(tdb, NULL, NULL) == 0);

	/* Figure out which bucket free entry is. */
	b_off = bucket_off(tdb->ftable_off, size_to_bucket(1024));
	/* Lock and coalesce. */
	ok1(tdb_lock_free_bucket(tdb, b_off, TDB_LOCK_WAIT) == 0);
	ok1(coalesce(tdb, layout->elem[1].base.off, b_off, 1024) == 1);
	ok1(tdb->allrecord_lock.count == 0 && tdb->num_lockrecs == 0);
	ok1(free_record_length(tdb, layout->elem[1].base.off)
	    == 1024 + sizeof(struct tdb_used_record) + 512);
	ok1(tdb_check(tdb, NULL, NULL) == 0);
	tdb_close(tdb);

	/* Coalescing can be done due to three free records, then EOF */
	layout = new_tdb_layout(NULL);
	tdb_layout_add_freetable(layout);
	tdb_layout_add_free(layout, 1024, 0);
	tdb_layout_add_free(layout, 512, 0);
	tdb_layout_add_free(layout, 256, 0);
	tdb = tdb_layout_get(layout);
	ok1(free_record_length(tdb, layout->elem[1].base.off) == 1024);
	ok1(free_record_length(tdb, layout->elem[2].base.off) == 512);
	ok1(free_record_length(tdb, layout->elem[3].base.off) == 256);
	ok1(tdb_check(tdb, NULL, NULL) == 0);

	/* Figure out which bucket free entry is. */
	b_off = bucket_off(tdb->ftable_off, size_to_bucket(1024));
	/* Lock and coalesce. */
	ok1(tdb_lock_free_bucket(tdb, b_off, TDB_LOCK_WAIT) == 0);
	ok1(coalesce(tdb, layout->elem[1].base.off, b_off, 1024) == 1);
	ok1(tdb->allrecord_lock.count == 0 && tdb->num_lockrecs == 0);
	ok1(free_record_length(tdb, layout->elem[1].base.off)
	    == 1024 + sizeof(struct tdb_used_record) + 512
	    + sizeof(struct tdb_used_record) + 256);
	ok1(tdb_check(tdb, NULL, NULL) == 0);
	tdb_close(tdb);

	ok1(tap_log_messages == 0);
	return exit_status();
}
