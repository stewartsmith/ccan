 /* 
   Unix SMB/CIFS implementation.

   trivial database library

   Copyright (C) Andrew Tridgell              1999-2005
   Copyright (C) Paul `Rusty' Russell		   2000
   Copyright (C) Jeremy Allison			   2000-2003

     ** NOTE! The following LGPL license applies to the tdb
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

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
#include <ccan/build_assert/build_assert.h>

static int fcntl_lock(struct tdb_context *tdb,
		      int rw, off_t off, off_t len, bool waitflag)
{
	struct flock fl;

	fl.l_type = rw;
	fl.l_whence = SEEK_SET;
	fl.l_start = off;
	fl.l_len = len;
	fl.l_pid = 0;

	add_stat(tdb, lock_lowlevel, 1);
	if (waitflag)
		return fcntl(tdb->fd, F_SETLKW, &fl);
	else {
		add_stat(tdb, lock_nonblock, 1);
		return fcntl(tdb->fd, F_SETLK, &fl);
	}
}

static int fcntl_unlock(struct tdb_context *tdb, int rw, off_t off, off_t len)
{
	struct flock fl;
#if 0 /* Check they matched up locks and unlocks correctly. */
	char line[80];
	FILE *locks;
	bool found = false;

	locks = fopen("/proc/locks", "r");

	while (fgets(line, 80, locks)) {
		char *p;
		int type, start, l;

		/* eg. 1: FLOCK  ADVISORY  WRITE 2440 08:01:2180826 0 EOF */
		p = strchr(line, ':') + 1;
		if (strncmp(p, " POSIX  ADVISORY  ", strlen(" POSIX  ADVISORY  ")))
			continue;
		p += strlen(" FLOCK  ADVISORY  ");
		if (strncmp(p, "READ  ", strlen("READ  ")) == 0)
			type = F_RDLCK;
		else if (strncmp(p, "WRITE ", strlen("WRITE ")) == 0)
			type = F_WRLCK;
		else
			abort();
		p += 6;
		if (atoi(p) != getpid())
			continue;
		p = strchr(strchr(p, ' ') + 1, ' ') + 1;
		start = atoi(p);
		p = strchr(p, ' ') + 1;
		if (strncmp(p, "EOF", 3) == 0)
			l = 0;
		else
			l = atoi(p) - start + 1;

		if (off == start) {
			if (len != l) {
				fprintf(stderr, "Len %u should be %u: %s",
					(int)len, l, line);
				abort();
			}
			if (type != rw) {
				fprintf(stderr, "Type %s wrong: %s",
					rw == F_RDLCK ? "READ" : "WRITE", line);
				abort();
			}
			found = true;
			break;
		}
	}

	if (!found) {
		fprintf(stderr, "Unlock on %u@%u not found!",
			(int)off, (int)len);
		abort();
	}

	fclose(locks);
#endif

	fl.l_type = F_UNLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = off;
	fl.l_len = len;
	fl.l_pid = 0;

	return fcntl(tdb->fd, F_SETLKW, &fl);
}

/* a byte range locking function - return 0 on success
   this functions locks/unlocks 1 byte at the specified offset.

   note that a len of zero means lock to end of file
*/
static int tdb_brlock(struct tdb_context *tdb,
		      int rw_type, tdb_off_t offset, tdb_off_t len,
		      enum tdb_lock_flags flags)
{
	int ret;

	if (tdb->flags & TDB_NOLOCK) {
		return 0;
	}

	if (rw_type == F_WRLCK && tdb->read_only) {
		tdb_logerr(tdb, TDB_ERR_RDONLY, TDB_DEBUG_WARNING,
			   "Write lock attempted on read-only database");
		return -1;
	}

	/* A 32 bit system cannot open a 64-bit file, but it could have
	 * expanded since then: check here. */
	if ((size_t)(offset + len) != offset + len) {
		tdb_logerr(tdb, TDB_ERR_IO, TDB_DEBUG_ERROR,
			 "tdb_brlock: lock on giant offset %llu",
			 (long long)(offset + len));
		return -1;
	}

	do {
		ret = fcntl_lock(tdb, rw_type, offset, len,
				 flags & TDB_LOCK_WAIT);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1) {
		tdb->ecode = TDB_ERR_LOCK;
		/* Generic lock error. errno set by fcntl.
		 * EAGAIN is an expected return from non-blocking
		 * locks. */
		if (!(flags & TDB_LOCK_PROBE) && errno != EAGAIN) {
			tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
				   "tdb_brlock failed (fd=%d) at"
				   " offset %zu rw_type=%d flags=%d len=%zu:"
				   " %s",
				   tdb->fd, (size_t)offset, rw_type,
				   flags, (size_t)len, strerror(errno));
		}
		return -1;
	}
	return 0;
}

