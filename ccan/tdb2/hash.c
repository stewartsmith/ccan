 /* 
   Trivial Database 2: hash handling
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
#include <ccan/hash/hash.h>

static uint64_t jenkins_hash(const void *key, size_t length, uint64_t seed,
			     void *arg)
{
	uint64_t ret;
	/* hash64_stable assumes lower bits are more important; they are a
	 * slightly better hash.  We use the upper bits first, so swap them. */
	ret = hash64_stable((const unsigned char *)key, length, seed);
	return (ret >> 32) | (ret << 32);
}

void tdb_hash_init(struct tdb_context *tdb)
{
	tdb->khash = jenkins_hash;
	tdb->hash_priv = NULL;
}

uint64_t tdb_hash(struct tdb_context *tdb, const void *ptr, size_t len)
{
	return tdb->khash(ptr, len, tdb->hash_seed, tdb->hash_priv);
}

uint64_t hash_record(struct tdb_context *tdb, tdb_off_t off)
{
	const struct tdb_used_record *r;
	const void *key;
	uint64_t klen, hash;

	r = tdb_access_read(tdb, off, sizeof(*r), true);
	if (!r)
		/* FIXME */
		return 0;

	klen = rec_key_length(r);
	tdb_access_release(tdb, r);

	key = tdb_access_read(tdb, off + sizeof(*r), klen, false);
	if (!key)
		return 0;

	hash = tdb_hash(tdb, key, klen);
	tdb_access_release(tdb, key);
	return hash;
}

/* Get bits from a value. */
static uint32_t bits(uint64_t val, unsigned start, unsigned num)
{
	assert(num <= 32);
	return (val >> start) & ((1U << num) - 1);
}

/* We take bits from the top: that way we can lock whole sections of the hash
 * by using lock ranges. */
static uint32_t use_bits(struct hash_info *h, unsigned num)
{
	h->hash_used += num;
	return bits(h->h, 64 - h->hash_used, num);
}

static bool key_matches(struct tdb_context *tdb,
			const struct tdb_used_record *rec,
			tdb_off_t off,
			const struct tdb_data *key)
{
	bool ret = false;
	const char *rkey;

	if (rec_key_length(rec) != key->dsize) {
		add_stat(tdb, compare_wrong_keylen, 1);
		return ret;
	}

	rkey = tdb_access_read(tdb, off + sizeof(*rec), key->dsize, false);
	if (!rkey)
		return ret;
	if (memcmp(rkey, key->dptr, key->dsize) == 0)
		ret = true;
	else
		add_stat(tdb, compare_wrong_keycmp, 1);
	tdb_access_release(tdb, rkey);
	return ret;
}

/* Does entry match? */
static bool match(struct tdb_context *tdb,
		  struct hash_info *h,
		  const struct tdb_data *key,
		  tdb_off_t val,
		  struct tdb_used_record *rec)
{
	tdb_off_t off;

	add_stat(tdb, compares, 1);
	/* Desired bucket must match. */
	if (h->home_bucket != (val & TDB_OFF_HASH_GROUP_MASK)) {
		add_stat(tdb, compare_wrong_bucket, 1);
		return false;
	}

	/* Top bits of offset == next bits of hash. */
	if (bits(val, TDB_OFF_HASH_EXTRA_BIT, TDB_OFF_UPPER_STEAL_EXTRA)
	    != bits(h->h, 64 - h->hash_used - TDB_OFF_UPPER_STEAL_EXTRA,
		    TDB_OFF_UPPER_STEAL_EXTRA)) {
		add_stat(tdb, compare_wrong_offsetbits, 1);
		return false;
	}

	off = val & TDB_OFF_MASK;
	if (tdb_read_convert(tdb, off, rec, sizeof(*rec)) == -1)
		return false;

	if ((h->h & ((1 << 11)-1)) != rec_hash(rec)) {
		add_stat(tdb, compare_wrong_rechash, 1);
		return false;
	}

	return key_matches(tdb, rec, off, key);
}

