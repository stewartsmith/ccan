 /* 
   Trivial Database 2: human-readable summary code
   Copyright (C) Rusty Russell 2010
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "private.h"
#include <assert.h>
#include <ccan/tally/tally.h>

static int count_hash(struct tdb_context *tdb,
		      tdb_off_t hash_off, unsigned bits)
{
	const tdb_off_t *h;
	unsigned int i, count = 0;

	h = tdb_access_read(tdb, hash_off, sizeof(*h) << bits, true);
	if (!h)
		return -1;
	for (i = 0; i < (1 << bits); i++)
		count += (h[i] != 0);

	tdb_access_release(tdb, h);
	return count;
}

static bool summarize(struct tdb_context *tdb,
		      struct tally *hashes,
		      struct tally *ftables,
		      struct tally *free,
		      struct tally *keys,
		      struct tally *data,
		      struct tally *extra,
		      struct tally *uncoal,
		      struct tally *buckets,
		      struct tally *chains)
{
	tdb_off_t off;
	tdb_len_t len;
	tdb_len_t unc = 0;

	for (off = sizeof(struct tdb_header); off < tdb->map_size; off += len) {
		const union {
			struct tdb_used_record u;
			struct tdb_free_record f;
			struct tdb_recovery_record r;
		} *p;
		/* We might not be able to get the whole thing. */
		p = tdb_access_read(tdb, off, sizeof(p->f), true);
		if (!p)
			return false;
		if (p->r.magic == TDB_RECOVERY_INVALID_MAGIC
		    || p->r.magic == TDB_RECOVERY_MAGIC) {
			if (unc) {
				tally_add(uncoal, unc);
				unc = 0;
			}
			len = sizeof(p->r) + p->r.max_len;
		} else if (frec_magic(&p->f) == TDB_FREE_MAGIC) {
			len = frec_len(&p->f);
			tally_add(free, len);
			tally_add(buckets, size_to_bucket(len));
			len += sizeof(p->u);
			unc++;
		} else if (rec_magic(&p->u) == TDB_USED_MAGIC) {
			if (unc) {
				tally_add(uncoal, unc);
				unc = 0;
			}
			len = sizeof(p->u)
				+ rec_key_length(&p->u)
				+ rec_data_length(&p->u)
				+ rec_extra_padding(&p->u);

			tally_add(keys, rec_key_length(&p->u));
			tally_add(data, rec_data_length(&p->u));
			tally_add(extra, rec_extra_padding(&p->u));
		} else if (rec_magic(&p->u) == TDB_HTABLE_MAGIC) {
			int count = count_hash(tdb,
					       off + sizeof(p->u),
					       TDB_SUBLEVEL_HASH_BITS);
			if (count == -1)
				return false;
			tally_add(hashes, count);
			tally_add(extra, rec_extra_padding(&p->u));
			len = sizeof(p->u)
				+ rec_data_length(&p->u)
				+ rec_extra_padding(&p->u);
		} else if (rec_magic(&p->u) == TDB_FTABLE_MAGIC) {
			len = sizeof(p->u)
				+ rec_data_length(&p->u)
				+ rec_extra_padding(&p->u);
			tally_add(ftables, rec_data_length(&p->u));
			tally_add(extra, rec_extra_padding(&p->u));
		} else if (rec_magic(&p->u) == TDB_CHAIN_MAGIC) {
			len = sizeof(p->u)
				+ rec_data_length(&p->u)
				+ rec_extra_padding(&p->u);
			tally_add(chains, 1);
			tally_add(extra, rec_extra_padding(&p->u));
		} else
			len = dead_space(tdb, off);
		tdb_access_release(tdb, p);
	}
	if (unc)
		tally_add(uncoal, unc);
	return true;
}

#define SUMMARY_FORMAT \
	"Size of file/data: %zu/%zu\n" \
	"Number of records: %zu\n" \
	"Smallest/average/largest keys: %zu/%zu/%zu\n%s" \
	"Smallest/average/largest data: %zu/%zu/%zu\n%s" \
	"Smallest/average/largest padding: %zu/%zu/%zu\n%s" \
	"Number of free records: %zu\n" \
	"Smallest/average/largest free records: %zu/%zu/%zu\n%s" \
	"Number of uncoalesced records: %zu\n" \
	"Smallest/average/largest uncoalesced runs: %zu/%zu/%zu\n%s" \
	"Number of free lists: %zu\n%s" \
	"Toplevel hash used: %u of %u\n" \
	"Number of chains: %zu\n" \
	"Number of subhashes: %zu\n" \
	"Smallest/average/largest subhash entries: %zu/%zu/%zu\n%s" \
	"Percentage keys/data/padding/free/rechdrs/freehdrs/hashes: %.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f\n"

#define BUCKET_SUMMARY_FORMAT_A					\
	"Free bucket %zu: total entries %zu.\n"			\
	"Smallest/average/largest length: %zu/%zu/%zu\n%s"
#define BUCKET_SUMMARY_FORMAT_B					\
	"Free bucket %zu-%zu: total entries %zu.\n"		\
	"Smallest/average/largest length: %zu/%zu/%zu\n%s"

#define HISTO_WIDTH 70
#define HISTO_HEIGHT 20