static int tdb_brunlock(struct tdb_context *tdb,
			int rw_type, tdb_off_t offset, size_t len)
{
	int ret;

	if (tdb->flags & TDB_NOLOCK) {
		return 0;
	}

	do {
		ret = fcntl_unlock(tdb, rw_type, offset, len);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_TRACE,
			   "tdb_brunlock failed (fd=%d) at offset %zu"
			   " rw_type=%d len=%zu",
			   tdb->fd, (size_t)offset, rw_type, (size_t)len);
	}
	return ret;
}

/*
  upgrade a read lock to a write lock. This needs to be handled in a
  special way as some OSes (such as solaris) have too conservative
  deadlock detection and claim a deadlock when progress can be
  made. For those OSes we may loop for a while.  
*/
int tdb_allrecord_upgrade(struct tdb_context *tdb)
{
	int count = 1000;

	if (tdb->allrecord_lock.count != 1) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_allrecord_upgrade failed: count %u too high",
			   tdb->allrecord_lock.count);
		return -1;
	}

	if (tdb->allrecord_lock.off != 1) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_allrecord_upgrade failed: already upgraded?");
		return -1;
	}

	while (count--) {
		struct timeval tv;
		if (tdb_brlock(tdb, F_WRLCK,
			       TDB_HASH_LOCK_START, 0,
			       TDB_LOCK_WAIT|TDB_LOCK_PROBE) == 0) {
			tdb->allrecord_lock.ltype = F_WRLCK;
			tdb->allrecord_lock.off = 0;
			return 0;
		}
		if (errno != EDEADLK) {
			break;
		}
		/* sleep for as short a time as we can - more portable than usleep() */
		tv.tv_sec = 0;
		tv.tv_usec = 1;
		select(0, NULL, NULL, NULL, &tv);
	}
	tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_WARNING,
		   "tdb_allrecord_upgrade failed");
	return -1;
}

static struct tdb_lock_type *find_nestlock(struct tdb_context *tdb,
					   tdb_off_t offset)
{
	unsigned int i;

	for (i=0; i<tdb->num_lockrecs; i++) {
		if (tdb->lockrecs[i].off == offset) {
			return &tdb->lockrecs[i];
		}
	}
	return NULL;
}

int tdb_lock_and_recover(struct tdb_context *tdb)
{
	int ret;

	if (tdb_allrecord_lock(tdb, F_WRLCK, TDB_LOCK_WAIT|TDB_LOCK_NOCHECK,
			       false) == -1) {
		return -1;
	}

	if (tdb_lock_open(tdb, TDB_LOCK_WAIT|TDB_LOCK_NOCHECK) == -1) {
		tdb_allrecord_unlock(tdb, F_WRLCK);
		return -1;
	}
	ret = tdb_transaction_recover(tdb);

	tdb_unlock_open(tdb);
	tdb_allrecord_unlock(tdb, F_WRLCK);

	return ret;
}

/* lock an offset in the database. */
static int tdb_nest_lock(struct tdb_context *tdb, tdb_off_t offset, int ltype,
			 enum tdb_lock_flags flags)
{
	struct tdb_lock_type *new_lck;

