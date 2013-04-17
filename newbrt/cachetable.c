/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
#ident "Copyright (c) 2007-2010 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."


#include <toku_portability.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#include "memory.h"
#include "workqueue.h"
#include "threadpool.h"
#include "cachetable.h"
#include "rwlock.h"
#include "toku_worker.h"
#include "log_header.h"
#include "checkpoint.h"
#include "minicron.h"
#include "log-internal.h"


#if !defined(TOKU_CACHETABLE_DO_EVICT_FROM_WRITER)
#error
#endif

// use worker threads 0->no 1->yes
#define DO_WORKER_THREAD 1
#if DO_WORKER_THREAD
static void cachetable_writer(WORKITEM);
static void cachetable_reader(WORKITEM);
#endif

// use cachetable locks 0->no 1->yes
#define DO_CACHETABLE_LOCK 1

// simulate long latency write operations with usleep. time in milliseconds.
#define DO_CALLBACK_USLEEP 0
#define DO_CALLBACK_BUSYWAIT 0

#define TRACE_CACHETABLE 0
#if TRACE_CACHETABLE
#define WHEN_TRACE_CT(x) x
#else
#define WHEN_TRACE_CT(x) ((void)0)
#endif

// these should be in the cachetable object, but we make them file-wide so that gdb can get them easily
static u_int64_t cachetable_hit;
static u_int64_t cachetable_miss;
static u_int64_t cachetable_wait_reading;  // how many times does get_and_pin() wait for a node to be read?
static u_int64_t cachetable_wait_writing;  // how many times does get_and_pin() wait for a node to be written?
static u_int64_t cachetable_puts;          // how many times has a newly created node been put into the cachetable?
static u_int64_t cachetable_prefetches;    // how many times has a block been prefetched into the cachetable?
static u_int64_t cachetable_maybe_get_and_pins;      // how many times has maybe_get_and_pin(_clean) been called?
static u_int64_t cachetable_maybe_get_and_pin_hits;  // how many times has get_and_pin(_clean) returned with a node?
static u_int64_t cachetable_wait_checkpoint;         // number of times get_and_pin waits for a node to be written for a checkpoint
static u_int64_t cachetable_misstime;     // time spent waiting for disk read
static u_int64_t cachetable_waittime;     // time spent waiting for another thread to release lock (e.g. prefetch, writing)
static u_int64_t cachetable_lock_taken = 0;
static u_int64_t cachetable_lock_released = 0;
static u_int64_t local_checkpoint;        // number of times a local checkpoint was taken for a commit (2440)
static u_int64_t local_checkpoint_files;  // number of files subject to local checkpoint taken for a commit (2440)
static u_int64_t local_checkpoint_during_checkpoint;  // number of times a local checkpoint happened during normal checkpoint (2440)



enum ctpair_state {
    CTPAIR_INVALID = 0, // invalid
    CTPAIR_IDLE = 1,    // in memory
    CTPAIR_READING = 2, // being read into memory
    CTPAIR_WRITING = 3, // being written from memory
};


/* The workqueue pointer cq is set in:
 *   cachetable_complete_write_pair()      cq is cleared, called from many paths, cachetable lock is held during this function
 *   cachetable_flush_cachefile()          called during close and truncate, cachetable lock is held during this function
 *   toku_cachetable_unpin_and_remove()    called during node merge, cachetable lock is held during this function
 *
 */
typedef struct ctpair *PAIR;
struct ctpair {
    enum typ_tag tag;
    CACHEFILE cachefile;
    CACHEKEY key;
    void    *value;
    long     size;
    enum ctpair_state state;
    enum cachetable_dirty dirty;

    char     verify_flag;        // Used in verify_cachetable()
    BOOL     write_me;           // write_pair 
    BOOL     remove_me;          // write_pair

    u_int32_t fullhash;

    CACHETABLE_FLUSH_CALLBACK flush_callback;
    CACHETABLE_FETCH_CALLBACK fetch_callback;
    void    *extraargs;

    PAIR     next,prev;          // In LRU list.
    PAIR     hash_chain;


    BOOL     checkpoint_pending; // If this is on, then we have got to write the pair out to disk before modifying it.
    PAIR     pending_next;
    PAIR     pending_prev;

    struct rwlock rwlock; // multiple get's, single writer
    struct workqueue *cq;        // writers sometimes return ctpair's using this queue
    struct workitem asyncwork;   // work item for the worker threads
    u_int32_t refs;  //References that prevent descruction
    int already_removed;  //If a pair is removed from the cachetable, but cannot be freed because refs>0, this is set.
};

static void * const zero_value = 0;
static int const zero_size = 0;

static int maybe_flush_some (CACHETABLE ct, long size);

static inline void
ctpair_add_ref(PAIR p) {
    assert(!p->already_removed);
    p->refs++;
}

static inline void ctpair_destroy(PAIR p) {
    assert(p->refs>0);
    p->refs--;
    if (p->refs==0) {
        rwlock_destroy(&p->rwlock);
        toku_free(p);
    }
}

// The cachetable is as close to an ENV as we get.
// There are 3 locks, must be taken in this order
//      openfd_mutex
//      cachetable_mutex
//      cachefiles_mutex
struct cachetable {
    enum typ_tag tag;
    u_int32_t n_in_table;         // number of pairs in the hash table
    u_int32_t table_size;         // number of buckets in the hash table
    PAIR *table;                  // hash table
    PAIR  head,tail;              // of LRU list. head is the most recently used. tail is least recently used.
    CACHEFILE cachefiles;         // list of cachefiles that use this cachetable
    CACHEFILE cachefiles_in_checkpoint; //list of cachefiles included in checkpoint in progress
    int64_t size_current;            // the sum of the sizes of the pairs in the cachetable
    int64_t size_limit;              // the limit to the sum of the pair sizes
    int64_t size_writing;            // the sum of the sizes of the pairs being written
    TOKULOGGER logger;
    toku_pthread_mutex_t *mutex;  // coarse lock that protects the cachetable, the cachefiles, and the pairs
    toku_pthread_mutex_t cachefiles_mutex;  // lock that protects the cachefiles list
    struct workqueue wq;          // async work queue 
    THREADPOOL threadpool;        // pool of worker threads
    LSN lsn_of_checkpoint_in_progress;
    u_int32_t checkpoint_num_files;  // how many cachefiles are in the checkpoint
    u_int32_t checkpoint_num_txns;   // how many transactions are in the checkpoint
    PAIR pending_head;           // list of pairs marked with checkpoint_pending
    struct rwlock pending_lock;  // multiple writer threads, single checkpoint thread
    struct minicron checkpointer; // the periodic checkpointing thread
    toku_pthread_mutex_t openfd_mutex;  // make toku_cachetable_openfd() single-threaded
    LEAFLOCK_POOL leaflock_pool;
    OMT reserved_filenums;
    char *env_dir;
    BOOL set_env_dir; //Can only set env_dir once
};

// Lock the cachetable
static inline void cachefiles_lock(CACHETABLE ct) {
    int r = toku_pthread_mutex_lock(&ct->cachefiles_mutex); assert(r == 0);
}

// Unlock the cachetable
static inline void cachefiles_unlock(CACHETABLE ct) {
    int r = toku_pthread_mutex_unlock(&ct->cachefiles_mutex); assert(r == 0);
}

// Lock the cachetable
static inline void cachetable_lock(CACHETABLE ct __attribute__((unused))) {
#if DO_CACHETABLE_LOCK
    int r = toku_pthread_mutex_lock(ct->mutex); assert(r == 0);
    cachetable_lock_taken++;
#endif
}

// Unlock the cachetable
static inline void cachetable_unlock(CACHETABLE ct __attribute__((unused))) {
#if DO_CACHETABLE_LOCK
    cachetable_lock_released++;
    int r = toku_pthread_mutex_unlock(ct->mutex); assert(r == 0);
#endif
}

// Wait for cache table space to become available 
// size_current is number of bytes currently occupied by data (referred to by pairs)
// size_writing is number of bytes queued up to be written out (sum of sizes of pairs in CTPAIR_WRITING state)
static inline void cachetable_wait_write(CACHETABLE ct) {
    // if we're writing more than half the data in the cachetable
    while (2*ct->size_writing > ct->size_current) {
        workqueue_wait_write(&ct->wq, 0);
    }
}

enum cachefile_checkpoint_state {
    CS_INVALID = 0,
    CS_NOT_IN_PROGRESS,
    CS_CALLED_BEGIN_CHECKPOINT,
    CS_CALLED_CHECKPOINT
};

struct cachefile {
    CACHEFILE next;
    CACHEFILE next_in_checkpoint;
    BOOL for_checkpoint; //True if part of the in-progress checkpoint
    u_int64_t refcount; /* CACHEFILEs are shared. Use a refcount to decide when to really close it.
			 * The reference count is one for every open DB.
			 * Plus one for every commit/rollback record.  (It would be harder to keep a count for every open transaction,
			 * because then we'd have to figure out if the transaction was already counted.  If we simply use a count for
			 * every record in the transaction, we'll be ok.  Hence we use a 64-bit counter to make sure we don't run out.
			 */
    BOOL is_closing;    /* TRUE if a cachefile is being close/has been closed. */
    struct rwlock fdlock; // Protect changing the fd and is_dev_null
                          // Only write-locked by toku_cachefile_redirect_nullfd()
    BOOL is_dev_null;    //True if was deleted and redirected to /dev/null (starts out FALSE, can be set to TRUE, can never be set back to FALSE)
    int fd;       /* Bug: If a file is opened read-only, then it is stuck in read-only.  If it is opened read-write, then subsequent writers can write to it too. */
    CACHETABLE cachetable;
    struct fileid fileid;
    FILENUM filenum;
    char *fname_in_env; /* Used for logging */

    void *userdata;
    int (*log_fassociate_during_checkpoint)(CACHEFILE cf, void *userdata); // When starting a checkpoint we must log all open files.
    int (*log_suppress_rollback_during_checkpoint)(CACHEFILE cf, void *userdata); // When starting a checkpoint we must log which files need rollbacks suppressed
    int (*close_userdata)(CACHEFILE cf, int fd, void *userdata, char **error_string, BOOL lsnvalid, LSN); // when closing the last reference to a cachefile, first call this function. 
    int (*begin_checkpoint_userdata)(CACHEFILE cf, int fd, LSN lsn_of_checkpoint, void *userdata); // before checkpointing cachefiles call this function.
    int (*checkpoint_userdata)(CACHEFILE cf, int fd, void *userdata); // when checkpointing a cachefile, call this function.
    int (*end_checkpoint_userdata)(CACHEFILE cf, int fd, void *userdata); // after checkpointing cachefiles call this function.
    int (*note_pin_by_checkpoint)(CACHEFILE cf, void *userdata); // add a reference to the userdata to prevent it from being removed from memory
    int (*note_unpin_by_checkpoint)(CACHEFILE cf, void *userdata); // add a reference to the userdata to prevent it from being removed from memory
    toku_pthread_cond_t openfd_wait;    // openfd must wait until file is fully closed (purged from cachetable) if file is opened and closed simultaneously
    toku_pthread_cond_t closefd_wait;   // toku_cachefile_of_iname_and_add_reference() must wait until file is fully closed (purged from cachetable) if run while file is being closed.
    u_int32_t closefd_waiting;          // Number of threads waiting on closefd_wait (0 or 1, error otherwise).
    struct rwlock checkpoint_lock; //protects checkpoint callback functions
                                   //acts as fast mutex by only using 'write-lock'
    LSN most_recent_global_checkpoint_that_finished_early;
    LSN for_local_checkpoint;
    enum cachefile_checkpoint_state checkpoint_state; //Protected by checkpoint_lock
};

static int
checkpoint_thread (void *cachetable_v)
// Effect:  If checkpoint_period>0 thn periodically run a checkpoint.
//  If someone changes the checkpoint_period (calling toku_set_checkpoint_period), then the checkpoint will run sooner or later.
//  If someone sets the checkpoint_shutdown boolean , then this thread exits. 
// This thread notices those changes by waiting on a condition variable.
{
    CACHETABLE ct = cachetable_v;
    int r = toku_checkpoint(ct, ct->logger, NULL, NULL, NULL, NULL);
    if (r) {
        fprintf(stderr, "%s:%d Got error %d while doing checkpoint\n", __FILE__, __LINE__, r);
	abort(); // Don't quite know what to do with these errors.
    }
    return r;
}

int toku_set_checkpoint_period (CACHETABLE ct, u_int32_t new_period) {
    return toku_minicron_change_period(&ct->checkpointer, new_period);
}

u_int32_t toku_get_checkpoint_period (CACHETABLE ct) {
    return toku_minicron_get_period(&ct->checkpointer);
}

int toku_create_cachetable(CACHETABLE *result, long size_limit, LSN UU(initial_lsn), TOKULOGGER logger) {
    TAGMALLOC(CACHETABLE, ct);
    if (ct == 0) return ENOMEM;
    memset(ct, 0, sizeof(*ct));
    ct->table_size = 4;
    rwlock_init(&ct->pending_lock);
    XCALLOC_N(ct->table_size, ct->table);
    ct->size_limit = size_limit;
    ct->logger = logger;
    toku_init_workers(&ct->wq, &ct->threadpool);
    ct->mutex = workqueue_lock_ref(&ct->wq);
    int r = toku_pthread_mutex_init(&ct->openfd_mutex, NULL); assert(r == 0);
    r = toku_pthread_mutex_init(&ct->cachefiles_mutex, 0); assert(r == 0);
    toku_minicron_setup(&ct->checkpointer, 0, checkpoint_thread, ct); // default is no checkpointing
    r = toku_leaflock_create(&ct->leaflock_pool); assert(r==0);
    r = toku_omt_create(&ct->reserved_filenums);  assert(r==0);
    ct->env_dir = toku_xstrdup(".");
    *result = ct;
    return 0;
}