char *tdb_summary(struct tdb_context *tdb, enum tdb_summary_flags flags)
{
	tdb_len_t len;
	struct tally *ftables, *hashes, *freet, *keys, *data, *extra, *uncoal,
		*buckets, *chains;
	char *hashesg, *freeg, *keysg, *datag, *extrag, *uncoalg, *bucketsg;
	char *ret = NULL;

	hashesg = freeg = keysg = datag = extrag = uncoalg = bucketsg = NULL;

	if (tdb_allrecord_lock(tdb, F_RDLCK, TDB_LOCK_WAIT, false) != 0)
		return NULL;

	if (tdb_lock_expand(tdb, F_RDLCK) != 0) {
		tdb_allrecord_unlock(tdb, F_RDLCK);
		return NULL;
	}

	/* Start stats off empty. */
	ftables = tally_new(HISTO_HEIGHT);
	hashes = tally_new(HISTO_HEIGHT);
	freet = tally_new(HISTO_HEIGHT);
	keys = tally_new(HISTO_HEIGHT);
	data = tally_new(HISTO_HEIGHT);
	extra = tally_new(HISTO_HEIGHT);
	uncoal = tally_new(HISTO_HEIGHT);
	buckets = tally_new(HISTO_HEIGHT);
	chains = tally_new(HISTO_HEIGHT);
	if (!ftables || !hashes || !freet || !keys || !data || !extra
	    || !uncoal || !buckets || !chains) {
		tdb_logerr(tdb, TDB_ERR_OOM, TDB_DEBUG_ERROR,
			   "tdb_summary: failed to allocate tally structures");
		goto unlock;
	}

	if (!summarize(tdb, hashes, ftables, freet, keys, data, extra, uncoal,
		       buckets, chains))
		goto unlock;

	if (flags & TDB_SUMMARY_HISTOGRAMS) {
		hashesg = tally_histogram(hashes, HISTO_WIDTH, HISTO_HEIGHT);
		freeg = tally_histogram(freet, HISTO_WIDTH, HISTO_HEIGHT);
		keysg = tally_histogram(keys, HISTO_WIDTH, HISTO_HEIGHT);
		datag = tally_histogram(data, HISTO_WIDTH, HISTO_HEIGHT);
		extrag = tally_histogram(extra, HISTO_WIDTH, HISTO_HEIGHT);
		uncoalg = tally_histogram(uncoal, HISTO_WIDTH, HISTO_HEIGHT);
		bucketsg = tally_histogram(buckets, HISTO_WIDTH, HISTO_HEIGHT);
	}

	/* 20 is max length of a %llu. */
	len = strlen(SUMMARY_FORMAT) + 33*20 + 1
		+ (hashesg ? strlen(hashesg) : 0)
		+ (freeg ? strlen(freeg) : 0)
		+ (keysg ? strlen(keysg) : 0)
		+ (datag ? strlen(datag) : 0)
		+ (extrag ? strlen(extrag) : 0)
		+ (uncoalg ? strlen(uncoalg) : 0)
		+ (bucketsg ? strlen(bucketsg) : 0);

	ret = malloc(len);
	if (!ret)
		goto unlock;

	len = sprintf(ret, SUMMARY_FORMAT,
		      (size_t)tdb->map_size,
		      tally_num(keys) + tally_num(data),
		      tally_num(keys),
		      tally_min(keys), tally_mean(keys), tally_max(keys),
		      keysg ? keysg : "",
		      tally_min(data), tally_mean(data), tally_max(data),
		      datag ? datag : "",
		      tally_min(extra), tally_mean(extra), tally_max(extra),
		      extrag ? extrag : "",
		      tally_num(freet),
		      tally_min(freet), tally_mean(freet), tally_max(freet),
		      freeg ? freeg : "",
		      tally_total(uncoal, NULL),
		      tally_min(uncoal), tally_mean(uncoal), tally_max(uncoal),
		      uncoalg ? uncoalg : "",
		      tally_num(buckets),
		      bucketsg ? bucketsg : "",
		      count_hash(tdb, offsetof(struct tdb_header, hashtable),
				 TDB_TOPLEVEL_HASH_BITS),
		      1 << TDB_TOPLEVEL_HASH_BITS,
		      tally_num(chains),
		      tally_num(hashes),
		      tally_min(hashes), tally_mean(hashes), tally_max(hashes),
		      hashesg ? hashesg : "",
		      tally_total(keys, NULL) * 100.0 / tdb->map_size,
		      tally_total(data, NULL) * 100.0 / tdb->map_size,
		      tally_total(extra, NULL) * 100.0 / tdb->map_size,
		      tally_total(freet, NULL) * 100.0 / tdb->map_size,
		      (tally_num(keys) + tally_num(freet) + tally_num(hashes))
		      * sizeof(struct tdb_used_record) * 100.0 / tdb->map_size,
		      tally_num(ftables) * sizeof(struct tdb_freetable)
		      * 100.0 / tdb->map_size,
		      (tally_num(hashes)
		       * (sizeof(tdb_off_t) << TDB_SUBLEVEL_HASH_BITS)
		       + (sizeof(tdb_off_t) << TDB_TOPLEVEL_HASH_BITS)
		       + sizeof(struct tdb_chain) * tally_num(chains))
		      * 100.0 / tdb->map_size);

unlock:
	free(hashesg);
	free(freeg);
	free(keysg);
	free(datag);
	free(extrag);
	free(uncoalg);
	free(bucketsg);
	free(hashes);
	free(buckets);
	free(freet);
	free(keys);
	free(data);
	free(extra);
	free(uncoal);
	free(ftables);
	free(chains);

	tdb_allrecord_unlock(tdb, F_RDLCK);
	tdb_unlock_expand(tdb, F_RDLCK);
	return ret;
}