static tdb_off_t hbucket_off(tdb_off_t group_start, unsigned bucket)
{
	return group_start
		+ (bucket % (1 << TDB_HASH_GROUP_BITS)) * sizeof(tdb_off_t);
}

bool is_subhash(tdb_off_t val)
{
	return (val >> TDB_OFF_UPPER_STEAL_SUBHASH_BIT) & 1;
}

/* FIXME: Guess the depth, don't over-lock! */
static tdb_off_t hlock_range(tdb_off_t group, tdb_off_t *size)
{
	*size = 1ULL << (64 - (TDB_TOPLEVEL_HASH_BITS - TDB_HASH_GROUP_BITS));
	return group << (64 - (TDB_TOPLEVEL_HASH_BITS - TDB_HASH_GROUP_BITS));
}

static tdb_off_t COLD find_in_chain(struct tdb_context *tdb,
				    struct tdb_data key,
				    tdb_off_t chain,
				    struct hash_info *h,
				    struct tdb_used_record *rec,
				    struct traverse_info *tinfo)
{
	tdb_off_t off, next;

	/* In case nothing is free, we set these to zero. */
	h->home_bucket = h->found_bucket = 0;

	for (off = chain; off; off = next) {
		unsigned int i;

		h->group_start = off;
		if (tdb_read_convert(tdb, off, h->group, sizeof(h->group)))
			return TDB_OFF_ERR;

		for (i = 0; i < (1 << TDB_HASH_GROUP_BITS); i++) {
			tdb_off_t recoff;
			if (!h->group[i]) {
				/* Remember this empty bucket. */
				h->home_bucket = h->found_bucket = i;
				continue;
			}

			/* We can insert extra bits via add_to_hash
			 * empty bucket logic. */
			recoff = h->group[i] & TDB_OFF_MASK;
			if (tdb_read_convert(tdb, recoff, rec, sizeof(*rec)))
				return TDB_OFF_ERR;

			if (key_matches(tdb, rec, recoff, &key)) {
				h->home_bucket = h->found_bucket = i;

				if (tinfo) {
					tinfo->levels[tinfo->num_levels]
						.hashtable = off;
					tinfo->levels[tinfo->num_levels]
						.total_buckets
						= 1 << TDB_HASH_GROUP_BITS;
					tinfo->levels[tinfo->num_levels].entry
						= i;
					tinfo->num_levels++;
				}
				return recoff;
			}
		}
		next = tdb_read_off(tdb, off
				    + offsetof(struct tdb_chain, next));
		if (next == TDB_OFF_ERR)
			return TDB_OFF_ERR;
		if (next)
			next += sizeof(struct tdb_used_record);
	}
	return 0;
}

/* This is the core routine which searches the hashtable for an entry.
 * On error, no locks are held and TDB_OFF_ERR is returned.
 * Otherwise, hinfo is filled in (and the optional tinfo).
 * If not found, the return value is 0.
 * If found, the return value is the offset, and *rec is the record. */
tdb_off_t find_and_lock(struct tdb_context *tdb,
			struct tdb_data key,
			int ltype,
			struct hash_info *h,
			struct tdb_used_record *rec,
			struct traverse_info *tinfo)
{
	uint32_t i, group;
	tdb_off_t hashtable;

	h->h = tdb_hash(tdb, key.dptr, key.dsize);
	h->hash_used = 0;
	group = use_bits(h, TDB_TOPLEVEL_HASH_BITS - TDB_HASH_GROUP_BITS);
	h->home_bucket = use_bits(h, TDB_HASH_GROUP_BITS);

	h->hlock_start = hlock_range(group, &h->hlock_range);
	if (tdb_lock_hashes(tdb, h->hlock_start, h->hlock_range, ltype,
			    TDB_LOCK_WAIT))
		return TDB_OFF_ERR;

	hashtable = offsetof(struct tdb_header, hashtable);
	if (tinfo) {
		tinfo->toplevel_group = group;
		tinfo->num_levels = 1;
		tinfo->levels[0].entry = 0;
		tinfo->levels[0].hashtable = hashtable 
			+ (group << TDB_HASH_GROUP_BITS) * sizeof(tdb_off_t);
		tinfo->levels[0].total_buckets = 1 << TDB_HASH_GROUP_BITS;
	}