uint64_t toku_cachetable_reserve_memory(CACHETABLE ct, double fraction) {
    cachetable_lock(ct);
    cachetable_wait_write(ct);
    uint64_t reserved_memory = fraction*ct->size_limit;
    {
	int r = maybe_flush_some(ct, reserved_memory);
	if (r) {
	    cachetable_unlock(ct);
	    return r;
	}
    }
    ct->size_current += reserved_memory;
    cachetable_unlock(ct);
    return reserved_memory;
}

void toku_cachetable_release_reserved_memory(CACHETABLE ct, uint64_t reserved_memory) {
    cachetable_lock(ct);
    ct->size_current -= reserved_memory;
    assert(ct->size_current >= 0);
    cachetable_unlock(ct);
}

void
toku_cachetable_set_env_dir(CACHETABLE ct, const char *env_dir) {
    assert(!ct->set_env_dir);
    toku_free(ct->env_dir);
    ct->env_dir = toku_xstrdup(env_dir);
    ct->set_env_dir = TRUE;
}

//
// Increment the reference count
// MUST HOLD cachetable lock
static void
cachefile_refup (CACHEFILE cf) {
    cf->refcount++;
}

// What cachefile goes with particular iname (iname relative to env)?
// The transaction that is adding the reference might not have a reference
// to the brt, therefore the cachefile might be closing.
// If closing, we want to return that it is not there, but must wait till after
// the close has finished.
// Once the close has finished, there must not be a cachefile with that name
// in the cachetable.
int toku_cachefile_of_iname_in_env (CACHETABLE ct, const char *iname_in_env, CACHEFILE *cf) {
    BOOL restarted = FALSE;
    cachefiles_lock(ct);
    CACHEFILE extant;
    int r;
restart:
    r = ENOENT;
    for (extant = ct->cachefiles; extant; extant=extant->next) {
        if (extant->fname_in_env &&
            !strcmp(extant->fname_in_env, iname_in_env)) {
            assert(!restarted); //If restarted and found again, this is an error.
            if (extant->is_closing) {
                //Cachefile is closing, wait till finished.
                assert(extant->closefd_waiting==0); //Single client thread (any more and this needs to be re-analyzed).
                extant->closefd_waiting++;
		int rwait = toku_pthread_cond_wait(&extant->closefd_wait, ct->mutex);
		assert(rwait == 0);
                restarted = TRUE;
                goto restart; //Restart and verify that it is not found in the second loop.
            }
	    *cf = extant;
	    r = 0;
            break;
	}
    }
    cachefiles_unlock(ct);
    return r;
}

// What cachefile goes with particular fd?
// This function can only be called if the brt is still open, so file must 
// still be open and cannot be in the is_closing state.
int toku_cachefile_of_filenum (CACHETABLE ct, FILENUM filenum, CACHEFILE *cf) {
    cachefiles_lock(ct);
    CACHEFILE extant;
    int r = ENOENT;
    *cf = NULL;
    for (extant = ct->cachefiles; extant; extant=extant->next) {
	if (extant->filenum.fileid==filenum.fileid) {
            assert(!extant->is_closing);
	    *cf = extant;
            r = 0;
            break;
	}
    }
    cachefiles_unlock(ct);
    return r;
}

static FILENUM next_filenum_to_use={0};

static void cachefile_init_filenum(CACHEFILE cf, int fd, const char *fname_in_env, struct fileid fileid) {
    cf->fd = fd;
    cf->fileid = fileid;
    cf->fname_in_env = toku_xstrdup(fname_in_env);
}

// If something goes wrong, close the fd.  After this, the caller shouldn't close the fd, but instead should close the cachefile.
int toku_cachetable_openfd (CACHEFILE *cfptr, CACHETABLE ct, int fd, const char *fname_in_env) {
    return toku_cachetable_openfd_with_filenum(cfptr, ct, fd, fname_in_env, FALSE, next_filenum_to_use, FALSE);
}

static int
find_by_filenum (OMTVALUE v, void *filenumv) {
    FILENUM fnum     = *(FILENUM*)v;
    FILENUM fnumfind = *(FILENUM*)filenumv;
    if (fnum.fileid<fnumfind.fileid) return -1;
    if (fnum.fileid>fnumfind.fileid) return +1;
    return 0;
}

static BOOL
is_filenum_reserved(CACHETABLE ct, FILENUM filenum) {
    OMTVALUE v;
    int r;
    BOOL rval;

    r = toku_omt_find_zero(ct->reserved_filenums, find_by_filenum, &filenum, &v, NULL, NULL);
    if (r==0) {
        FILENUM* found = v;
        assert(found->fileid == filenum.fileid);
        rval = TRUE;
    }
    else {
        assert(r==DB_NOTFOUND);
        rval = FALSE;
    }
    return rval;
}

static void
reserve_filenum(CACHETABLE ct, FILENUM filenum) {
    int r;
    assert(filenum.fileid != FILENUM_NONE.fileid);

    uint32_t index;
    r = toku_omt_find_zero(ct->reserved_filenums, find_by_filenum, &filenum, NULL, &index, NULL);
    assert(r==DB_NOTFOUND);
    FILENUM *XMALLOC(entry);
    *entry = filenum;
    r = toku_omt_insert_at(ct->reserved_filenums, entry, index);
    assert(r==0);
}

static void
unreserve_filenum(CACHETABLE ct, FILENUM filenum) {
    OMTVALUE v;
    int r;

    uint32_t index;
    r = toku_omt_find_zero(ct->reserved_filenums, find_by_filenum, &filenum, &v, &index, NULL);
    assert(r==0);
    FILENUM* found = v;
    assert(found->fileid == filenum.fileid);
    toku_free(found);
    r = toku_omt_delete_at(ct->reserved_filenums, index);
    assert(r==0);
}

    
int
toku_cachetable_reserve_filenum (CACHETABLE ct, FILENUM *reserved_filenum, BOOL with_filenum, FILENUM filenum) {
    int r;
    CACHEFILE extant;
    
    cachetable_lock(ct);
    cachefiles_lock(ct);

    if (with_filenum) {
        // verify that filenum is not in use
        for (extant = ct->cachefiles; extant; extant=extant->next) {
            if (filenum.fileid == extant->filenum.fileid) {
                r = EEXIST;
                goto exit;
            }
        }
        if (is_filenum_reserved(ct, filenum)) {
            r = EEXIST;
            goto exit;
        }
    } else {
        // find an unused fileid and use it
    try_again:
        for (extant = ct->cachefiles; extant; extant=extant->next) {
            if (next_filenum_to_use.fileid==extant->filenum.fileid) {
                next_filenum_to_use.fileid++;
                goto try_again;
            }
        }
        if (is_filenum_reserved(ct, next_filenum_to_use)) {
            next_filenum_to_use.fileid++;
            goto try_again;
        }
    }
    {
        //Reserve a filenum.
        FILENUM reserved;
        if (with_filenum)
            reserved.fileid = filenum.fileid;
        else
            reserved.fileid = next_filenum_to_use.fileid++;
        reserve_filenum(ct, reserved);
        *reserved_filenum = reserved;
        r = 0;
    }
 exit:
    cachefiles_unlock(ct);
    cachetable_unlock(ct);
    return r;
}

void
toku_cachetable_unreserve_filenum (CACHETABLE ct, FILENUM reserved_filenum) {
    cachetable_lock(ct);
    cachefiles_lock(ct);
    unreserve_filenum(ct, reserved_filenum);
    cachefiles_unlock(ct);
    cachetable_unlock(ct);
}

int toku_cachetable_openfd_with_filenum (CACHEFILE *cfptr, CACHETABLE ct, int fd, 
					 const char *fname_in_env,
					 BOOL with_filenum, FILENUM filenum, BOOL reserved) {
    int r;
    CACHEFILE extant;
    struct fileid fileid;
    
    if (with_filenum) assert(filenum.fileid != FILENUM_NONE.fileid);
    if (reserved) assert(with_filenum);
    r = toku_os_get_unique_file_id(fd, &fileid);
    if (r != 0) { 
        r=errno; close(fd); // no change for t:2444
        return r;
    }
    r = toku_pthread_mutex_lock(&ct->openfd_mutex);   // purpose is to make this function single-threaded
    assert(r==0);
    cachetable_lock(ct);
    cachefiles_lock(ct);
    for (extant = ct->cachefiles; extant; extant=extant->next) {
	if (memcmp(&extant->fileid, &fileid, sizeof(fileid))==0) {
            //File is already open (and in cachetable as extant)
            cachefile_refup(extant);
	    if (extant->is_closing) {
		// if another thread is closing this file, wait until the close is fully complete
                cachefiles_unlock(ct); //Cannot hold cachefiles lock over the cond_wait
		r = toku_pthread_cond_wait(&extant->openfd_wait, ct->mutex);
		assert(r == 0);
                cachefiles_lock(ct);
		goto try_again;    // other thread has closed this file, go create a new cachefile
	    }	    
            assert(!is_filenum_reserved(ct, extant->filenum));
	    r = close(fd);  // no change for t:2444
            assert(r == 0);
	    // re-use pre-existing cachefile 
	    *cfptr = extant;
	    r = 0;
	    goto exit;
	}
    }

    //File is not open.  Make a new cachefile.

    if (with_filenum) {
        // verify that filenum is not in use
        for (extant = ct->cachefiles; extant; extant=extant->next) {
            if (filenum.fileid == extant->filenum.fileid) {
                r = EEXIST;
                goto exit;
            }
        }
        if (is_filenum_reserved(ct, filenum)) {
            if (reserved)
                unreserve_filenum(ct, filenum);
            else {
                r = EEXIST;
                goto exit;
            }
        }
    } else {
        // find an unused fileid and use it
    try_again:
        assert(next_filenum_to_use.fileid != FILENUM_NONE.fileid);
        for (extant = ct->cachefiles; extant; extant=extant->next) {
            if (next_filenum_to_use.fileid==extant->filenum.fileid) {
                next_filenum_to_use.fileid++;
                goto try_again;
            }
        }
        if (is_filenum_reserved(ct, next_filenum_to_use)) {
            next_filenum_to_use.fileid++;
            goto try_again;
        }
    }
    {
	// create a new cachefile entry in the cachetable
        CACHEFILE XCALLOC(newcf);
        newcf->cachetable = ct;
        newcf->filenum.fileid = with_filenum ? filenum.fileid : next_filenum_to_use.fileid++;
        cachefile_init_filenum(newcf, fd, fname_in_env, fileid);
	newcf->refcount = 1;
	newcf->next = ct->cachefiles;
	ct->cachefiles = newcf;

        rwlock_init(&newcf->fdlock);
        rwlock_init(&newcf->checkpoint_lock);
        newcf->most_recent_global_checkpoint_that_finished_early = ZERO_LSN;
        newcf->for_local_checkpoint = ZERO_LSN;
        newcf->checkpoint_state = CS_NOT_IN_PROGRESS;

	r = toku_pthread_cond_init(&newcf->openfd_wait, NULL); assert(r == 0);
	r = toku_pthread_cond_init(&newcf->closefd_wait, NULL); assert(r == 0);
	*cfptr = newcf;
	r = 0;
    }
 exit:
    cachefiles_unlock(ct);
    {
	int rm = toku_pthread_mutex_unlock(&ct->openfd_mutex);
	assert (rm == 0);
    } 
    cachetable_unlock(ct);
    return r;
}

static int cachetable_flush_cachefile (CACHETABLE, CACHEFILE cf);
static void assert_cachefile_is_flushed_and_removed (CACHETABLE ct, CACHEFILE cf);

// Do not use this function to redirect to /dev/null.  
int toku_cachefile_redirect (CACHEFILE cf, int newfd, const char *fname_in_env) {
    int r;
    struct fileid newfileid;
    assert(newfd >= 0);
    r = toku_os_get_unique_file_id(newfd, &newfileid);
    assert(r==0);

    CACHETABLE ct = cf->cachetable;
    cachetable_lock(ct);
    assert_cachefile_is_flushed_and_removed(ct, cf);
    {
        //Verify fd is not already in use, use unique file id.
        CACHEFILE extant;
        cachefiles_lock(ct);
        for (extant = ct->cachefiles; extant; extant=extant->next) {
            assert(memcmp(&extant->fileid, &newfileid, sizeof(newfileid))!=0);
        }
        cachefiles_unlock(ct);
    }

    rwlock_write_lock(&cf->fdlock, ct->mutex);
    assert(!cf->is_closing);
    assert(!cf->is_dev_null);
    assert(cf->closefd_waiting == 0);

    r = toku_file_fsync_without_accounting(cf->fd); //t:2444
    assert(r == 0);   
    close(cf->fd);                         // close the old file

    toku_free(cf->fname_in_env);           // free old iname string
    cf->fname_in_env = NULL;               // hygiene
    cachefile_init_filenum(cf, newfd, fname_in_env, newfileid);
    rwlock_write_unlock(&cf->fdlock);
    cachetable_unlock(ct);

    return 0;
}

//TEST_ONLY_FUNCTION
int toku_cachetable_openf (CACHEFILE *cfptr, CACHETABLE ct, const char *fname_in_env, int flags, mode_t mode) {
    char *fname_in_cwd = toku_construct_full_name(2, ct->env_dir, fname_in_env);
    int fd = open(fname_in_cwd, flags+O_BINARY, mode);
    int r;
    if (fd<0) r = errno;
    else      r = toku_cachetable_openfd (cfptr, ct, fd, fname_in_env);
    toku_free(fname_in_cwd);
    return r;
}