	if (offset > TDB_HASH_LOCK_START + TDB_HASH_LOCK_RANGE + tdb->map_size / 8) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_FATAL,
			   "tdb_nest_lock: invalid offset %zu ltype=%d",
			   (size_t)offset, ltype);
		return -1;
	}

	if (tdb->flags & TDB_NOLOCK)
		return 0;

	add_stat(tdb, locks, 1);

	new_lck = find_nestlock(tdb, offset);
	if (new_lck) {
		if (new_lck->ltype == F_RDLCK && ltype == F_WRLCK) {
			tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_FATAL,
				   "tdb_nest_lock: offset %zu has read lock",
				   (size_t)offset);
			return -1;
		}
		/* Just increment the struct, posix locks don't stack. */
		new_lck->count++;
		return 0;
	}

	if (tdb->num_lockrecs
	    && offset >= TDB_HASH_LOCK_START
	    && offset < TDB_HASH_LOCK_START + TDB_HASH_LOCK_RANGE) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_FATAL,
			   "tdb_nest_lock: already have a hash lock?");
		return -1;
	}

	new_lck = (struct tdb_lock_type *)realloc(
		tdb->lockrecs,
		sizeof(*tdb->lockrecs) * (tdb->num_lockrecs+1));
	if (new_lck == NULL) {
		tdb_logerr(tdb, TDB_ERR_OOM, TDB_DEBUG_ERROR,
			 "tdb_nest_lock: unable to allocate %zu lock struct",
			 tdb->num_lockrecs + 1);
		errno = ENOMEM;
		return -1;
	}
	tdb->lockrecs = new_lck;

	/* Since fcntl locks don't nest, we do a lock for the first one,
	   and simply bump the count for future ones */
	if (tdb_brlock(tdb, ltype, offset, 1, flags)) {
		return -1;
	}

	/* First time we grab a lock, perhaps someone died in commit? */
	if (!(flags & TDB_LOCK_NOCHECK)
	    && tdb->num_lockrecs == 0
	    && unlikely(tdb_needs_recovery(tdb))) {
		tdb_brunlock(tdb, ltype, offset, 1);

		if (tdb_lock_and_recover(tdb) == -1) {
			return -1;
		}

		if (tdb_brlock(tdb, ltype, offset, 1, flags)) {
			return -1;
		}
	}

	tdb->lockrecs[tdb->num_lockrecs].off = offset;
	tdb->lockrecs[tdb->num_lockrecs].count = 1;
	tdb->lockrecs[tdb->num_lockrecs].ltype = ltype;
	tdb->num_lockrecs++;

	return 0;
}

static int tdb_nest_unlock(struct tdb_context *tdb, tdb_off_t off, int ltype)
{
	int ret = -1;
	struct tdb_lock_type *lck;

	if (tdb->flags & TDB_NOLOCK)
		return 0;

	lck = find_nestlock(tdb, off);
	if ((lck == NULL) || (lck->count == 0)) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_nest_unlock: no lock for %zu", (size_t)off);
		return -1;
	}

	if (lck->count > 1) {
		lck->count--;
		return 0;
	}

	/*
	 * This lock has count==1 left, so we need to unlock it in the
	 * kernel. We don't bother with decrementing the in-memory array
	 * element, we're about to overwrite it with the last array element
	 * anyway.
	 */
	ret = tdb_brunlock(tdb, ltype, off, 1);

	/*
	 * Shrink the array by overwriting the element just unlocked with the
	 * last array element.
	 */
	*lck = tdb->lockrecs[--tdb->num_lockrecs];

	return ret;
}

/*
  get the transaction lock
 */
int tdb_transaction_lock(struct tdb_context *tdb, int ltype)
{
	return tdb_nest_lock(tdb, TDB_TRANSACTION_LOCK, ltype, TDB_LOCK_WAIT);
}

/*
  release the transaction lock
 */
int tdb_transaction_unlock(struct tdb_context *tdb, int ltype)
{
	return tdb_nest_unlock(tdb, TDB_TRANSACTION_LOCK, ltype);
}

/* We only need to lock individual bytes, but Linux merges consecutive locks
 * so we lock in contiguous ranges. */
static int tdb_lock_gradual(struct tdb_context *tdb,
			    int ltype, enum tdb_lock_flags flags,
			    tdb_off_t off, tdb_off_t len)
{
	int ret;
	enum tdb_lock_flags nb_flags = (flags & ~TDB_LOCK_WAIT);

	if (len <= 1) {
		/* 0 would mean to end-of-file... */
		assert(len != 0);
		/* Single hash.  Just do blocking lock. */
		return tdb_brlock(tdb, ltype, off, len, flags);
	}

	/* First we try non-blocking. */
	ret = tdb_brlock(tdb, ltype, off, len, nb_flags);
	if (ret == 0) {
		return 0;
	}

	/* Try locking first half, then second. */
	ret = tdb_lock_gradual(tdb, ltype, flags, off, len / 2);
	if (ret == -1)
		return -1;

	ret = tdb_lock_gradual(tdb, ltype, flags,
				    off + len / 2, len - len / 2);
	if (ret == -1) {
		tdb_brunlock(tdb, ltype, off, len / 2);
		return -1;
	}
	return 0;
}

/* lock/unlock entire database.  It can only be upgradable if you have some
 * other way of guaranteeing exclusivity (ie. transaction write lock). */