	while (h->hash_used <= 64) {
		/* Read in the hash group. */
		h->group_start = hashtable
			+ group * (sizeof(tdb_off_t) << TDB_HASH_GROUP_BITS);

		if (tdb_read_convert(tdb, h->group_start, &h->group,
				     sizeof(h->group)) == -1)
			goto fail;

		/* Pointer to another hash table?  Go down... */
		if (is_subhash(h->group[h->home_bucket])) {
			hashtable = (h->group[h->home_bucket] & TDB_OFF_MASK)
				+ sizeof(struct tdb_used_record);
			if (tinfo) {
				/* When we come back, use *next* bucket */
				tinfo->levels[tinfo->num_levels-1].entry
					+= h->home_bucket + 1;
			}
			group = use_bits(h, TDB_SUBLEVEL_HASH_BITS
					 - TDB_HASH_GROUP_BITS);
			h->home_bucket = use_bits(h, TDB_HASH_GROUP_BITS);
			if (tinfo) {
				tinfo->levels[tinfo->num_levels].hashtable
					= hashtable;
				tinfo->levels[tinfo->num_levels].total_buckets
					= 1 << TDB_SUBLEVEL_HASH_BITS;
				tinfo->levels[tinfo->num_levels].entry
					= group << TDB_HASH_GROUP_BITS;
				tinfo->num_levels++;
			}
			continue;
		}

		/* It's in this group: search (until 0 or all searched) */
		for (i = 0, h->found_bucket = h->home_bucket;
		     i < (1 << TDB_HASH_GROUP_BITS);
		     i++, h->found_bucket = ((h->found_bucket+1)
					     % (1 << TDB_HASH_GROUP_BITS))) {
			if (is_subhash(h->group[h->found_bucket]))
				continue;

			if (!h->group[h->found_bucket])
				break;

			if (match(tdb, h, &key, h->group[h->found_bucket],
				  rec)) {
				if (tinfo) {
					tinfo->levels[tinfo->num_levels-1].entry
						+= h->found_bucket;
				}
				return h->group[h->found_bucket] & TDB_OFF_MASK;
			}
		}
		/* Didn't find it: h indicates where it would go. */
		return 0;
	}

	return find_in_chain(tdb, key, hashtable, h, rec, tinfo);

fail:
	tdb_unlock_hashes(tdb, h->hlock_start, h->hlock_range, ltype);
	return TDB_OFF_ERR;
}

/* I wrote a simple test, expanding a hash to 2GB, for the following
 * cases:
 * 1) Expanding all the buckets at once,
 * 2) Expanding the bucket we wanted to place the new entry into.
 * 3) Expanding the most-populated bucket,
 *
 * I measured the worst/average/best density during this process.
 * 1) 3%/16%/30%
 * 2) 4%/20%/38%
 * 3) 6%/22%/41%
 *
 * So we figure out the busiest bucket for the moment.
 */
static unsigned fullest_bucket(struct tdb_context *tdb,
			       const tdb_off_t *group,
			       unsigned new_bucket)
{
	unsigned counts[1 << TDB_HASH_GROUP_BITS] = { 0 };
	unsigned int i, best_bucket;

	/* Count the new entry. */
	counts[new_bucket]++;
	best_bucket = new_bucket;

	for (i = 0; i < (1 << TDB_HASH_GROUP_BITS); i++) {
		unsigned this_bucket;

		if (is_subhash(group[i]))
			continue;
		this_bucket = group[i] & TDB_OFF_HASH_GROUP_MASK;
		if (++counts[this_bucket] > counts[best_bucket])
			best_bucket = this_bucket;
	}

	return best_bucket;
}

static bool put_into_group(tdb_off_t *group,
			   unsigned bucket, tdb_off_t encoded)
{
	unsigned int i;

	for (i = 0; i < (1 << TDB_HASH_GROUP_BITS); i++) {
		unsigned b = (bucket + i) % (1 << TDB_HASH_GROUP_BITS);

		if (group[b] == 0) {
			group[b] = encoded;
			return true;
		}
	}
	return false;
}