WORKQUEUE toku_cachetable_get_workqueue(CACHETABLE ct) {
    return &ct->wq;
}

void toku_cachefile_get_workqueue_load (CACHEFILE cf, int *n_in_queue, int *n_threads) {
    CACHETABLE ct = cf->cachetable;
    *n_in_queue = workqueue_n_in_queue(&ct->wq, 1);
    *n_threads  = threadpool_get_current_threads(ct->threadpool);
}

//Test-only function
int toku_cachefile_set_fd (CACHEFILE cf, int fd, const char *fname_in_env) {
    int r;
    struct fileid fileid;
    (void)toku_cachefile_get_and_pin_fd(cf);
    r=toku_os_get_unique_file_id(fd, &fileid);
    if (r != 0) { 
        r=errno; close(fd); goto cleanup; // no change for t:2444
    }
    if (cf->close_userdata && (r = cf->close_userdata(cf, cf->fd, cf->userdata, 0, FALSE, ZERO_LSN))) {
        goto cleanup;
    }
    cf->close_userdata = NULL;
    cf->checkpoint_userdata = NULL;
    cf->begin_checkpoint_userdata = NULL;
    cf->end_checkpoint_userdata = NULL;
    cf->userdata = NULL;

    close(cf->fd); // no change for t:2444
    cf->fd = -1;
    if (cf->fname_in_env) {
	toku_free(cf->fname_in_env);
	cf->fname_in_env = NULL;
    }
    //It is safe to have the name repeated since this is a newbrt-only test function.
    //There isn't an environment directory so its both env/cwd.
    cachefile_init_filenum(cf, fd, fname_in_env, fileid);
    r = 0;
cleanup:
    toku_cachefile_unpin_fd(cf);
    return r;
}

LEAFLOCK_POOL
toku_cachefile_leaflock_pool (CACHEFILE cf) {
    return cf->cachetable->leaflock_pool;
}

char *
toku_cachefile_fname_in_env (CACHEFILE cf) {
    return cf->fname_in_env;
}

int toku_cachefile_get_and_pin_fd (CACHEFILE cf) {
    CACHETABLE ct = cf->cachetable;
    cachetable_lock(ct);
    rwlock_prefer_read_lock(&cf->fdlock, cf->cachetable->mutex);
    cachetable_unlock(ct);
    return cf->fd;
}

void toku_cachefile_unpin_fd (CACHEFILE cf) {
    CACHETABLE ct = cf->cachetable;
    cachetable_lock(ct);
    rwlock_read_unlock(&cf->fdlock);
    cachetable_unlock(ct);
}

//Must be holding a read or write lock on cf->fdlock
BOOL
toku_cachefile_is_dev_null_unlocked (CACHEFILE cf) {
    return cf->is_dev_null;
}

//Must already be holding fdlock (read or write)
int
toku_cachefile_truncate (CACHEFILE cf, toku_off_t new_size) {
    int r;
    if (toku_cachefile_is_dev_null_unlocked(cf)) r = 0; //Don't truncate /dev/null
    else {
        r = ftruncate(cf->fd, new_size);
        if (r != 0)
            r = errno;
    }
    return r;
}

static CACHEFILE remove_cf_from_list_locked (CACHEFILE cf, CACHEFILE list) {
    if (list==0) return 0;
    else if (list==cf) {
	return list->next;
    } else {
	list->next = remove_cf_from_list_locked(cf, list->next);
	return list;
    }
}

static void remove_cf_from_cachefiles_list (CACHEFILE cf) {
    CACHETABLE ct = cf->cachetable;
    cachefiles_lock(ct);
    ct->cachefiles = remove_cf_from_list_locked(cf, ct->cachefiles);
    cachefiles_unlock(ct);
}

int toku_cachefile_close (CACHEFILE *cfp, char **error_string, BOOL oplsn_valid, LSN oplsn) {

    CACHEFILE cf = *cfp;
    CACHETABLE ct = cf->cachetable;
    cachetable_lock(ct);
    assert(cf->refcount>0);
    if (oplsn_valid)
        assert(cf->refcount==1); //Recovery is trying to force an lsn.  Must get passed through.
    cf->refcount--;
    if (cf->refcount==0) {
        //Checkpoint holds a reference, close should be impossible if still in use by a checkpoint.
        assert(!cf->next_in_checkpoint);
        assert(!cf->for_checkpoint);
        assert(!cf->is_closing);
        cf->is_closing = TRUE; //Mark this cachefile so that no one will re-use it.
	int r;
	// cachetable_flush_cachefile() may release and retake cachetable_lock,
	// allowing another thread to get into either/both of
        //  - toku_cachetable_openfd()
        //  - toku_cachefile_of_iname_and_add_reference()
	if ((r = cachetable_flush_cachefile(ct, cf))) {
	error:
	    remove_cf_from_cachefiles_list(cf);
	    if (cf->refcount > 0) {
                int rs;
		assert(cf->refcount == 1);       // toku_cachetable_openfd() is single-threaded
                assert(!cf->next_in_checkpoint); //checkpoint cannot run on a closing file
                assert(!cf->for_checkpoint);     //checkpoint cannot run on a closing file
		rs = toku_pthread_cond_signal(&cf->openfd_wait); assert(rs == 0);
	    }
            if (cf->closefd_waiting > 0) {
                int rs;
                assert(cf->closefd_waiting == 1);
		rs = toku_pthread_cond_signal(&cf->closefd_wait); assert(rs == 0);
            }
	    // we can destroy the condition variables because if there was another thread waiting, it was already signalled
            {
                int rd;
                rd = toku_pthread_cond_destroy(&cf->openfd_wait);
                assert(rd == 0);
                rd = toku_pthread_cond_destroy(&cf->closefd_wait);
                assert(rd == 0);
            }
	    if (cf->fname_in_env) toku_free(cf->fname_in_env);

            rwlock_write_lock(&cf->fdlock, ct->mutex);
            if ( !toku_cachefile_is_dev_null_unlocked(cf) ) {
                int r3 = toku_file_fsync_without_accounting(cf->fd); //t:2444
                if (r3!=0) fprintf(stderr, "%s:%d During error handling, could not fsync file r=%d errno=%d\n", __FILE__, __LINE__, r3, errno);
            }
	    int r2 = close(cf->fd);
	    if (r2!=0) fprintf(stderr, "%s:%d During error handling, could not close file r=%d errno=%d\n", __FILE__, __LINE__, r2, errno);
	    //assert(r == 0);
            rwlock_write_unlock(&cf->fdlock);
            rwlock_destroy(&cf->fdlock);
            rwlock_write_lock(&cf->checkpoint_lock, ct->mutex); //Just to make sure we can get it
            rwlock_write_unlock(&cf->checkpoint_lock);
            rwlock_destroy(&cf->checkpoint_lock);
	    toku_free(cf);
	    *cfp = NULL;
	    cachetable_unlock(ct);
	    return r;
        }
	if (cf->close_userdata) {
            rwlock_prefer_read_lock(&cf->fdlock, ct->mutex);
            r = cf->close_userdata(cf, cf->fd, cf->userdata, error_string, oplsn_valid, oplsn);
            rwlock_read_unlock(&cf->fdlock);
            if (r!=0) goto error;
	}
       	cf->close_userdata = NULL;
	cf->checkpoint_userdata = NULL;
	cf->begin_checkpoint_userdata = NULL;
	cf->end_checkpoint_userdata = NULL;
	cf->userdata = NULL;
        remove_cf_from_cachefiles_list(cf);
        // refcount could be non-zero if another thread is trying to open this cachefile,
	// but is blocked in toku_cachetable_openfd() waiting for us to finish closing it.
	if (cf->refcount > 0) {
            int rs;
	    assert(cf->refcount == 1);   // toku_cachetable_openfd() is single-threaded
	    rs = toku_pthread_cond_signal(&cf->openfd_wait); assert(rs == 0);
	}
        if (cf->closefd_waiting > 0) {
            int rs;
            assert(cf->closefd_waiting == 1);
            rs = toku_pthread_cond_signal(&cf->closefd_wait); assert(rs == 0);
        }
        // we can destroy the condition variables because if there was another thread waiting, it was already signalled
        {
            int rd;
            rd = toku_pthread_cond_destroy(&cf->openfd_wait);
            assert(rd == 0);
            rd = toku_pthread_cond_destroy(&cf->closefd_wait);
            assert(rd == 0);
        }
        rwlock_write_lock(&cf->fdlock, ct->mutex); //Just make sure we can get it.
        cachetable_unlock(ct);

        if ( !toku_cachefile_is_dev_null_unlocked(cf) ) {
            r = toku_file_fsync_without_accounting(cf->fd); //t:2444
            assert(r == 0);   
        }

        cachetable_lock(ct);
        rwlock_write_unlock(&cf->fdlock);
        rwlock_destroy(&cf->fdlock);
        rwlock_write_lock(&cf->checkpoint_lock, ct->mutex); //Just to make sure we can get it
        rwlock_write_unlock(&cf->checkpoint_lock);
        rwlock_destroy(&cf->checkpoint_lock);
        cachetable_unlock(ct);

	r = close(cf->fd);
	assert(r == 0);
        cf->fd = -1;

	if (cf->fname_in_env) toku_free(cf->fname_in_env);
	toku_free(cf);
	*cfp=NULL;
	return r;
    } else {
        cachetable_unlock(ct);
	*cfp=NULL;
	return 0;
    }
}

int toku_cachefile_flush (CACHEFILE cf) {
    CACHETABLE ct = cf->cachetable;
    cachetable_lock(ct);
    int r = cachetable_flush_cachefile(ct, cf);
    cachetable_unlock(ct);
    return r;
}

// This hash function comes from Jenkins:  http://burtleburtle.net/bob/c/lookup3.c
// The idea here is to mix the bits thoroughly so that we don't have to do modulo by a prime number.
// Instead we can use a bitmask on a table of size power of two.
// This hash function does yield improved performance on ./db-benchmark-test-tokudb and ./scanscan
static inline u_int32_t rot(u_int32_t x, u_int32_t k) {
    return (x<<k) | (x>>(32-k));
}
static inline u_int32_t final (u_int32_t a, u_int32_t b, u_int32_t c) {
    c ^= b; c -= rot(b,14);
    a ^= c; a -= rot(c,11);
    b ^= a; b -= rot(a,25);
    c ^= b; c -= rot(b,16);
    a ^= c; a -= rot(c,4); 
    b ^= a; b -= rot(a,14);
    c ^= b; c -= rot(b,24);
    return c;
}

u_int32_t toku_cachetable_hash (CACHEFILE cachefile, BLOCKNUM key)
// Effect: Return a 32-bit hash key.  The hash key shall be suitable for using with bitmasking for a table of size power-of-two.
{
    return final(cachefile->filenum.fileid, (u_int32_t)(key.b>>32), (u_int32_t)key.b);
}

#if 0
static unsigned int hashit (CACHETABLE ct, CACHEKEY key, CACHEFILE cachefile) {
    assert(0==(ct->table_size & (ct->table_size -1))); // make sure table is power of two
    return (toku_cachetable_hash(key,cachefile))&(ct->table_size-1);
}
#endif

static void cachetable_rehash (CACHETABLE ct, u_int32_t newtable_size) {
    // printf("rehash %p %d %d %d\n", t, primeindexdelta, ct->n_in_table, ct->table_size);

    assert(newtable_size>=4 && ((newtable_size & (newtable_size-1))==0));
    PAIR *newtable = toku_calloc(newtable_size, sizeof(*ct->table));
    u_int32_t i;
    //printf("%s:%d newtable_size=%d\n", __FILE__, __LINE__, newtable_size);
    assert(newtable!=0);
    u_int32_t oldtable_size = ct->table_size;
    ct->table_size=newtable_size;
    for (i=0; i<newtable_size; i++) newtable[i]=0;
    for (i=0; i<oldtable_size; i++) {
	PAIR p;
	while ((p=ct->table[i])!=0) {
	    unsigned int h = p->fullhash&(newtable_size-1);
	    ct->table[i] = p->hash_chain;
	    p->hash_chain = newtable[h];
	    newtable[h] = p;
	}
    }
    toku_free(ct->table);
    // printf("Freed\n");
    ct->table=newtable;
    //printf("Done growing or shrinking\n");
}

static void lru_remove (CACHETABLE ct, PAIR p) {
    if (p->next) {
	p->next->prev = p->prev;
    } else {
	assert(ct->tail==p);
	ct->tail = p->prev;
    }
    if (p->prev) {
	p->prev->next = p->next;
    } else {
	assert(ct->head==p);
	ct->head = p->next;
    }
    p->prev = p->next = 0;
}

static void lru_add_to_list (CACHETABLE ct, PAIR p) {
    // requires that touch_me is not currently in the table.
    assert(p->prev==0);
    p->prev = 0;
    p->next = ct->head;
    if (ct->head) {
	ct->head->prev = p;
    } else {
	assert(!ct->tail);
	ct->tail = p;
    }
    ct->head = p; 
}

static void lru_touch (CACHETABLE ct, PAIR p) {
    lru_remove(ct,p);
    lru_add_to_list(ct,p);
}

static PAIR remove_from_hash_chain (PAIR remove_me, PAIR list) {
    if (remove_me==list) return list->hash_chain;
    list->hash_chain = remove_from_hash_chain(remove_me, list->hash_chain);
    return list;
}