int tdb_allrecord_lock(struct tdb_context *tdb, int ltype,
		       enum tdb_lock_flags flags, bool upgradable)
{
	/* FIXME: There are no locks on read-only dbs */
	if (tdb->read_only) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_allrecord_lock: read-only");
		return -1;
	}

	if (tdb->allrecord_lock.count
	    && (ltype == F_RDLCK || tdb->allrecord_lock.ltype == F_WRLCK)) {
		tdb->allrecord_lock.count++;
		return 0;
	}

	if (tdb->allrecord_lock.count) {
		/* a global lock of a different type exists */
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_allrecord_lock: already have %s lock",
			   tdb->allrecord_lock.ltype == F_RDLCK
			   ? "read" : "write");
		return -1;
	}

	if (tdb_has_hash_locks(tdb)) {
		/* can't combine global and chain locks */
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			 "tdb_allrecord_lock: already have chain lock");
		return -1;
	}

	if (upgradable && ltype != F_RDLCK) {
		/* tdb error: you can't upgrade a write lock! */
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_allrecord_lock: can't upgrade a write lock");
		return -1;
	}

	add_stat(tdb, locks, 1);
again:
	/* Lock hashes, gradually. */
	if (tdb_lock_gradual(tdb, ltype, flags, TDB_HASH_LOCK_START,
			     TDB_HASH_LOCK_RANGE)) {
		if (!(flags & TDB_LOCK_PROBE)) {
			tdb_logerr(tdb, tdb->ecode, TDB_DEBUG_ERROR,
				   "tdb_allrecord_lock hashes failed");
		}
		return -1;
	}

	/* Lock free tables: there to end of file. */
	if (tdb_brlock(tdb, ltype, TDB_HASH_LOCK_START + TDB_HASH_LOCK_RANGE,
		       0, flags)) {
		if (!(flags & TDB_LOCK_PROBE)) {
			tdb_logerr(tdb, tdb->ecode, TDB_DEBUG_ERROR,
				 "tdb_allrecord_lock freetables failed");
		}
		tdb_brunlock(tdb, ltype, TDB_HASH_LOCK_START, 
			     TDB_HASH_LOCK_RANGE);
		return -1;
	}

	tdb->allrecord_lock.count = 1;
	/* If it's upgradable, it's actually exclusive so we can treat
	 * it as a write lock. */
	tdb->allrecord_lock.ltype = upgradable ? F_WRLCK : ltype;
	tdb->allrecord_lock.off = upgradable;

	/* Now check for needing recovery. */
	if (!(flags & TDB_LOCK_NOCHECK) && unlikely(tdb_needs_recovery(tdb))) {
		tdb_allrecord_unlock(tdb, ltype);
		if (tdb_lock_and_recover(tdb) == -1) {
			return -1;
		}		
		goto again;
	}

	return 0;
}

int tdb_lock_open(struct tdb_context *tdb, enum tdb_lock_flags flags)
{
	return tdb_nest_lock(tdb, TDB_OPEN_LOCK, F_WRLCK, flags);
}

void tdb_unlock_open(struct tdb_context *tdb)
{
	tdb_nest_unlock(tdb, TDB_OPEN_LOCK, F_WRLCK);
}

bool tdb_has_open_lock(struct tdb_context *tdb)
{
	return find_nestlock(tdb, TDB_OPEN_LOCK) != NULL;
}

int tdb_lock_expand(struct tdb_context *tdb, int ltype)
{
	/* Lock doesn't protect data, so don't check (we recurse if we do!) */
	return tdb_nest_lock(tdb, TDB_EXPANSION_LOCK, ltype,
			     TDB_LOCK_WAIT | TDB_LOCK_NOCHECK);
}

void tdb_unlock_expand(struct tdb_context *tdb, int ltype)
{
	tdb_nest_unlock(tdb, TDB_EXPANSION_LOCK, ltype);
}

/* unlock entire db */
int tdb_allrecord_unlock(struct tdb_context *tdb, int ltype)
{
	if (tdb->allrecord_lock.count == 0) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_allrecord_unlock: not locked!");
		return -1;
	}

	/* Upgradable locks are marked as write locks. */
	if (tdb->allrecord_lock.ltype != ltype
	    && (!tdb->allrecord_lock.off || ltype != F_RDLCK)) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			 "tdb_allrecord_unlock: have %s lock",
			   tdb->allrecord_lock.ltype == F_RDLCK
			   ? "read" : "write");
		return -1;
	}

	if (tdb->allrecord_lock.count > 1) {
		tdb->allrecord_lock.count--;
		return 0;
	}

	tdb->allrecord_lock.count = 0;
	tdb->allrecord_lock.ltype = 0;

	return tdb_brunlock(tdb, ltype, TDB_HASH_LOCK_START, 0);
}

bool tdb_has_expansion_lock(struct tdb_context *tdb)
{
	return find_nestlock(tdb, TDB_EXPANSION_LOCK) != NULL;
}