static void force_into_group(tdb_off_t *group,
			     unsigned bucket, tdb_off_t encoded)
{
	if (!put_into_group(group, bucket, encoded))
		abort();
}

static tdb_off_t encode_offset(tdb_off_t new_off, struct hash_info *h)
{
	return h->home_bucket
		| new_off
		| ((uint64_t)bits(h->h,
				  64 - h->hash_used - TDB_OFF_UPPER_STEAL_EXTRA,
				  TDB_OFF_UPPER_STEAL_EXTRA)
		   << TDB_OFF_HASH_EXTRA_BIT);
}

/* Simply overwrite the hash entry we found before. */ 
int replace_in_hash(struct tdb_context *tdb,
		    struct hash_info *h,
		    tdb_off_t new_off)
{
	return tdb_write_off(tdb, hbucket_off(h->group_start, h->found_bucket),
			     encode_offset(new_off, h));
}

/* We slot in anywhere that's empty in the chain. */
static int COLD add_to_chain(struct tdb_context *tdb,
			     tdb_off_t subhash,
			     tdb_off_t new_off)
{
	size_t entry = tdb_find_zero_off(tdb, subhash, 1<<TDB_HASH_GROUP_BITS);

	if (entry == 1 << TDB_HASH_GROUP_BITS) {
		tdb_off_t next;

		next = tdb_read_off(tdb, subhash
				    + offsetof(struct tdb_chain, next));
		if (next == TDB_OFF_ERR)
			return -1;

		if (!next) {
			next = alloc(tdb, 0, sizeof(struct tdb_chain), 0,
				     TDB_CHAIN_MAGIC, false);
			if (next == TDB_OFF_ERR)
				return -1;
			if (zero_out(tdb, next+sizeof(struct tdb_used_record),
				     sizeof(struct tdb_chain)))
				return -1;
			if (tdb_write_off(tdb, subhash
					  + offsetof(struct tdb_chain, next),
					  next) != 0)
				return -1;
		}
		return add_to_chain(tdb, next, new_off);
	}

	return tdb_write_off(tdb, subhash + entry * sizeof(tdb_off_t),
			     new_off);
}

/* Add into a newly created subhash. */
static int add_to_subhash(struct tdb_context *tdb, tdb_off_t subhash,
			  unsigned hash_used, tdb_off_t val)
{
	tdb_off_t off = (val & TDB_OFF_MASK), *group;
	struct hash_info h;
	unsigned int gnum;

	h.hash_used = hash_used;

	if (hash_used + TDB_SUBLEVEL_HASH_BITS > 64)
		return add_to_chain(tdb, subhash, off);

	h.h = hash_record(tdb, off);
	gnum = use_bits(&h, TDB_SUBLEVEL_HASH_BITS-TDB_HASH_GROUP_BITS);
	h.group_start = subhash
		+ gnum * (sizeof(tdb_off_t) << TDB_HASH_GROUP_BITS);
	h.home_bucket = use_bits(&h, TDB_HASH_GROUP_BITS);

	group = tdb_access_write(tdb, h.group_start,
				 sizeof(*group) << TDB_HASH_GROUP_BITS, true);
	if (!group)
		return -1;
	force_into_group(group, h.home_bucket, encode_offset(off, &h));
	return tdb_access_commit(tdb, group);
}