//Remove a pair from the list of pairs that were marked with the
//pending bit for the in-progress checkpoint.
//Requires: cachetable lock is held during duration.
static void
pending_pairs_remove (CACHETABLE ct, PAIR p) {
    if (p->pending_next) {
	p->pending_next->pending_prev = p->pending_prev;
    }
    if (p->pending_prev) {
	p->pending_prev->pending_next = p->pending_next;
    }
    else if (ct->pending_head==p) {
	ct->pending_head = p->pending_next;
    }
    p->pending_prev = p->pending_next = NULL;
}

// Remove a pair from the cachetable
// Effects: the pair is removed from the LRU list and from the cachetable's hash table.
// The size of the objects in the cachetable is adjusted by the size of the pair being
// removed.

static void cachetable_remove_pair (CACHETABLE ct, PAIR p) {
    lru_remove(ct, p);
    pending_pairs_remove(ct, p);

    assert(ct->n_in_table>0);
    ct->n_in_table--;
    // Remove it from the hash chain.
    {
	unsigned int h = p->fullhash&(ct->table_size-1);
	ct->table[h] = remove_from_hash_chain (p, ct->table[h]);
    }
    ct->size_current -= p->size; assert(ct->size_current >= 0);
    p->already_removed = TRUE;
}

// Maybe remove a pair from the cachetable and free it, depending on whether
// or not there are any threads interested in the pair.  The flush callback
// is called with write_me and keep_me both false, and the pair is destroyed.

static void cachetable_maybe_remove_and_free_pair (CACHETABLE ct, PAIR p) {
    if (rwlock_users(&p->rwlock) == 0) {
        cachetable_remove_pair(ct, p);

        // helgrind
        CACHETABLE_FLUSH_CALLBACK flush_callback = p->flush_callback;
        CACHEFILE cachefile = p->cachefile;
        CACHEKEY key = p->key;
        void *value = p->value;
        void *extraargs = p->extraargs;
        long size = p->size;

        rwlock_prefer_read_lock(&cachefile->fdlock, ct->mutex);
        cachetable_unlock(ct);

        flush_callback(cachefile, cachefile->fd, key, value, extraargs, size, FALSE, FALSE, TRUE);

        cachetable_lock(ct);
        rwlock_read_unlock(&cachefile->fdlock);

        ctpair_destroy(p);
    }
}

static void abort_fetch_pair(PAIR p) {
    rwlock_write_unlock(&p->rwlock);
    if (rwlock_users(&p->rwlock) == 0)
        ctpair_destroy(p);
}

// Read a pair from a cachefile into memory using the pair's fetch callback
static int cachetable_fetch_pair(CACHETABLE ct, CACHEFILE cf, PAIR p) {
    // helgrind
    CACHETABLE_FETCH_CALLBACK fetch_callback = p->fetch_callback;
    CACHEKEY key = p->key;
    u_int32_t fullhash = p->fullhash;
    void *extraargs = p->extraargs;

    void *toku_value = 0;
    long size = 0;

    WHEN_TRACE_CT(printf("%s:%d CT: fetch_callback(%lld...)\n", __FILE__, __LINE__, key));    

    rwlock_prefer_read_lock(&cf->fdlock, ct->mutex);
    cachetable_unlock(ct);

    int r;
    if (toku_cachefile_is_dev_null_unlocked(cf)) r = -1;
    else r = fetch_callback(cf, cf->fd, key, fullhash, &toku_value, &size, extraargs);

    cachetable_lock(ct);
    rwlock_read_unlock(&cf->fdlock);

    if (r) {
        cachetable_remove_pair(ct, p);
        p->state = CTPAIR_INVALID;
        if (p->cq) {
            workqueue_enq(p->cq, &p->asyncwork, 1);
            return r;
        }
        abort_fetch_pair(p);
        return r;
    } else {
        lru_touch(ct, p);
        p->value = toku_value;
        p->size = size;
        ct->size_current += size;
        if (p->cq) {
            workqueue_enq(p->cq, &p->asyncwork, 1);
            return 0;
        }
        p->state = CTPAIR_IDLE;
        rwlock_write_unlock(&p->rwlock);
        if (0) printf("%s:%d %"PRId64" complete\n", __FUNCTION__, __LINE__, key.b);
        return 0;
    }
}

static void cachetable_complete_write_pair (CACHETABLE ct, PAIR p, BOOL do_remove);

// Write a pair to storage
// Effects: an exclusive lock on the pair is obtained, the write callback is called,
// the pair dirty state is adjusted, and the write is completed.  The write_me boolean
// is true when the pair is dirty and the pair is requested to be written.  The keep_me
// boolean is true, so the pair is not yet evicted from the cachetable.
// Requires: This thread must hold the write lock for the pair.
static void cachetable_write_pair(CACHETABLE ct, PAIR p) {
    rwlock_read_lock(&ct->pending_lock, ct->mutex);

    // helgrind
    CACHETABLE_FLUSH_CALLBACK flush_callback = p->flush_callback;
    CACHEFILE cachefile = p->cachefile;
    CACHEKEY key = p->key;
    void *value = p->value;
    void *extraargs = p->extraargs;
    long size = p->size;
    BOOL dowrite = (BOOL)(p->dirty && p->write_me);
    BOOL for_checkpoint = p->checkpoint_pending;

    //Must set to FALSE before releasing cachetable lock
    p->checkpoint_pending = FALSE;   // This is the only place this flag is cleared.
    rwlock_prefer_read_lock(&cachefile->fdlock, ct->mutex);
    cachetable_unlock(ct);

    // write callback
    if (toku_cachefile_is_dev_null_unlocked(cachefile)) dowrite = FALSE;
    flush_callback(cachefile, cachefile->fd, key, value, extraargs, size, dowrite, TRUE, for_checkpoint);
#if DO_CALLBACK_USLEEP
    usleep(DO_CALLBACK_USLEEP);
#endif
#if DO_CALLBACK_BUSYWAIT
    struct timeval tstart;
    gettimeofday(&tstart, 0);
    long long ltstart = tstart.tv_sec * 1000000 + tstart.tv_usec;
    while (1) {
        struct timeval t;
        gettimeofday(&t, 0);
        long long lt = t.tv_sec * 1000000 + t.tv_usec;
        if (lt - ltstart > DO_CALLBACK_BUSYWAIT)
            break;
    }
#endif

    cachetable_lock(ct);
    rwlock_read_unlock(&cachefile->fdlock);

    // the pair is no longer dirty once written
    if (p->dirty && p->write_me)
        p->dirty = CACHETABLE_CLEAN;

    assert(!p->checkpoint_pending);
    rwlock_read_unlock(&ct->pending_lock);

    // stuff it into a completion queue for delayed completion if a completion queue exists
    // otherwise complete the write now
    if (p->cq)
        workqueue_enq(p->cq, &p->asyncwork, 1);
    else
        cachetable_complete_write_pair(ct, p, p->remove_me);
}

// complete the write of a pair by reseting the writing flag, adjusting the write
// pending size, and maybe removing the pair from the cachetable if there are no
// references to it

static void cachetable_complete_write_pair (CACHETABLE ct, PAIR p, BOOL do_remove) {
    p->cq = 0;
    p->state = CTPAIR_IDLE;

    // maybe wakeup any stalled writers when the pending writes fall below 
    // 1/8 of the size of the cachetable
    ct->size_writing -= p->size; 
    assert(ct->size_writing >= 0);
    if (8*ct->size_writing <= ct->size_current)
        workqueue_wakeup_write(&ct->wq, 0);

    rwlock_write_unlock(&p->rwlock);
    if (do_remove)
        cachetable_maybe_remove_and_free_pair(ct, p);
}

// flush and remove a pair from the cachetable.  the callbacks are run by a thread in
// a thread pool.

static void flush_and_maybe_remove (CACHETABLE ct, PAIR p, BOOL write_me) {
    rwlock_write_lock(&p->rwlock, ct->mutex);
    p->state = CTPAIR_WRITING;
    assert(ct->size_writing>=0);
    ct->size_writing += p->size;
    assert(ct->size_writing >= 0);
    p->write_me = write_me;
    p->remove_me = TRUE;
#if DO_WORKER_THREAD
    WORKITEM wi = &p->asyncwork;
    workitem_init(wi, cachetable_writer, p);
    // evictions without a write or unpinned pair's that are clean
    // can be run in the current thread
    if (!p->write_me || (!rwlock_readers(&p->rwlock) && !p->dirty)) {
        cachetable_write_pair(ct, p);
    } else {
#if !TOKU_CACHETABLE_DO_EVICT_FROM_WRITER
        p->remove_me = FALSE;           // run the remove on the main thread
#endif
        workqueue_enq(&ct->wq, wi, 0);
    }
#else
    cachetable_write_pair(ct, p);
#endif
}

static int maybe_flush_some (CACHETABLE ct, long size) {
    int r = 0;
again:
    if (size + ct->size_current > ct->size_limit + ct->size_writing) {
	{
	    //unsigned long rss __attribute__((__unused__)) = check_max_rss();
	    //printf("this-size=%.6fMB projected size = %.2fMB  limit=%2.fMB  rss=%2.fMB\n", size/(1024.0*1024.0), (size+t->size_current)/(1024.0*1024.0), t->size_limit/(1024.0*1024.0), rss/256.0);
	    //struct mallinfo m = mallinfo();
	    //printf(" arena=%d hblks=%d hblkhd=%d\n", m.arena, m.hblks, m.hblkhd);
	}
        /* Try to remove one. */
	PAIR remove_me;
	for (remove_me = ct->tail; remove_me; remove_me = remove_me->prev) {
	    if (remove_me->state == CTPAIR_IDLE && !rwlock_users(&remove_me->rwlock)) {
		flush_and_maybe_remove(ct, remove_me, TRUE);
		goto again;
	    }
	}
	/* All were pinned. */
	//printf("All are pinned\n");
	return 0; // Don't indicate an error code.  Instead let memory get overfull.
    }

    if ((4 * ct->n_in_table < ct->table_size) && ct->table_size > 4)
        cachetable_rehash(ct, ct->table_size/2);

    return r;
}

void toku_cachetable_maybe_flush_some(CACHETABLE ct) {
    cachetable_lock(ct);
    maybe_flush_some(ct, 0);
    cachetable_unlock(ct);
}

static PAIR cachetable_insert_at(CACHETABLE ct, 
                                 CACHEFILE cachefile, CACHEKEY key, void *value, 
                                 enum ctpair_state state,
                                 u_int32_t fullhash, 
                                 long size,
                                 CACHETABLE_FLUSH_CALLBACK flush_callback,
                                 CACHETABLE_FETCH_CALLBACK fetch_callback,
                                 void *extraargs, 
                                 enum cachetable_dirty dirty) {
    TAGMALLOC(PAIR, p);
    assert(p);
    memset(p, 0, sizeof *p);
    ctpair_add_ref(p);
    p->cachefile = cachefile;
    p->key = key;
    p->value = value;
    p->fullhash = fullhash;
    p->dirty = dirty;
    p->size = size;
    p->state = state;
    p->flush_callback = flush_callback;
    p->fetch_callback = fetch_callback;
    p->extraargs = extraargs;
    p->fullhash = fullhash;
    p->next = p->prev = 0;
    rwlock_init(&p->rwlock);
    p->cq = 0;
    lru_add_to_list(ct, p);
    u_int32_t h = fullhash & (ct->table_size-1);
    p->hash_chain = ct->table[h];
    ct->table[h] = p;
    ct->n_in_table++;
    ct->size_current += size;
    if (ct->n_in_table > ct->table_size) {
        cachetable_rehash(ct, ct->table_size*2);
    }
    return p;
}

enum { hash_histogram_max = 100 };
static unsigned long long hash_histogram[hash_histogram_max];
void toku_cachetable_print_hash_histogram (void) {
    int i;
    for (i=0; i<hash_histogram_max; i++)
	if (hash_histogram[i]) printf("%d:%llu ", i, hash_histogram[i]);
    printf("\n");
    printf("miss=%"PRIu64" hit=%"PRIu64" wait_reading=%"PRIu64" wait=%"PRIu64"\n", 
           cachetable_miss, cachetable_hit, cachetable_wait_reading, cachetable_wait_writing);
}

static void
note_hash_count (int count) {
    if (count>=hash_histogram_max) count=hash_histogram_max-1;
    hash_histogram[count]++;
}

int toku_cachetable_put(CACHEFILE cachefile, CACHEKEY key, u_int32_t fullhash, void*value, long size,
			CACHETABLE_FLUSH_CALLBACK flush_callback, 
                        CACHETABLE_FETCH_CALLBACK fetch_callback, void *extraargs) {
    WHEN_TRACE_CT(printf("%s:%d CT cachetable_put(%lld)=%p\n", __FILE__, __LINE__, key, value));
    CACHETABLE ct = cachefile->cachetable;
    int count=0;
    cachetable_lock(ct);
    cachetable_wait_write(ct);
    {
	PAIR p;
	for (p=ct->table[fullhash&(cachefile->cachetable->table_size-1)]; p; p=p->hash_chain) {
	    count++;
	    if (p->key.b==key.b && p->cachefile==cachefile) {
		// Semantically, these two asserts are not strictly right.  After all, when are two functions eq?
		// In practice, the functions better be the same.
		assert(p->flush_callback==flush_callback);
		assert(p->fetch_callback==fetch_callback);
                rwlock_read_lock(&p->rwlock, ct->mutex);
		note_hash_count(count);
                cachetable_unlock(ct);
		return -1; /* Already present. */
	    }
	}
    }
    int r;
    if ((r=maybe_flush_some(ct, size))) {
        cachetable_unlock(ct);
        return r;
    }
    // flushing could change the table size, but wont' change the fullhash
    cachetable_puts++;
    PAIR p = cachetable_insert_at(ct, cachefile, key, value, CTPAIR_IDLE, fullhash, size, flush_callback, fetch_callback, extraargs, CACHETABLE_DIRTY);
    assert(p);
    rwlock_read_lock(&p->rwlock, ct->mutex);
    note_hash_count(count);
    cachetable_unlock(ct);
    return 0;
}