bool tdb_has_hash_locks(struct tdb_context *tdb)
{
	unsigned int i;

	for (i=0; i<tdb->num_lockrecs; i++) {
		if (tdb->lockrecs[i].off >= TDB_HASH_LOCK_START
		    && tdb->lockrecs[i].off < (TDB_HASH_LOCK_START
					       + TDB_HASH_LOCK_RANGE))
			return true;
	}
	return false;
}

static bool tdb_has_free_lock(struct tdb_context *tdb)
{
	unsigned int i;

	for (i=0; i<tdb->num_lockrecs; i++) {
		if (tdb->lockrecs[i].off
		    > TDB_HASH_LOCK_START + TDB_HASH_LOCK_RANGE)
			return true;
	}
	return false;
}

int tdb_lock_hashes(struct tdb_context *tdb,
		    tdb_off_t hash_lock,
		    tdb_len_t hash_range,
		    int ltype, enum tdb_lock_flags waitflag)
{
	/* FIXME: Do this properly, using hlock_range */
	unsigned lock = TDB_HASH_LOCK_START
		+ (hash_lock >> (64 - TDB_HASH_LOCK_RANGE_BITS));

	/* a allrecord lock allows us to avoid per chain locks */
	if (tdb->allrecord_lock.count &&
	    (ltype == tdb->allrecord_lock.ltype || ltype == F_RDLCK)) {
		return 0;
	}

	if (tdb->allrecord_lock.count) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_lock_hashes: already have %s allrecordlock",
			   tdb->allrecord_lock.ltype == F_RDLCK
			   ? "read" : "write");
		return -1;
	}

	if (tdb_has_free_lock(tdb)) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_lock_hashes: already have free lock");
		return -1;
	}

	if (tdb_has_expansion_lock(tdb)) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_lock_hashes: already have expansion lock");
		return -1;
	}

	return tdb_nest_lock(tdb, lock, ltype, waitflag);
}

int tdb_unlock_hashes(struct tdb_context *tdb,
		      tdb_off_t hash_lock,
		      tdb_len_t hash_range, int ltype)
{
	unsigned lock = TDB_HASH_LOCK_START
		+ (hash_lock >> (64 - TDB_HASH_LOCK_RANGE_BITS));

	/* a allrecord lock allows us to avoid per chain locks */
	if (tdb->allrecord_lock.count) {
		if (tdb->allrecord_lock.ltype == F_RDLCK
		    && ltype == F_WRLCK) {
			tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_FATAL,
				   "tdb_unlock_hashes RO allrecord!");
			return -1;
		}
		return 0;
	}

	return tdb_nest_unlock(tdb, lock, ltype);
}

/* Hash locks use TDB_HASH_LOCK_START + the next 30 bits.
 * Then we begin; bucket offsets are sizeof(tdb_len_t) apart, so we divide.
 * The result is that on 32 bit systems we don't use lock values > 2^31 on
 * files that are less than 4GB.
 */
static tdb_off_t free_lock_off(tdb_off_t b_off)
{
	return TDB_HASH_LOCK_START + TDB_HASH_LOCK_RANGE
		+ b_off / sizeof(tdb_off_t);
}

int tdb_lock_free_bucket(struct tdb_context *tdb, tdb_off_t b_off,
			 enum tdb_lock_flags waitflag)
{
	assert(b_off >= sizeof(struct tdb_header));

	/* a allrecord lock allows us to avoid per chain locks */
	if (tdb->allrecord_lock.count) {
		if (tdb->allrecord_lock.ltype == F_WRLCK)
			return 0;
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_FATAL,
			 "tdb_lock_free_bucket with RO allrecordlock!");
		return -1;
	}

#if 0 /* FIXME */
	if (tdb_has_expansion_lock(tdb)) {
		tdb_logerr(tdb, TDB_ERR_LOCK, TDB_DEBUG_ERROR,
			   "tdb_lock_free_bucket: already have expansion lock");
		return -1;
	}
#endif

	return tdb_nest_lock(tdb, free_lock_off(b_off), F_WRLCK, waitflag);
}

void tdb_unlock_free_bucket(struct tdb_context *tdb, tdb_off_t b_off)
{
	if (tdb->allrecord_lock.count)
		return;

	tdb_nest_unlock(tdb, free_lock_off(b_off), F_WRLCK);
}

void tdb_lock_init(struct tdb_context *tdb)
{
	tdb->num_lockrecs = 0;
	tdb->lockrecs = NULL;
	tdb->allrecord_lock.count = 0;
}