static int expand_group(struct tdb_context *tdb, struct hash_info *h)
{
	unsigned bucket, num_vals, i, magic;
	size_t subsize;
	tdb_off_t subhash;
	tdb_off_t vals[1 << TDB_HASH_GROUP_BITS];

	/* Attach new empty subhash under fullest bucket. */
	bucket = fullest_bucket(tdb, h->group, h->home_bucket);

	if (h->hash_used == 64) {
		add_stat(tdb, alloc_chain, 1);
		subsize = sizeof(struct tdb_chain);
		magic = TDB_CHAIN_MAGIC;
	} else {
		add_stat(tdb, alloc_subhash, 1);
		subsize = (sizeof(tdb_off_t) << TDB_SUBLEVEL_HASH_BITS);
		magic = TDB_HTABLE_MAGIC;
	}

	subhash = alloc(tdb, 0, subsize, 0, magic, false);
	if (subhash == TDB_OFF_ERR)
		return -1;

	if (zero_out(tdb, subhash + sizeof(struct tdb_used_record), subsize))
		return -1;

	/* Remove any which are destined for bucket or are in wrong place. */
	num_vals = 0;
	for (i = 0; i < (1 << TDB_HASH_GROUP_BITS); i++) {
		unsigned home_bucket = h->group[i] & TDB_OFF_HASH_GROUP_MASK;
		if (!h->group[i] || is_subhash(h->group[i]))
			continue;
		if (home_bucket == bucket || home_bucket != i) {
			vals[num_vals++] = h->group[i];
			h->group[i] = 0;
		}
	}
	/* FIXME: This assert is valid, but we do this during unit test :( */
	/* assert(num_vals); */

	/* Overwrite expanded bucket with subhash pointer. */
	h->group[bucket] = subhash | (1ULL << TDB_OFF_UPPER_STEAL_SUBHASH_BIT);

	/* Point to actual contents of record. */
	subhash += sizeof(struct tdb_used_record);

	/* Put values back. */
	for (i = 0; i < num_vals; i++) {
		unsigned this_bucket = vals[i] & TDB_OFF_HASH_GROUP_MASK;

		if (this_bucket == bucket) {
			if (add_to_subhash(tdb, subhash, h->hash_used, vals[i]))
				return -1;
		} else {
			/* There should be room to put this back. */
			force_into_group(h->group, this_bucket, vals[i]);
		}
	}
	return 0;
}

int delete_from_hash(struct tdb_context *tdb, struct hash_info *h)
{
	unsigned int i, num_movers = 0;
	tdb_off_t movers[1 << TDB_HASH_GROUP_BITS];

	h->group[h->found_bucket] = 0;
	for (i = 1; i < (1 << TDB_HASH_GROUP_BITS); i++) {
		unsigned this_bucket;

		this_bucket = (h->found_bucket+i) % (1 << TDB_HASH_GROUP_BITS);
		/* Empty bucket?  We're done. */
		if (!h->group[this_bucket])
			break;

		/* Ignore subhashes. */
		if (is_subhash(h->group[this_bucket]))
			continue;

		/* If this one is not happy where it is, we'll move it. */
		if ((h->group[this_bucket] & TDB_OFF_HASH_GROUP_MASK)
		    != this_bucket) {
			movers[num_movers++] = h->group[this_bucket];
			h->group[this_bucket] = 0;
		}
	}

	/* Put back the ones we erased. */
	for (i = 0; i < num_movers; i++) {
		force_into_group(h->group, movers[i] & TDB_OFF_HASH_GROUP_MASK,
				 movers[i]);
	}

	/* Now we write back the hash group */
	return tdb_write_convert(tdb, h->group_start,
				 h->group, sizeof(h->group));
}

int add_to_hash(struct tdb_context *tdb, struct hash_info *h, tdb_off_t new_off)
{
	/* We hit an empty bucket during search?  That's where it goes. */
	if (!h->group[h->found_bucket]) {
		h->group[h->found_bucket] = encode_offset(new_off, h);
		/* Write back the modified group. */
		return tdb_write_convert(tdb, h->group_start,
					 h->group, sizeof(h->group));
	}

	if (h->hash_used > 64)
		return add_to_chain(tdb, h->group_start, new_off);

	/* We're full.  Expand. */
	if (expand_group(tdb, h) == -1)
		return -1;

	if (is_subhash(h->group[h->home_bucket])) {
		/* We were expanded! */
		tdb_off_t hashtable;
		unsigned int gnum;

		/* Write back the modified group. */
		if (tdb_write_convert(tdb, h->group_start, h->group,
				      sizeof(h->group)))
			return -1;

		/* Move hashinfo down a level. */
		hashtable = (h->group[h->home_bucket] & TDB_OFF_MASK)
			+ sizeof(struct tdb_used_record);
		gnum = use_bits(h,TDB_SUBLEVEL_HASH_BITS - TDB_HASH_GROUP_BITS);
		h->home_bucket = use_bits(h, TDB_HASH_GROUP_BITS);
		h->group_start = hashtable
			+ gnum * (sizeof(tdb_off_t) << TDB_HASH_GROUP_BITS);
		if (tdb_read_convert(tdb, h->group_start, &h->group,
				     sizeof(h->group)) == -1)
			return -1;
	}

	/* Expanding the group must have made room if it didn't choose this
	 * bucket. */
	if (put_into_group(h->group, h->home_bucket, encode_offset(new_off, h)))
		return tdb_write_convert(tdb, h->group_start,
					 h->group, sizeof(h->group));

	/* This can happen if all hashes in group (and us) dropped into same
	 * group in subhash. */
	return add_to_hash(tdb, h, new_off);
}