static uint64_t get_tnow(void) {
    struct timeval tv;
    int r = gettimeofday(&tv, NULL); assert(r == 0);
    return tv.tv_sec * 1000000ULL + tv.tv_usec;
}

// for debug 
static PAIR write_for_checkpoint_pair = NULL;

// On entry: hold the ct lock
// On exit:  the node is written out
// Method:   take write lock
//           maybe write out the node
//           if p->cq, put on completion queue.  Else release write lock
static void
write_pair_for_checkpoint (CACHETABLE ct, PAIR p, BOOL write_if_dirty)
{
    if (p->dirty) {
        write_for_checkpoint_pair = p;
        rwlock_write_lock(&p->rwlock, ct->mutex); // grab an exclusive lock on the pair
        assert(p->state!=CTPAIR_WRITING);         // if we have the write lock, no one else should be writing out the node
        if (p->dirty && (write_if_dirty || p->checkpoint_pending)) {
            // this is essentially a flush_and_maybe_remove except that
            // we already have p->rwlock and we just do the write in our own thread.
            p->state = CTPAIR_WRITING; //most of this code should run only if NOT ALREADY CTPAIR_WRITING
            assert(ct->size_writing>=0);
            ct->size_writing += p->size;
            assert(ct->size_writing>=0);
            p->write_me = TRUE;
            p->remove_me = FALSE;
            workitem_init(&p->asyncwork, NULL, p);
            cachetable_write_pair(ct, p);    // releases the write lock on the pair
        }
        else if (p->cq) {
            assert(ct->size_writing>=0);
            ct->size_writing += p->size; //cachetable_complete_write_pair will reduce by p->size
            workitem_init(&p->asyncwork, NULL, p);
            workqueue_enq(p->cq, &p->asyncwork, 1);
        }
        else
            rwlock_write_unlock(&p->rwlock); // didn't call cachetable_write_pair so we have to unlock it ourselves.
        write_for_checkpoint_pair = NULL;
    }
}

// for debugging
// valid only if this function is called only by a single thread
static u_int64_t get_and_pin_footprint = 0;

static CACHEFILE get_and_pin_cachefile = NULL;
static CACHEKEY  get_and_pin_key       = {0};
static u_int32_t get_and_pin_fullhash  = 0;            


int toku_cachetable_get_and_pin(CACHEFILE cachefile, CACHEKEY key, u_int32_t fullhash, void**value, long *sizep,
			        CACHETABLE_FLUSH_CALLBACK flush_callback, 
                                CACHETABLE_FETCH_CALLBACK fetch_callback, void *extraargs) {
    CACHETABLE ct = cachefile->cachetable;
    PAIR p;
    int count=0;


    get_and_pin_footprint = 1;

    cachetable_lock(ct);
    get_and_pin_cachefile = cachefile;
    get_and_pin_key       = key;
    get_and_pin_fullhash  = fullhash;            
    
    get_and_pin_footprint = 2;
    cachetable_wait_write(ct);
    get_and_pin_footprint = 3;
    for (p=ct->table[fullhash&(ct->table_size-1)]; p; p=p->hash_chain) {
	count++;
	if (p->key.b==key.b && p->cachefile==cachefile) {
	    uint64_t t0 = 0;
	    int do_wait_time = 0;

            if (p->rwlock.writer || p->rwlock.want_write) {
                if (p->state == CTPAIR_READING)
                    cachetable_wait_reading++;
                else
                    cachetable_wait_writing++;
		do_wait_time = 1;
            } else if (p->checkpoint_pending) {
                do_wait_time = 1;
                cachetable_wait_checkpoint++;
            }
            if (do_wait_time)
	        t0 = get_tnow();

            if (p->checkpoint_pending) {
		get_and_pin_footprint = 4;		
		write_pair_for_checkpoint(ct, p, FALSE);
	    }
	    // still have the cachetable lock
	    // TODO: #1398  kill this hack before it multiplies further
	    // This logic here to prevent deadlock that results when a query pins a node,
	    // then the straddle callback creates a cursor that pins it again.  If 
	    // toku_cachetable_end_checkpoint() is called between those two calls to pin
	    // the node, then the checkpoint function waits for the first pin to be released
	    // while the callback waits for the checkpoint function to release the write
	    // lock.  The work-around is to have an unfair rwlock mechanism that favors the 
	    // reader.
#ifdef  BRT_LEVEL_STRADDLE_CALLBACK_LOGIC_NOT_READY
	    if (STRADDLE_HACK_INSIDE_CALLBACK) {
		get_and_pin_footprint = 6;
		rwlock_prefer_read_lock(&p->rwlock, ct->mutex);
	    }
	    else
#endif
		{
		    get_and_pin_footprint = 7;
		    rwlock_read_lock(&p->rwlock, ct->mutex);
		}
	    if (do_wait_time)
		cachetable_waittime += get_tnow() - t0;
	    get_and_pin_footprint = 8;
            if (p->state == CTPAIR_INVALID) {
		get_and_pin_footprint = 9;
                rwlock_read_unlock(&p->rwlock);
                if (rwlock_users(&p->rwlock) == 0)
                    ctpair_destroy(p);
                cachetable_unlock(ct);
		get_and_pin_footprint = 0;
                return ENODEV;
            }
	    lru_touch(ct,p);
	    *value = p->value;
            if (sizep) *sizep = p->size;
            cachetable_hit++;
	    note_hash_count(count);
            cachetable_unlock(ct);
	    WHEN_TRACE_CT(printf("%s:%d cachtable_get_and_pin(%lld)--> %p\n", __FILE__, __LINE__, key, *value));
	    get_and_pin_footprint = 0;
	    return 0;
	}
    }
    get_and_pin_footprint = 9;
    note_hash_count(count);
    int r;
    // Note.  hashit(t,key) may have changed as a result of flushing.  But fullhash won't have changed.
    {
	p = cachetable_insert_at(ct, cachefile, key, zero_value, CTPAIR_READING, fullhash, zero_size, flush_callback, fetch_callback, extraargs, CACHETABLE_CLEAN);
        assert(p);
	get_and_pin_footprint = 10;
        rwlock_write_lock(&p->rwlock, ct->mutex);
	uint64_t t0 = get_tnow();

        r = cachetable_fetch_pair(ct, cachefile, p);
        if (r) {
            cachetable_unlock(ct);
	    get_and_pin_footprint = 0;
            return r;
        }
        cachetable_miss++;
	cachetable_misstime += get_tnow() - t0;
	get_and_pin_footprint = 11;
        rwlock_read_lock(&p->rwlock, ct->mutex);
        assert(p->state == CTPAIR_IDLE);

	*value = p->value;
        if (sizep) *sizep = p->size;
    }
    get_and_pin_footprint = 12;
    r = maybe_flush_some(ct, 0);
    cachetable_unlock(ct);
    WHEN_TRACE_CT(printf("%s:%d did fetch: cachtable_get_and_pin(%lld)--> %p\n", __FILE__, __LINE__, key, *value));
    get_and_pin_footprint = 0;
    return r;
}

// Lookup a key in the cachetable.  If it is found and it is not being written, then
// acquire a read lock on the pair, update the LRU list, and return sucess.
//
// However, if the page is clean or has checkpoint pending, don't return success.
// This will minimize the number of dirty nodes.
// Rationale:  maybe_get_and_pin is used when the system has an alternative to modifying a node.
//  In the context of checkpointing, we don't want to gratuituously dirty a page, because it causes an I/O.
//  For example, imagine that we can modify a bit in a dirty parent, or modify a bit in a clean child, then we should modify
//  the dirty parent (which will have to do I/O eventually anyway) rather than incur a full block write to modify one bit.
//  Similarly, if the checkpoint is actually pending, we don't want to block on it.
int toku_cachetable_maybe_get_and_pin (CACHEFILE cachefile, CACHEKEY key, u_int32_t fullhash, void**value) {
    CACHETABLE ct = cachefile->cachetable;
    PAIR p;
    int count = 0;
    int r = -1;
    cachetable_lock(ct);
    cachetable_maybe_get_and_pins++;
    for (p=ct->table[fullhash&(ct->table_size-1)]; p; p=p->hash_chain) {
	count++;
	if (p->key.b==key.b && p->cachefile==cachefile) {
            if (p->state == CTPAIR_IDLE && //If not idle, will require a stall and/or will be clean once it is idle, or might be GONE once not idle
                !p->checkpoint_pending &&  //If checkpoint pending, we would need to first write it, which would make it clean
                p->dirty &&
                rwlock_try_prefer_read_lock(&p->rwlock, ct->mutex) == 0 //Grab read lock.  If any stall would be necessary that means it would be clean AFTER the stall, so don't even try to stall
            ) {
                cachetable_maybe_get_and_pin_hits++;
                *value = p->value;
                lru_touch(ct,p);
                r = 0;
                //printf("%s:%d cachetable_maybe_get_and_pin(%lld)--> %p\n", __FILE__, __LINE__, key, *value);
            }
            break;
	}
    }
    note_hash_count(count);
    cachetable_unlock(ct);
    return r;
}

//Used by shortcut query path.
//Same as toku_cachetable_maybe_get_and_pin except that we don't care if the node is clean or dirty (return the node regardless).
//All other conditions remain the same.
int toku_cachetable_maybe_get_and_pin_clean (CACHEFILE cachefile, CACHEKEY key, u_int32_t fullhash, void**value) {
    CACHETABLE ct = cachefile->cachetable;
    PAIR p;
    int count = 0;
    int r = -1;
    cachetable_lock(ct);
    cachetable_maybe_get_and_pins++;
    for (p=ct->table[fullhash&(ct->table_size-1)]; p; p=p->hash_chain) {
	count++;
	if (p->key.b==key.b && p->cachefile==cachefile) {
            if (p->state == CTPAIR_IDLE && //If not idle, will require a stall and/or might be GONE once not idle
                !p->checkpoint_pending &&  //If checkpoint pending, we would need to first write it, which would make it clean (if the pin would be used for writes.  If would be used for read-only we could return it, but that would increase complexity)
                rwlock_try_prefer_read_lock(&p->rwlock, ct->mutex) == 0 //Grab read lock only if no stall required
            ) {
                cachetable_maybe_get_and_pin_hits++;
                *value = p->value;
                lru_touch(ct,p);
                r = 0;
                //printf("%s:%d cachetable_maybe_get_and_pin_clean(%lld)--> %p\n", __FILE__, __LINE__, key, *value);
            }
            break;
	}
    }
    note_hash_count(count);
    cachetable_unlock(ct);
    return r;
}


int toku_cachetable_unpin(CACHEFILE cachefile, CACHEKEY key, u_int32_t fullhash, enum cachetable_dirty dirty, long size)
// size==0 means that the size didn't change.
{
    CACHETABLE ct = cachefile->cachetable;
    PAIR p;
    WHEN_TRACE_CT(printf("%s:%d unpin(%lld)", __FILE__, __LINE__, key));
    //printf("%s:%d is dirty now=%d\n", __FILE__, __LINE__, dirty);
    int count = 0;
    int r = -1;
    //assert(fullhash == toku_cachetable_hash(cachefile, key));
    cachetable_lock(ct);
    for (p=ct->table[fullhash&(ct->table_size-1)]; p; p=p->hash_chain) {
	count++;
	if (p->key.b==key.b && p->cachefile==cachefile) {
	    assert(rwlock_readers(&p->rwlock)>0);
            rwlock_read_unlock(&p->rwlock);
	    if (dirty) p->dirty = CACHETABLE_DIRTY;
            if (size != 0) {
                ct->size_current -= p->size; if (p->state == CTPAIR_WRITING) ct->size_writing -= p->size;
                p->size = size;
                ct->size_current += p->size; if (p->state == CTPAIR_WRITING) ct->size_writing += p->size;
            }
	    WHEN_TRACE_CT(printf("[count=%lld]\n", p->pinned));
	    {
		if ((r=maybe_flush_some(ct, 0))) {
                    cachetable_unlock(ct);
                    return r;
                }
	    }
            r = 0; // we found one
            break;
	}
    }
    note_hash_count(count);
    cachetable_unlock(ct);
    return r;
}

int toku_cachefile_prefetch(CACHEFILE cf, CACHEKEY key, u_int32_t fullhash,
                            CACHETABLE_FLUSH_CALLBACK flush_callback, 
                            CACHETABLE_FETCH_CALLBACK fetch_callback, 
                            void *extraargs) {
    if (0) printf("%s:%d %"PRId64"\n", __FUNCTION__, __LINE__, key.b);
    CACHETABLE ct = cf->cachetable;
    cachetable_lock(ct);
    // lookup
    PAIR p;
    for (p = ct->table[fullhash&(ct->table_size-1)]; p; p = p->hash_chain) {
	if (p->key.b==key.b && p->cachefile==cf) {
            //Maybe check for pending and do write_pair_for_checkpoint()?
            lru_touch(ct, p);
            break;
        }
    }

    // if not found then create a pair in the READING state and fetch it
    if (p == 0) {
        cachetable_prefetches++;
	p = cachetable_insert_at(ct, cf, key, zero_value, CTPAIR_READING, fullhash, zero_size, flush_callback, fetch_callback, extraargs, CACHETABLE_CLEAN);
        assert(p);
        rwlock_write_lock(&p->rwlock, ct->mutex);
#if DO_WORKER_THREAD
        workitem_init(&p->asyncwork, cachetable_reader, p);
        workqueue_enq(&ct->wq, &p->asyncwork, 0);
#else
        cachetable_fetch_pair(ct, cf, p);
#endif
    }
    cachetable_unlock(ct);
    return 0;
}