/* Traverse support: returns offset of record, or 0 or TDB_OFF_ERR. */
static tdb_off_t iterate_hash(struct tdb_context *tdb,
			      struct traverse_info *tinfo)
{
	tdb_off_t off, val;
	unsigned int i;
	struct traverse_level *tlevel;

	tlevel = &tinfo->levels[tinfo->num_levels-1];

again:
	for (i = tdb_find_nonzero_off(tdb, tlevel->hashtable,
				      tlevel->entry, tlevel->total_buckets);
	     i != tlevel->total_buckets;
	     i = tdb_find_nonzero_off(tdb, tlevel->hashtable,
				      i+1, tlevel->total_buckets)) {
		val = tdb_read_off(tdb, tlevel->hashtable+sizeof(tdb_off_t)*i);
		if (unlikely(val == TDB_OFF_ERR))
			return TDB_OFF_ERR;

		off = val & TDB_OFF_MASK;

		/* This makes the delete-all-in-traverse case work
		 * (and simplifies our logic a little). */
		if (off == tinfo->prev)
			continue;

		tlevel->entry = i;

		if (!is_subhash(val)) {
			/* Found one. */
			tinfo->prev = off;
			return off;
		}

		/* When we come back, we want the next one */
		tlevel->entry++;
		tinfo->num_levels++;
		tlevel++;
		tlevel->hashtable = off + sizeof(struct tdb_used_record);
		tlevel->entry = 0;
		/* Next level is a chain? */
		if (unlikely(tinfo->num_levels == TDB_MAX_LEVELS + 1))
			tlevel->total_buckets = (1 << TDB_HASH_GROUP_BITS);
		else
			tlevel->total_buckets = (1 << TDB_SUBLEVEL_HASH_BITS);
		goto again;
	}

	/* Nothing there? */
	if (tinfo->num_levels == 1)
		return 0;

	/* Handle chained entries. */
	if (unlikely(tinfo->num_levels == TDB_MAX_LEVELS + 1)) {
		tlevel->hashtable = tdb_read_off(tdb, tlevel->hashtable
						 + offsetof(struct tdb_chain,
							    next));
		if (tlevel->hashtable == TDB_OFF_ERR)
			return TDB_OFF_ERR;
		if (tlevel->hashtable) {
			tlevel->hashtable += sizeof(struct tdb_used_record);
			tlevel->entry = 0;
			goto again;
		}
	}

	/* Go back up and keep searching. */
	tinfo->num_levels--;
	tlevel--;
	goto again;
}

/* Return 1 if we find something, 0 if not, -1 on error. */
int next_in_hash(struct tdb_context *tdb, int ltype,
		 struct traverse_info *tinfo,
		 TDB_DATA *kbuf, size_t *dlen)
{
	const unsigned group_bits = TDB_TOPLEVEL_HASH_BITS-TDB_HASH_GROUP_BITS;
	tdb_off_t hlock_start, hlock_range, off;

	while (tinfo->toplevel_group < (1 << group_bits)) {
		hlock_start = (tdb_off_t)tinfo->toplevel_group
			<< (64 - group_bits);
		hlock_range = 1ULL << group_bits;
		if (tdb_lock_hashes(tdb, hlock_start, hlock_range, ltype,
				    TDB_LOCK_WAIT) != 0)
			return -1;

		off = iterate_hash(tdb, tinfo);
		if (off) {
			struct tdb_used_record rec;

			if (tdb_read_convert(tdb, off, &rec, sizeof(rec))) {
				tdb_unlock_hashes(tdb,
						  hlock_start, hlock_range,
						  ltype);
				return -1;
			}
			if (rec_magic(&rec) != TDB_USED_MAGIC) {
				tdb_logerr(tdb, TDB_ERR_CORRUPT,
					   TDB_DEBUG_FATAL,
					   "next_in_hash:"
					   " corrupt record at %llu",
					   (long long)off);
				return -1;
			}

			kbuf->dsize = rec_key_length(&rec);

			/* They want data as well? */
			if (dlen) {
				*dlen = rec_data_length(&rec);
				kbuf->dptr = tdb_alloc_read(tdb, 
							    off + sizeof(rec),
							    kbuf->dsize
							    + *dlen);
			} else {
				kbuf->dptr = tdb_alloc_read(tdb, 
							    off + sizeof(rec),
							    kbuf->dsize);
			}
			tdb_unlock_hashes(tdb, hlock_start, hlock_range, ltype);
			return kbuf->dptr ? 1 : -1;
		}

		tdb_unlock_hashes(tdb, hlock_start, hlock_range, ltype);

		tinfo->toplevel_group++;
		tinfo->levels[0].hashtable
			+= (sizeof(tdb_off_t) << TDB_HASH_GROUP_BITS);
		tinfo->levels[0].entry = 0;
	}
	return 0;
}

/* Return 1 if we find something, 0 if not, -1 on error. */
int first_in_hash(struct tdb_context *tdb, int ltype,
		  struct traverse_info *tinfo,
		  TDB_DATA *kbuf, size_t *dlen)
{
	tinfo->prev = 0;
	tinfo->toplevel_group = 0;
	tinfo->num_levels = 1;
	tinfo->levels[0].hashtable = offsetof(struct tdb_header, hashtable);
	tinfo->levels[0].entry = 0;
	tinfo->levels[0].total_buckets = (1 << TDB_HASH_GROUP_BITS);

	return next_in_hash(tdb, ltype, tinfo, kbuf, dlen);
}

/* Even if the entry isn't in this hash bucket, you'd have to lock this
 * bucket to find it. */
static int chainlock(struct tdb_context *tdb, const TDB_DATA *key,
		     int ltype, enum tdb_lock_flags waitflag,
		     const char *func)
{
	int ret;
	uint64_t h = tdb_hash(tdb, key->dptr, key->dsize);
	tdb_off_t lockstart, locksize;
	unsigned int group, gbits;

	gbits = TDB_TOPLEVEL_HASH_BITS - TDB_HASH_GROUP_BITS;
	group = bits(h, 64 - gbits, gbits);

	lockstart = hlock_range(group, &locksize);

	ret = tdb_lock_hashes(tdb, lockstart, locksize, ltype, waitflag);
	tdb_trace_1rec(tdb, func, *key);
	return ret;
}

/* lock/unlock one hash chain. This is meant to be used to reduce
   contention - it cannot guarantee how many records will be locked */
int tdb_chainlock(struct tdb_context *tdb, TDB_DATA key)
{
	return chainlock(tdb, &key, F_WRLCK, TDB_LOCK_WAIT, "tdb_chainlock");
}

int tdb_chainunlock(struct tdb_context *tdb, TDB_DATA key)
{
	uint64_t h = tdb_hash(tdb, key.dptr, key.dsize);
	tdb_off_t lockstart, locksize;
	unsigned int group, gbits;

	gbits = TDB_TOPLEVEL_HASH_BITS - TDB_HASH_GROUP_BITS;
	group = bits(h, 64 - gbits, gbits);

	lockstart = hlock_range(group, &locksize);

	tdb_trace_1rec(tdb, "tdb_chainunlock", key);
	return tdb_unlock_hashes(tdb, lockstart, locksize, F_WRLCK);
}