// effect:   Move an object from one key to another key.
// requires: The object is pinned in the table
int toku_cachetable_rename (CACHEFILE cachefile, CACHEKEY oldkey, CACHEKEY newkey) {
    CACHETABLE ct = cachefile->cachetable;
    PAIR *ptr_to_p,p;
    int count = 0;
    u_int32_t fullhash = toku_cachetable_hash(cachefile, oldkey);
    cachetable_lock(ct);
    for (ptr_to_p = &ct->table[fullhash&(ct->table_size-1)],  p = *ptr_to_p;
         p;
         ptr_to_p = &p->hash_chain,                p = *ptr_to_p) {
        count++;
        if (p->key.b==oldkey.b && p->cachefile==cachefile) {
            note_hash_count(count);
            *ptr_to_p = p->hash_chain;
            p->key = newkey;
            u_int32_t new_fullhash = toku_cachetable_hash(cachefile, newkey);
            u_int32_t nh = new_fullhash&(ct->table_size-1);
            p->fullhash = new_fullhash;
            p->hash_chain = ct->table[nh];
            ct->table[nh] = p;
            cachetable_unlock(ct);
            return 0;
        }
    }
    note_hash_count(count);
    cachetable_unlock(ct);
    return -1;
}

void toku_cachefile_verify (CACHEFILE cf) {
    toku_cachetable_verify(cf->cachetable);
}

void toku_cachetable_verify (CACHETABLE ct) {
    cachetable_lock(ct);

    // First clear all the verify flags by going through the hash chains
    {
	u_int32_t i;
	for (i=0; i<ct->table_size; i++) {
	    PAIR p;
	    for (p=ct->table[i]; p; p=p->hash_chain) {
		p->verify_flag=0;
	    }
	}
    }
    // Now go through the LRU chain, make sure everything in the LRU chain is hashed, and set the verify flag.
    {
	PAIR p;
	for (p=ct->head; p; p=p->next) {
	    assert(p->verify_flag==0);
	    PAIR p2;
	    u_int32_t fullhash = p->fullhash;
	    //assert(fullhash==toku_cachetable_hash(p->cachefile, p->key));
	    for (p2=ct->table[fullhash&(ct->table_size-1)]; p2; p2=p2->hash_chain) {
		if (p2==p) {
		    /* found it */
		    goto next;
		}
	    }
	    fprintf(stderr, "Something in the LRU chain is not hashed\n");
	    assert(0);
	next:
	    p->verify_flag = 1;
	}
    }
    // Now make sure everything in the hash chains has the verify_flag set to 1.
    {
	u_int32_t i;
	for (i=0; i<ct->table_size; i++) {
	    PAIR p;
	    for (p=ct->table[i]; p; p=p->hash_chain) {
		assert(p->verify_flag);
	    }
	}
    }

    cachetable_unlock(ct);
}

static void assert_cachefile_is_flushed_and_removed (CACHETABLE ct, CACHEFILE cf) {
    u_int32_t i;
    // Check it two ways
    // First way: Look through all the hash chains
    for (i=0; i<ct->table_size; i++) {
	PAIR p;
	for (p=ct->table[i]; p; p=p->hash_chain) {
	    assert(p->cachefile!=cf);
	}
    }
    // Second way: Look through the LRU list.
    {
	PAIR p;
	for (p=ct->head; p; p=p->next) {
	    assert(p->cachefile!=cf);
	}
    }
}

// Flush (write to disk) all of the pairs that belong to a cachefile (or all pairs if 
// the cachefile is NULL.
// Must be holding cachetable lock on entry.
static int cachetable_flush_cachefile(CACHETABLE ct, CACHEFILE cf) {
    unsigned nfound = 0;
    struct workqueue cq;
    workqueue_init(&cq);

    // find all of the pairs owned by a cachefile and redirect their completion
    // to a completion queue.  flush and remove pairs in the IDLE state if they
    // are dirty.  pairs in the READING or WRITING states are already in the
    // work queue.
    unsigned i;

    unsigned num_pairs = 0;
    unsigned list_size = 256;
    PAIR *list = NULL;
    XMALLOC_N(list_size, list);
    //It is not safe to loop through the table (and hash chains) if you can
    //release the cachetable lock at any point within.

    //Make a list of pairs that belong to this cachefile.
    //Add a reference to them.
    for (i=0; i < ct->table_size; i++) {
	PAIR p;
	for (p = ct->table[i]; p; p = p->hash_chain) {
 	    if (cf == 0 || p->cachefile==cf) {
                ctpair_add_ref(p);
                list[num_pairs] = p;
                num_pairs++;
                if (num_pairs == list_size) {
                    list_size *= 2;
                    XREALLOC_N(list_size, list);
                }
            }
	}
    }
    //Loop through the list.
    //It is safe to access the memory (will not have been freed).
    //If 'already_removed' is set, then we should release our reference
    //and go to the next entry.
    for (i=0; i < num_pairs; i++) {
	PAIR p = list[i];
        if (!p->already_removed) {
            assert(cf == 0 || p->cachefile==cf);
            nfound++;
            p->cq = &cq;
            if (p->state == CTPAIR_IDLE)
                flush_and_maybe_remove(ct, p, TRUE);
        }
        ctpair_destroy(p);     //Release our reference
    }
    toku_free(list);

    // wait for all of the pairs in the work queue to complete
    for (i=0; i<nfound; i++) {
        cachetable_unlock(ct);
        WORKITEM wi = 0;
        //This workqueue's mutex is NOT the cachetable lock.
        //You must not be holding the cachetable lock during the dequeue.
        int r = workqueue_deq(&cq, &wi, 1); assert(r == 0);
        cachetable_lock(ct);
        PAIR p = workitem_arg(wi);
        p->cq = 0;
        if (p->state == CTPAIR_READING) { //Some other thread owned the lock, but transferred ownership to the thread executing this function
            rwlock_write_unlock(&p->rwlock);  //Release the lock
                                              //No one has a pin.  This was being read because of a prefetch.
            cachetable_maybe_remove_and_free_pair(ct, p);
        } else if (p->state == CTPAIR_WRITING) { //Some other thread (or this thread) owned the lock and transferred ownership to the thread executing this function
                                                 //No one has a pin.  This was written because of an eviction.
            cachetable_complete_write_pair(ct, p, TRUE);
        } else if (p->state == CTPAIR_INVALID) {
            abort_fetch_pair(p);
        } else
            assert(0);
    }
    workqueue_destroy(&cq);
    assert_cachefile_is_flushed_and_removed(ct, cf);

    if ((4 * ct->n_in_table < ct->table_size) && (ct->table_size>4))
        cachetable_rehash(ct, ct->table_size/2);

    return 0;
}

/* Requires that no locks be held that are used by the checkpoint logic (ydb, etc.) */
void
toku_cachetable_minicron_shutdown(CACHETABLE ct) {
    int  r = toku_minicron_shutdown(&ct->checkpointer);
    assert(r==0);
}

/* Require that it all be flushed. */
int 
toku_cachetable_close (CACHETABLE *ctp) {
    CACHETABLE ct=*ctp;
    if (!toku_minicron_has_been_shutdown(&ct->checkpointer)) {
	// for test code only, production code uses toku_cachetable_minicron_shutdown()
	int  r = toku_minicron_shutdown(&ct->checkpointer);
	assert(r==0);
    }
    int r;
    cachetable_lock(ct);
    if ((r=cachetable_flush_cachefile(ct, 0))) {
        cachetable_unlock(ct);
        return r;
    }
    u_int32_t i;
    for (i=0; i<ct->table_size; i++) {
	if (ct->table[i]) return -1;
    }
    assert(ct->size_writing == 0);
    rwlock_destroy(&ct->pending_lock);
    r = toku_pthread_mutex_destroy(&ct->openfd_mutex); assert(r == 0);
    cachetable_unlock(ct);
    toku_destroy_workers(&ct->wq, &ct->threadpool);
    r = toku_leaflock_destroy(&ct->leaflock_pool); assert(r==0);
    toku_omt_destroy(&ct->reserved_filenums);
    r = toku_pthread_mutex_destroy(&ct->cachefiles_mutex); assert(r == 0);
    toku_free(ct->table);
    toku_free(ct->env_dir);
    toku_free(ct);
    *ctp = 0;
    return 0;
}

void toku_cachetable_get_miss_times(CACHETABLE UU(ct), uint64_t *misscount, uint64_t *misstime) {
    if (misscount) 
        *misscount = cachetable_miss;
    if (misstime) 
        *misstime = cachetable_misstime;
}

int toku_cachetable_unpin_and_remove (CACHEFILE cachefile, CACHEKEY key) {
    int r = ENOENT;
    // Removing something already present is OK.
    CACHETABLE ct = cachefile->cachetable;
    PAIR p;
    int count = 0;
    cachetable_lock(ct);
    u_int32_t fullhash = toku_cachetable_hash(cachefile, key);
    for (p=ct->table[fullhash&(ct->table_size-1)]; p; p=p->hash_chain) {
        count++;
	if (p->key.b==key.b && p->cachefile==cachefile) {
	    p->dirty = CACHETABLE_CLEAN; // clear the dirty bit.  We're just supposed to remove it.
	    assert(rwlock_readers(&p->rwlock)==1);
	    assert(rwlock_blocked_readers(&p->rwlock)==0);
            rwlock_read_unlock(&p->rwlock);
            if (rwlock_blocked_writers(&p->rwlock)>0) {
                struct workqueue cq;
                workqueue_init(&cq);
                while (rwlock_blocked_writers(&p->rwlock)>0) {
                    //Someone (one or more checkpoint threads) is waiting for a write lock
                    //on this pair.
                    //They are still blocked because we have not released the
                    //cachetable lock.
                    //If we freed the memory for the pair we would have dangling
                    //pointers.  We need to let the checkpoint thread finish up with
                    //this pair.

                    p->cq = &cq;

                    //  If anyone is waiting on write lock, let them finish.
                    cachetable_unlock(ct);

                    WORKITEM wi = NULL;
                    r = workqueue_deq(&cq, &wi, 1);
                    //Writer is now done.
                    assert(r == 0);
                    PAIR pp = workitem_arg(wi);
                    assert(pp == p);

                    //We are holding the write lock on the pair
                    cachetable_lock(ct);
                    assert(rwlock_writers(&p->rwlock) == 1);
                    assert(rwlock_readers(&p->rwlock) == 0);
                    assert(rwlock_blocked_readers(&p->rwlock) == 0);
                    cachetable_complete_write_pair(ct, p, TRUE);
                }
                workqueue_destroy(&cq);
            }
            else {
                //Remove pair.
                cachetable_maybe_remove_and_free_pair(ct, p);
            }
            r = 0;
	    goto done;
	}
    }
 done:
    note_hash_count(count);
    cachetable_unlock(ct);
    return r;
}

static int
set_filenum_in_array(OMTVALUE brtv, u_int32_t index, void*arrayv) {
    FILENUM *array = arrayv;
    BRT brt = brtv;
    array[index] = toku_cachefile_filenum(brt->cf);
    return 0;
}

static int
log_open_txn (OMTVALUE txnv, u_int32_t UU(index), void *UU(extra)) {
    TOKUTXN    txn    = txnv;
    TOKULOGGER logger = txn->logger;
    FILENUMS open_filenums;
    uint32_t num_filenums = toku_omt_size(txn->open_brts);
    FILENUM array[num_filenums];
    {
        open_filenums.num      = num_filenums;
        open_filenums.filenums = array;
        //Fill in open_filenums
        int r = toku_omt_iterate(txn->open_brts, set_filenum_in_array, array);
        assert(r==0);
    }
    int r = toku_log_xstillopen(logger, NULL, 0,
                                toku_txn_get_txnid(txn),
                                toku_txn_get_txnid(toku_logger_txn_parent(txn)),
                                txn->rollentry_raw_count,
                                open_filenums,
                                txn->force_fsync_on_commit,
                                txn->num_rollback_nodes,
                                txn->num_rollentries,
                                txn->spilled_rollback_head,
                                txn->spilled_rollback_tail,
                                txn->current_rollback);
    assert(r==0);
    return 0;
}

static int
unpin_rollback_log_for_checkpoint (OMTVALUE txnv, u_int32_t UU(index), void *UU(extra)) {
    int r = 0;
    TOKUTXN    txn    = txnv;
    if (txn->pinned_inprogress_rollback_log) {
        r = toku_rollback_log_unpin(txn, txn->pinned_inprogress_rollback_log);
        assert(r==0);
    }
    return r;
}

// TODO: #1510 locking of cachetable is suspect
//             verify correct algorithm overall

int 
toku_cachetable_begin_checkpoint (CACHETABLE ct, TOKULOGGER logger) {
    // Requires:   All three checkpoint-relevant locks must be held (see checkpoint.c).
    // Algorithm:  Write a checkpoint record to the log, noting the LSN of that record.
    //             Use the begin_checkpoint callback to take necessary snapshots (header, btt)
    //             Mark every dirty node as "pending."  ("Pending" means that the node must be
    //                                                    written to disk before it can be modified.)

    {
        unsigned i;
	if (logger) { // Unpin all 'inprogress rollback log nodes' pinned by transactions
            int r = toku_omt_iterate(logger->live_txns,
                                     unpin_rollback_log_for_checkpoint,
                                     NULL);
            assert(r==0);
        }
	cachetable_lock(ct);
	//Initialize accountability counters
	ct->checkpoint_num_files = 0;
	ct->checkpoint_num_txns  = 0;

        //Make list of cachefiles to be included in checkpoint.
        //If refcount is 0, the cachefile is closing (performing a local checkpoint)
        {
            CACHEFILE cf;
            assert(ct->cachefiles_in_checkpoint==NULL);
            cachefiles_lock(ct);
            for (cf = ct->cachefiles; cf; cf=cf->next) {
                assert(!cf->is_closing); //Closing requires ydb lock (or in checkpoint).  Cannot happen.
                assert(cf->refcount>0);  //Must have a reference if not closing.
                //Incremement reference count of cachefile because we're using it for the checkpoint.
                //This will prevent closing during the checkpoint.
                int r = cf->note_pin_by_checkpoint(cf, cf->userdata);
                assert(r==0);
                cf->next_in_checkpoint       = ct->cachefiles_in_checkpoint;
                ct->cachefiles_in_checkpoint = cf;
                cf->for_checkpoint           = TRUE;
            }
            cachefiles_unlock(ct);
        }

	if (logger) {
	    // The checkpoint must be performed after the lock is acquired.
	    {
		LSN begin_lsn={.lsn=-1}; // we'll need to store the lsn of the checkpoint begin in all the trees that are checkpointed.
		int r = toku_log_begin_checkpoint(logger, &begin_lsn, 0, 0);
		assert(r==0);
		ct->lsn_of_checkpoint_in_progress = begin_lsn;
	    }
	    // Log all the open files
	    {
                //Must loop through ALL open files (even if not included in checkpoint).
		CACHEFILE cf;
                cachefiles_lock(ct);
		for (cf = ct->cachefiles; cf; cf=cf->next) {
                    if (cf->log_fassociate_during_checkpoint) {
                        int r = cf->log_fassociate_during_checkpoint(cf, cf->userdata);
			ct->checkpoint_num_files++;
                        assert(r==0);
                    }
		}
                cachefiles_unlock(ct);
	    }
	    // Log all the open transactions MUST BE AFTER OPEN FILES
	    {
                ct->checkpoint_num_txns = toku_omt_size(logger->live_txns);
                int r = toku_omt_iterate(logger->live_txns, log_open_txn, NULL);
		assert(r==0);
	    }
	    // Log rollback suppression for all the open files MUST BE AFTER TXNS
	    {
                //Must loop through ALL open files (even if not included in checkpoint).
		CACHEFILE cf;
                cachefiles_lock(ct);
		for (cf = ct->cachefiles; cf; cf=cf->next) {
                    if (cf->log_suppress_rollback_during_checkpoint) {
                        int r = cf->log_suppress_rollback_during_checkpoint(cf, cf->userdata);
                        assert(r==0);
                    }
		}
                cachefiles_unlock(ct);
	    }
	}

        unsigned int npending = 0;
        rwlock_write_lock(&ct->pending_lock, ct->mutex);
        for (i=0; i < ct->table_size; i++) {
            PAIR p;
            for (p = ct->table[i]; p; p=p->hash_chain) {
		assert(!p->checkpoint_pending);
                //Only include pairs belonging to cachefiles in the checkpoint
                if (!p->cachefile->for_checkpoint) continue;
                if (p->state == CTPAIR_READING)
                    continue;   // skip pairs being read as they will be clean
                else if (p->state == CTPAIR_IDLE || p->state == CTPAIR_WRITING) {
		    if (p->dirty) {
			p->checkpoint_pending = TRUE;
                        if (ct->pending_head)
                            ct->pending_head->pending_prev = p;
                        p->pending_next                = ct->pending_head;
                        p->pending_prev                = NULL;
                        ct->pending_head               = p;
                        npending++;
                    }
                } else
                    assert(0);
            }
        }
        rwlock_write_unlock(&ct->pending_lock);
        if (0) fprintf(stderr, "%s:%d %u %u\n", __FUNCTION__, __LINE__, npending, ct->n_in_table);

        //begin_checkpoint_userdata must be called AFTER all the pairs are marked as pending.
        //Once marked as pending, we own write locks on the pairs, which means the writer threads can't conflict.
	{
	    CACHEFILE cf;
            cachefiles_lock(ct);
	    for (cf = ct->cachefiles_in_checkpoint; cf; cf=cf->next_in_checkpoint) {
		if (cf->begin_checkpoint_userdata) {
                    rwlock_prefer_read_lock(&cf->fdlock, ct->mutex);
                    rwlock_write_lock(&cf->checkpoint_lock, ct->mutex);
                    assert(cf->checkpoint_state == CS_NOT_IN_PROGRESS);
		    int r = cf->begin_checkpoint_userdata(cf, cf->fd, ct->lsn_of_checkpoint_in_progress, cf->userdata);
		    assert(r==0);
                    cf->checkpoint_state = CS_CALLED_BEGIN_CHECKPOINT;
                    rwlock_write_unlock(&cf->checkpoint_lock);
                    rwlock_read_unlock(&cf->fdlock);
		}
	    }
            cachefiles_unlock(ct);
	}

	cachetable_unlock(ct);
    }
    return 0;
}


// This is used by the cachetable_race test.  
extern int get_toku_checkpointing_user_data_status(void) __attribute__((__visibility__("default")));
static volatile int toku_checkpointing_user_data_status = 0;
static void set_toku_checkpointing_user_data_status (int v) {
    toku_checkpointing_user_data_status = v;
}
int get_toku_checkpointing_user_data_status (void) {
    return toku_checkpointing_user_data_status;
}

int
toku_cachetable_end_checkpoint(CACHETABLE ct, TOKULOGGER logger,
                               void (*ydb_lock)(void), void (*ydb_unlock)(void),
                               void (*testcallback_f)(void*),  void * testextra) {
    // Requires:   The big checkpoint lock must be held (see checkpoint.c).
    // Algorithm:  Write all pending nodes to disk
    //             Use checkpoint callback to write snapshot information to disk (header, btt)
    //             Use end_checkpoint callback to fsync dictionary and log, and to free unused blocks
    // Note:       If testcallback is null (for testing purposes only), call it after writing dictionary but before writing log

    int retval = 0;
    cachetable_lock(ct);
    {
        // 
        // #TODO: #1424 Long-lived get and pin (held by cursor) will cause a deadlock here.
        //        Need some solution (possibly modify requirement for write lock or something else).
	PAIR p;
	while ((p = ct->pending_head)!=0) {
            ct->pending_head = ct->pending_head->pending_next;
            pending_pairs_remove(ct, p);
	    write_pair_for_checkpoint(ct, p, FALSE); // if still pending, clear the pending bit and write out the node
	    // Don't need to unlock and lock cachetable, because the cachetable was unlocked and locked while the flush callback ran.
	}
    }
    assert(!ct->pending_head);


    {   // have just written data blocks, so next write the translation and header for each open dictionary
	CACHEFILE cf;
        //cachefiles_in_checkpoint is protected by the checkpoint_safe_lock
	for (cf = ct->cachefiles_in_checkpoint; cf; cf=cf->next_in_checkpoint) {
	    if (cf->checkpoint_userdata) {
                rwlock_prefer_read_lock(&cf->fdlock, ct->mutex);
                rwlock_write_lock(&cf->checkpoint_lock, ct->mutex);
                if (!logger || ct->lsn_of_checkpoint_in_progress.lsn != cf->most_recent_global_checkpoint_that_finished_early.lsn) {
                    assert(ct->lsn_of_checkpoint_in_progress.lsn >= cf->most_recent_global_checkpoint_that_finished_early.lsn);
                    cachetable_unlock(ct);
                    assert(cf->checkpoint_state == CS_CALLED_BEGIN_CHECKPOINT);
		    set_toku_checkpointing_user_data_status(1);
                    int r = cf->checkpoint_userdata(cf, cf->fd, cf->userdata);
		    set_toku_checkpointing_user_data_status(0);
                    assert(r==0);
                    cf->checkpoint_state = CS_CALLED_CHECKPOINT;
                    cachetable_lock(ct);
                }
                else {
                    assert(cf->checkpoint_state == CS_NOT_IN_PROGRESS);
                }
                rwlock_write_unlock(&cf->checkpoint_lock);
                rwlock_read_unlock(&cf->fdlock);
	    }
	}
    }

    {   // everything has been written to file (or at least OS internal buffer)...
	// ... so fsync and call checkpoint-end function in block translator
	//     to free obsolete blocks on disk used by previous checkpoint
	CACHEFILE cf;
        //cachefiles_in_checkpoint is protected by the checkpoint_safe_lock
	for (cf = ct->cachefiles_in_checkpoint; cf; cf=cf->next_in_checkpoint) {
	    if (cf->end_checkpoint_userdata) {
                rwlock_prefer_read_lock(&cf->fdlock, ct->mutex);
                rwlock_write_lock(&cf->checkpoint_lock, ct->mutex);
                if (!logger || ct->lsn_of_checkpoint_in_progress.lsn != cf->most_recent_global_checkpoint_that_finished_early.lsn) {
                    assert(ct->lsn_of_checkpoint_in_progress.lsn >= cf->most_recent_global_checkpoint_that_finished_early.lsn);
                    cachetable_unlock(ct);
                    //end_checkpoint fsyncs the fd, which needs the fdlock
                    assert(cf->checkpoint_state == CS_CALLED_CHECKPOINT);
                    int r = cf->end_checkpoint_userdata(cf, cf->fd, cf->userdata);
                    assert(r==0);
                    cf->checkpoint_state = CS_NOT_IN_PROGRESS;
                    cachetable_lock(ct);
                }
                assert(cf->checkpoint_state == CS_NOT_IN_PROGRESS);
                rwlock_write_unlock(&cf->checkpoint_lock);
                rwlock_read_unlock(&cf->fdlock);
	    }
	}
    }
    cachetable_unlock(ct);

    {
        //Delete list of cachefiles in the checkpoint,
        //remove reference
        //clear bit saying they're in checkpoint
	CACHEFILE cf;
        //cachefiles_in_checkpoint is protected by the checkpoint_safe_lock
        while ((cf = ct->cachefiles_in_checkpoint)) {
            ct->cachefiles_in_checkpoint = cf->next_in_checkpoint; 
            cf->next_in_checkpoint       = NULL;
            cf->for_checkpoint           = FALSE;
            ydb_lock();
            int r = cf->note_unpin_by_checkpoint(cf, cf->userdata);
            ydb_unlock();
            if (r!=0) {
                retval = r;
                goto panic;
            }
        }
    }

    // For testing purposes only.  Dictionary has been fsync-ed to disk but log has not yet been written.
    if (testcallback_f) 
	testcallback_f(testextra);      

    if (logger) {
	int r = toku_log_end_checkpoint(logger, NULL,
					1, // want the end_checkpoint to be fsync'd
					ct->lsn_of_checkpoint_in_progress.lsn, 
					0,
					ct->checkpoint_num_files,
					ct->checkpoint_num_txns);
	assert(r==0);
	toku_logger_note_checkpoint(logger, ct->lsn_of_checkpoint_in_progress);
    }
    
panic:
    return retval;
}

TOKULOGGER toku_cachefile_logger (CACHEFILE cf) {
    return cf->cachetable->logger;
}

FILENUM toku_cachefile_filenum (CACHEFILE cf) {
    return cf->filenum;
}

#if DO_WORKER_THREAD

// Worker thread function to write a pair from memory to its cachefile
static void cachetable_writer(WORKITEM wi) {
    PAIR p = workitem_arg(wi);
    CACHETABLE ct = p->cachefile->cachetable;
    cachetable_lock(ct);
    cachetable_write_pair(ct, p);
    cachetable_unlock(ct);
}

// Worker thread function to read a pair from a cachefile to memory
static void cachetable_reader(WORKITEM wi) {
    PAIR p = workitem_arg(wi);
    CACHETABLE ct = p->cachefile->cachetable;
    cachetable_lock(ct);
    int r = cachetable_fetch_pair(ct, p->cachefile, p);
#define DO_FLUSH_FROM_READER 0
    if (r == 0) {
#if DO_FLUSH_FROM_READER
        maybe_flush_some(ct, 0);
#else
        r = r;
#endif
    }
    cachetable_unlock(ct);
}

#endif

// debug functions

int toku_cachetable_assert_all_unpinned (CACHETABLE ct) {
    u_int32_t i;
    int some_pinned=0;
    cachetable_lock(ct);
    for (i=0; i<ct->table_size; i++) {
	PAIR p;
	for (p=ct->table[i]; p; p=p->hash_chain) {
	    assert(rwlock_readers(&p->rwlock)>=0);
	    if (rwlock_readers(&p->rwlock)) {
		//printf("%s:%d pinned: %"PRId64" (%p)\n", __FILE__, __LINE__, p->key.b, p->value);
		some_pinned=1;
	    }
	}
    }
    cachetable_unlock(ct);
    return some_pinned;
}

int toku_cachefile_count_pinned (CACHEFILE cf, int print_them) {
    u_int32_t i;
    int n_pinned=0;
    CACHETABLE ct = cf->cachetable;
    cachetable_lock(ct);
    for (i=0; i<ct->table_size; i++) {
	PAIR p;
	for (p=ct->table[i]; p; p=p->hash_chain) {
	    assert(rwlock_readers(&p->rwlock)>=0);
	    if (rwlock_readers(&p->rwlock) && (cf==0 || p->cachefile==cf)) {
		if (print_them) printf("%s:%d pinned: %"PRId64" (%p)\n", __FILE__, __LINE__, p->key.b, p->value);
		n_pinned++;
	    }
	}
    }
    cachetable_unlock(ct);
    return n_pinned;
}

void toku_cachetable_print_state (CACHETABLE ct) {
    u_int32_t i;
    cachetable_lock(ct);
    for (i=0; i<ct->table_size; i++) {
        PAIR p = ct->table[i];
        if (p != 0) {
            printf("t[%u]=", i);
            for (p=ct->table[i]; p; p=p->hash_chain) {
                printf(" {%"PRId64", %p, dirty=%d, pin=%d, size=%ld}", p->key.b, p->cachefile, (int) p->dirty, rwlock_readers(&p->rwlock), p->size);
            }
            printf("\n");
        }
    }
    cachetable_unlock(ct);
}

void toku_cachetable_get_state (CACHETABLE ct, int *num_entries_ptr, int *hash_size_ptr, long *size_current_ptr, long *size_limit_ptr) {
    cachetable_lock(ct);
    if (num_entries_ptr) 
        *num_entries_ptr = ct->n_in_table;
    if (hash_size_ptr)
        *hash_size_ptr = ct->table_size;
    if (size_current_ptr)
        *size_current_ptr = ct->size_current;
    if (size_limit_ptr)
        *size_limit_ptr = ct->size_limit;
    cachetable_unlock(ct);
}

int toku_cachetable_get_key_state (CACHETABLE ct, CACHEKEY key, CACHEFILE cf, void **value_ptr,
				   int *dirty_ptr, long long *pin_ptr, long *size_ptr) {
    PAIR p;
    int count = 0;
    int r = -1;
    u_int32_t fullhash = toku_cachetable_hash(cf, key);
    cachetable_lock(ct);
    for (p = ct->table[fullhash&(ct->table_size-1)]; p; p = p->hash_chain) {
	count++;
        if (p->key.b == key.b && p->cachefile == cf) {
	    note_hash_count(count);
            if (value_ptr)
                *value_ptr = p->value;
            if (dirty_ptr)
                *dirty_ptr = p->dirty;
            if (pin_ptr)
                *pin_ptr = rwlock_readers(&p->rwlock);
            if (size_ptr)
                *size_ptr = p->size;
            r = 0;
            break;
        }
    }
    note_hash_count(count);
    cachetable_unlock(ct);
    return r;
}

void
toku_cachefile_set_userdata (CACHEFILE cf,
			     void *userdata,
                             int (*log_fassociate_during_checkpoint)(CACHEFILE, void*),
                             int (*log_suppress_rollback_during_checkpoint)(CACHEFILE, void*),
			     int (*close_userdata)(CACHEFILE, int, void*, char**, BOOL, LSN),
			     int (*checkpoint_userdata)(CACHEFILE, int, void*),
			     int (*begin_checkpoint_userdata)(CACHEFILE, int, LSN, void*),
                             int (*end_checkpoint_userdata)(CACHEFILE, int, void*),
                             int (*note_pin_by_checkpoint)(CACHEFILE, void*),
                             int (*note_unpin_by_checkpoint)(CACHEFILE, void*)) {
    cf->userdata = userdata;
    cf->log_fassociate_during_checkpoint = log_fassociate_during_checkpoint;
    cf->log_suppress_rollback_during_checkpoint = log_suppress_rollback_during_checkpoint;
    cf->close_userdata = close_userdata;
    cf->checkpoint_userdata = checkpoint_userdata;
    cf->begin_checkpoint_userdata = begin_checkpoint_userdata;
    cf->end_checkpoint_userdata = end_checkpoint_userdata;
    cf->note_pin_by_checkpoint = note_pin_by_checkpoint;
    cf->note_unpin_by_checkpoint = note_unpin_by_checkpoint;
}

void *toku_cachefile_get_userdata(CACHEFILE cf) {
    return cf->userdata;
}

CACHETABLE
toku_cachefile_get_cachetable(CACHEFILE cf) {
    return cf->cachetable;
}

//Only called by toku_brtheader_end_checkpoint
//Must have access to cf->fd (must be protected)
int
toku_cachefile_fsync(CACHEFILE cf) {
    int r;
    if (toku_cachefile_is_dev_null_unlocked(cf)) 
        r = 0; //Don't fsync /dev/null
    else 
        r = toku_file_fsync(cf->fd);
    return r;
}

int toku_cachefile_redirect_nullfd (CACHEFILE cf) {
    int null_fd;
    struct fileid fileid;

    CACHETABLE ct = cf->cachetable;
    cachetable_lock(ct);
    rwlock_write_lock(&cf->fdlock, ct->mutex);
    null_fd = open(DEV_NULL_FILE, O_WRONLY+O_BINARY);           
    assert(null_fd>=0);
    int r = toku_os_get_unique_file_id(null_fd, &fileid);
    assert(r==0);
    close(cf->fd);  // no change for t:2444
    cf->fd = null_fd;
    char *saved_fname_in_env = cf->fname_in_env;
    cf->fname_in_env = NULL;
    cachefile_init_filenum(cf, null_fd, saved_fname_in_env, fileid);
    if (saved_fname_in_env) toku_free(saved_fname_in_env);
    cf->is_dev_null = TRUE;
    rwlock_write_unlock(&cf->fdlock);
    cachetable_unlock(ct);
    return 0;
}

u_int64_t
toku_cachefile_size_in_memory(CACHEFILE cf)
{
    u_int64_t result=0;
    CACHETABLE ct=cf->cachetable;
    unsigned long i;
    for (i=0; i<ct->table_size; i++) {
	PAIR p;
	for (p=ct->table[i]; p; p=p->hash_chain) {
	    if (p->cachefile==cf) {
		result += p->size;
	    }
	}
    }
    return result;
}

void toku_cachetable_get_status(CACHETABLE ct, CACHETABLE_STATUS s) {
    s->lock_taken    = cachetable_lock_taken;
    s->lock_released = cachetable_lock_released;
    s->hit          = cachetable_hit;
    s->miss         = cachetable_miss;
    s->misstime     = cachetable_misstime;
    s->waittime     = cachetable_waittime;
    s->wait_reading = cachetable_wait_reading;
    s->wait_writing = cachetable_wait_writing;
    s->wait_checkpoint = cachetable_wait_checkpoint;
    s->puts         = cachetable_puts;
    s->prefetches   = cachetable_prefetches;
    s->maybe_get_and_pins      = cachetable_maybe_get_and_pins;
    s->maybe_get_and_pin_hits  = cachetable_maybe_get_and_pin_hits;
    s->size_current = ct->size_current;          
    s->size_limit   = ct->size_limit;            
    s->size_writing = ct->size_writing;          
    s->get_and_pin_footprint = get_and_pin_footprint;
    s->local_checkpoint      = local_checkpoint;
    s->local_checkpoint_files = local_checkpoint_files;
    s->local_checkpoint_during_checkpoint = local_checkpoint_during_checkpoint;
}

char *
toku_construct_full_name(int count, ...) {
    va_list ap;
    char *name = NULL;
    size_t n = 0;
    int i;
    va_start(ap, count);
    for (i=0; i<count; i++) {
        char *arg = va_arg(ap, char *);
        if (arg) {
            n += 1 + strlen(arg) + 1;
            char *newname = toku_xmalloc(n);
            if (name && !toku_os_is_absolute_name(arg))
                snprintf(newname, n, "%s/%s", name, arg);
            else
                snprintf(newname, n, "%s", arg);
            toku_free(name);
            name = newname;
        }
    }
    va_end(ap);

    return name;
}

char *
toku_cachetable_get_fname_in_cwd(CACHETABLE ct, const char * fname_in_env) {
    return toku_construct_full_name(2, ct->env_dir, fname_in_env);
}


// Returns the limit on the size of the cache table
uint64_t toku_cachetable_get_size_limit(CACHETABLE ct) {
    return ct->size_limit;
}

int 
toku_cachetable_local_checkpoint_for_commit (CACHETABLE ct, TOKUTXN txn, uint32_t n, CACHEFILE cachefiles[n]) {
    cachetable_lock(ct);
    local_checkpoint++;
    local_checkpoint_files += n;

    LSN begin_checkpoint_lsn = ZERO_LSN;
    uint32_t i;
    TOKULOGGER logger = txn->logger; 
    CACHEFILE cf;
    assert(logger); //Need transaction, so there must be a logger
    {
        int r = toku_log_local_txn_checkpoint(logger, &begin_checkpoint_lsn, 0, txn->txnid64);
        assert(r==0);
    }
    for (i = 0; i < n; i++) {
        cf = cachefiles[i];
        assert(cf->for_local_checkpoint.lsn == ZERO_LSN.lsn);
        cf->for_local_checkpoint = begin_checkpoint_lsn;
    }

    //Write out all dirty pairs.
    {
        uint32_t num_pairs = 0;
        uint32_t list_size = 256;
        PAIR *list = NULL;
        XMALLOC_N(list_size, list);
        PAIR p;

        //TODO: Determine if we can get rid of this use of pending_lock
        rwlock_write_lock(&ct->pending_lock, ct->mutex);
        for (i=0; i < ct->table_size; i++) {
            for (p = ct->table[i]; p; p=p->hash_chain) {
                //Only include pairs belonging to cachefiles in the checkpoint
                if (p->cachefile->for_local_checkpoint.lsn != begin_checkpoint_lsn.lsn) continue;
                if (p->state == CTPAIR_READING)
                    continue;   // skip pairs being read as they will be clean
                else if (p->state == CTPAIR_IDLE || p->state == CTPAIR_WRITING) {
                    if (p->dirty) {
                        ctpair_add_ref(p);
                        list[num_pairs] = p;
                        num_pairs++;
                        if (num_pairs == list_size) {
                            list_size *= 2;
                            XREALLOC_N(list_size, list);
                        }
                    }
                } else
                    assert(0);
            }
        }
        rwlock_write_unlock(&ct->pending_lock);

        for (i = 0; i < num_pairs; i++) {
            p = list[i];
            if (!p->already_removed) {
                write_pair_for_checkpoint(ct, p, TRUE);
            }
            ctpair_destroy(p);     //Release our reference
            // Don't need to unlock and lock cachetable,
            // because the cachetable was unlocked and locked while the flush callback ran.
        }
        toku_free(list);
    }

    for (i = 0; i < n; i++) {
        int r;
        cf = cachefiles[i];
        rwlock_prefer_read_lock(&cf->fdlock, ct->mutex);
        rwlock_write_lock(&cf->checkpoint_lock, ct->mutex);
        BOOL own_cachetable_lock = TRUE;
        switch (cf->checkpoint_state) {
        case CS_NOT_IN_PROGRESS:
            break;
        case CS_CALLED_BEGIN_CHECKPOINT:
            cachetable_unlock(ct);
            own_cachetable_lock = FALSE;
            assert(cf->checkpoint_state == CS_CALLED_BEGIN_CHECKPOINT);
            r = cf->checkpoint_userdata(cf, cf->fd, cf->userdata);
            assert(r==0);
            cf->checkpoint_state = CS_CALLED_CHECKPOINT;
            //FALL THROUGH ON PURPOSE.
        case CS_CALLED_CHECKPOINT:
            if (own_cachetable_lock)
                cachetable_unlock(ct);
            //end_checkpoint fsyncs the fd, which needs the fdlock
            assert(cf->checkpoint_state == CS_CALLED_CHECKPOINT);
            r = cf->end_checkpoint_userdata(cf, cf->fd, cf->userdata);
            assert(r==0);
            cf->checkpoint_state = CS_NOT_IN_PROGRESS;
            cachetable_lock(ct);
            assert(cf->most_recent_global_checkpoint_that_finished_early.lsn < ct->lsn_of_checkpoint_in_progress.lsn);
            cf->most_recent_global_checkpoint_that_finished_early = ct->lsn_of_checkpoint_in_progress;
	    local_checkpoint_during_checkpoint++;
            break;
        default:
            assert(FALSE);
        }
        { //Begin
            assert(cf->checkpoint_state == CS_NOT_IN_PROGRESS);
            r = cf->begin_checkpoint_userdata(cf, cf->fd, begin_checkpoint_lsn, cf->userdata);
            assert(r==0);
            cf->checkpoint_state = CS_CALLED_BEGIN_CHECKPOINT;
        }
        { //Middle
            assert(cf->checkpoint_state == CS_CALLED_BEGIN_CHECKPOINT);
            r = cf->checkpoint_userdata(cf, cf->fd, cf->userdata);
            assert(r==0);
            cf->checkpoint_state = CS_CALLED_CHECKPOINT;
        }
        { //End
            assert(cf->checkpoint_state == CS_CALLED_CHECKPOINT);
            r = cf->end_checkpoint_userdata(cf, cf->fd, cf->userdata);
            assert(r==0);
            cf->checkpoint_state = CS_NOT_IN_PROGRESS;
        }
        assert(cf->for_local_checkpoint.lsn == begin_checkpoint_lsn.lsn);
        cf->for_local_checkpoint = ZERO_LSN;

        rwlock_write_unlock(&cf->checkpoint_lock);
        rwlock_read_unlock(&cf->fdlock);
    }

    cachetable_unlock(ct);

    return 0;
}

