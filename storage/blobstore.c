// -*- mode: C; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: nil -*-
// vim: set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:

/*
 * blobstore.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h> // close
#include <time.h> // time
#include <sys/time.h> // gettimeofday
#include <sys/stat.h> // mkdir
#include <errno.h> // errno
#include <sys/types.h> // *dir, etc, wait
#include <sys/file.h> // flock
#include <dirent.h>
#include <sys/wait.h> // wait
#include <execinfo.h> // backtrace
#include <pthread.h>
#include "blobstore.h"
#include "diskutil.h"

#define BLOBSTORE_METADATA_FILE ".blobstore"
#define BLOBSTORE_DEFAULT_UMASK 0700
#define BLOBSTORE_METADATA_TIMEOUT_MS 999
#define BLOBSTORE_SLEEP_INTERVAL_MS 99
#define BLOBSTORE_MAX_CONCURRENT 99
#define BLOBSTORE_NO_TIMEOUT -1L
#define DM_PATH "/dev/mapper/"
#define DM_FORMAT DM_PATH "%s" // TODO: do not hardcode?
#define DMSETUP "/sbin/dmsetup" // TODO: do not hardcode?
#define MIN_BLOCKS_SNAPSHOT 32 // otherwise dmsetup fails with 
#define EUCA_ZERO "euca-zero"
#define EUCA_ZERO_SIZE "2199023255552" // is one a petabyte enough?
// device-mapper: reload ioctl failed: Cannot allocate memory OR
// device-mapper: reload ioctl failed: Input/output error

typedef enum { // paths to files containing... 
    BLOCKBLOB_PATH_NONE = 0, // sentinel for identifying files that are not blockblob related
    BLOCKBLOB_PATH_BLOCKS, // ...blocks, either in flat format or as a snapshot backing
    BLOCKBLOB_PATH_DM, // ...device mapper devices created for this clone, if any
    BLOCKBLOB_PATH_DEPS, // blockblobs that this blockblob depends on, if any
    BLOCKBLOB_PATH_LOOPBACK, // ...name of the loopback device for this blob, when attached
    BLOCKBLOB_PATH_SIG, // ...signature of the blob, if provided from outside
    BLOCKBLOB_PATH_REFS, // ...blockblobs that depend on this blockblob, if any
    BLOCKBLOB_PATH_TOTAL,
} blockblob_path_t; // if changing, change the array below and set_blockblob_metadata_path()

static const char * blobstore_metadata_suffixes [] = { // entries must match the ones in enum above
    "none", // sentinel entry so that all actual entries have indeces > 0
    "blocks", // must be second so loop in check_metadata_name() works
    "dm",
    "deps",
    "loopback",
    "sig",
    "refs"
};

typedef struct _blobstore_filelock {
    char path [PATH_MAX]; // path that the file was open with (TODO: canonicalize?)
    int type; // lock type, as used by fcntl()
    int refs; // number of open file descriptors for this path in this process
    int next_fd; // next available file descriptor in the table below:
    int fd        [BLOBSTORE_MAX_CONCURRENT];
    int fd_status [BLOBSTORE_MAX_CONCURRENT]; // 0 = unused, 1 = open
    pthread_rwlock_t lock; // reader/writer lock for controlling intra-process access
    pthread_mutex_t mutex; // for locking this specific struct during manipulations
    struct _blobstore_filelock * next; // pointer for constructing a LL
} blobstore_filelock;

__thread blobstore_error_t _blobstore_errno = BLOBSTORE_ERROR_OK; // thread-local errno
static unsigned char _do_print_errors = 1;
static pthread_mutex_t _blobstore_mutex = PTHREAD_MUTEX_INITIALIZER; // process-global mutex
static blobstore_filelock * locks_list = NULL; // process-global LL head (TODO: replace this with a hash table)
static char zero_buf [1] = "\0";

static void print_trace (void)
{
    void *array[64];
    size_t size;
    char **strings;
    size_t i;
    
    size = backtrace (array, sizeof(array)/sizeof(void *));
    strings = backtrace_symbols (array, size);
    
    for (i = 0; i < size; i++)
        printf ("\t%s\n", strings[i]);
    
    free (strings);
}

const char * blobstore_get_error_str ( blobstore_error_t error ) {
    return _blobstore_error_strings [error];
}

#define __INLINE__ __inline__

__INLINE__ static void _err_on (void)  { _do_print_errors = 1; }
__INLINE__ static void _err_off (void) { _do_print_errors = 0; }

static void err (blobstore_error_t error, const char * custom_msg)
{
    const char * msg = custom_msg;
    if ( msg == NULL ) {
        msg = blobstore_get_error_str (error);
    }
    if (_do_print_errors) {
        printf ("error: %s\n", msg); // TODO: add logging hooks
        //print_trace ();
    }
    _blobstore_errno = error;
}

/*
static void logg (unsigned int level, const char * msg)
{
    printf ("%s\n", msg);
}
*/

__INLINE__ static void propagate_system_errno (blobstore_error_t default_errno)
{
    switch (errno) {
    case ENOENT: _blobstore_errno = BLOBSTORE_ERROR_NOENT; break;
    case ENOMEM: _blobstore_errno = BLOBSTORE_ERROR_NOMEM; break;
    case EACCES: _blobstore_errno = BLOBSTORE_ERROR_ACCES; break;
    case EEXIST: _blobstore_errno = BLOBSTORE_ERROR_EXIST; break;
    case EINVAL: _blobstore_errno = BLOBSTORE_ERROR_INVAL; break;
    case ENOSPC: _blobstore_errno = BLOBSTORE_ERROR_NOSPC; break;
    case EAGAIN: _blobstore_errno = BLOBSTORE_ERROR_AGAIN; break;
    default:
        perror ("blobstore"); // TODO: remove?
        _blobstore_errno = default_errno;
    }
    err (_blobstore_errno, NULL); // print the message
}

static void gen_id (char * str, unsigned int size)
{
    struct timeval tv;
    gettimeofday (&tv, NULL);
    srandom ((unsigned int) ((unsigned long long)str * (unsigned long long)tv.tv_usec));
    snprintf (str, size, "%08lx%08lx%08lx", (unsigned long)random(), (unsigned long)random(), (unsigned long)random());
}

static struct flock * file_lock (short type, short whence) 
{
    static struct flock ret;
    ret.l_type = type;
    ret.l_start = 0;
    ret.l_whence = whence;
    ret.l_len = 0;
    ret.l_pid = getpid();
    return & ret;
}

// time since 1970 in microseconds
static long long time_usec (void)
{
    struct timeval tv;
    gettimeofday (&tv, NULL);
    return (long long)tv.tv_sec * 1000000 + tv.tv_usec;
}

// helper for close_and_unlock() and open_and_lock()
// MUST be called with _blobstore_mutex held
static void close_and_free_filelock (blobstore_filelock * l)
{
    // close all file descriptors at once 
    // (closing any one removes the lock for all descriptors)
    for (int i=0; i<l->next_fd; i++) {
        close (l->fd [i]);
    }
    pthread_rwlock_destroy (&(l->lock));
    pthread_mutex_destroy (&(l->mutex));    
    free (l);
}

// This function must be used to close files opened with open_and_lock().
// (Simply doing close() will leave the file locked via pthreads
// and future open_and_lock() requests from the same process may
// fail.)  Also, closing the file descriptor releases the OS file lock
// for the process, so any other read-only descriptors held by the
// process are no longer guarded since other processes may open the
// file for writing.
static int close_and_unlock (int fd)
{
    if (fd<0) {
        err (BLOBSTORE_ERROR_BADF, NULL);
        return -1;
    }
    int ret = 0;
    pthread_mutex_lock (&_blobstore_mutex); // grab global lock (this will not block and, plus, we may be deallocating)

    int index = -1;
    blobstore_filelock * l;
    blobstore_filelock ** next_ptr = &locks_list;
    for (l = locks_list; l; l=l->next) { // look for the fd 
        assert (l->next_fd>=0 && l->next_fd<=BLOBSTORE_MAX_CONCURRENT);
        for (int i=0; i<l->next_fd; i++) {
            if (l->fd [i] == fd) {
                index = i; // found it!
                break;
            }
        }
        if (index!=-1)
            break;
        next_ptr = &(l->next); // list head or prev element
    }
    if (l) { // we found a match
        assert (index>=0 && index<BLOBSTORE_MAX_CONCURRENT);
        if (l->fd_status [index] == 1) { // last not been closed
            l->fd_status [index] = 0; // set status to 'unused'
            if (--l->refs==0) { // if not other references...
                * next_ptr = l->next; // remove from LL
                close_and_free_filelock (l); // close and free(l)
            }
        } else {
            err (BLOBSTORE_ERROR_BADF, "file descriptor already closed");
            ret = -1;
        }
    } else {
        err (BLOBSTORE_ERROR_BADF, "not an open file descriptor");
        ret = -1;
    }

    pthread_mutex_unlock (&_blobstore_mutex);
    return ret;
}

// This function creates or opens a file and locks it; the lock is 
// - exclusive if the file is being created or written to, or a
// - non-exclusive readers' lock if the file was opened RDONLY.
// The lock works both across threads and processes.
// File descriptors obtained from this function should be
// released with close_and_unlock(). All locks held by a process 
// are released upon termination, whether normal or abnormal.
// 
// flags: BLOBSTORE_FLAG_RDONLY - open with O_RDONLY, reader lock
//        BLOBSTORE_FLAG_RDWR - open with O_RDWR, writer lock
//        BLOBSTORE_FLAG_CREAT - open with O_RDWR | O_CREAT, writer lock
//        BLOBSTORE_FLAG_EXCL - can be added to _CREAT, as with open()
//
// timeout_usec: timeout in microseconds for waiting on a lock
//               BLOBSTORE_NO_TIMEOUT / -1 - wait forever
//               BLOBSTORE_NO_WAIT / 0 - do not wait at all
//
// mode: gets passed to open() directly

static int open_and_lock (const char * path, 
                          int flags,
                          long long timeout_usec,
                          mode_t mode)
{
    short l_type;
    int o_flags = 0;
    long long deadline = time_usec() + timeout_usec;

    // decide what type of lock to use
    if (flags & BLOBSTORE_FLAG_RDONLY) {
        l_type = F_RDLCK; // use shared (read) lock
        o_flags |= O_RDONLY; // required when using F_RDLCK

    } else if ((flags & BLOBSTORE_FLAG_RDWR) ||
               (flags & BLOBSTORE_FLAG_CREAT)) {
        l_type = F_WRLCK; // use exclusive (write) lock
        o_flags |= O_RDWR; // required when using F_WRLCK
        if (flags & BLOBSTORE_FLAG_CREAT) {
            o_flags |= O_CREAT;
            // intentionally ignore _EXCL supplied without _CREAT
            if (flags & BLOBSTORE_FLAG_EXCL) o_flags |= O_EXCL;
        }

    } else {
        err (BLOBSTORE_ERROR_INVAL, "flags to open_and_lock must include either _RDONLY or _RDWR or _CREAT");
        return -1;
    }

    // handle intra-process locking, with a pthreads read-write lock
    pthread_mutex_lock (&_blobstore_mutex); // grab the global lock
    blobstore_filelock * l;
    blobstore_filelock ** next_ptr = &locks_list;
    for (l = locks_list; l; l=l->next) { // look through existing locks
        next_ptr = &(l->next); // either LL head or last element's next pointer
        if (strcmp (path, l->path) == 0)
            break;
    }
    if (l==NULL) { // this path is not locked by any thread
        l = calloc (1, sizeof(blobstore_filelock));
        if (l==NULL) {
            pthread_mutex_unlock (&_blobstore_mutex);
            err (BLOBSTORE_ERROR_NOMEM, NULL);
            return -1;
        }
        strncpy (l->path, path, sizeof(l->path));
        pthread_rwlock_init (&(l->lock), NULL);
        pthread_mutex_init (&(l->mutex), NULL);
        l->type = l_type; // lock type must match in future
        * next_ptr = l; // add at the end of LL
    }
    if (l->next_fd==BLOBSTORE_MAX_CONCURRENT) {
        pthread_mutex_unlock (&_blobstore_mutex);
        err (BLOBSTORE_ERROR_MFILE, "too many open file descriptors");
        return -1;
    }
    if (l->type!=l_type) {
        pthread_mutex_unlock (&_blobstore_mutex);
        err (BLOBSTORE_ERROR_INVAL, "lock type mismatch with the existing lock");
        return -1;
    }
    l->refs++; // increase the reference count while still under lock
    pthread_mutex_unlock (&_blobstore_mutex); // release global mutex

    // open/create the file, using Posix file locks for inter-process locking
    int pthread_rwlock_acquired = 0;
    int fd = open (path, o_flags, mode);
    if (fd == -1) {
        propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
        goto error;
    }
    for (;;) {
        // first try getting the posix rwlock
        int ret;
        if (l_type == F_WRLCK) 
            ret = pthread_rwlock_trywrlock (&(l->lock));
        else 
            ret = pthread_rwlock_tryrdlock (&(l->lock));
        if (ret==0) {
            // posix rwlock succeeded, try the file lock
            pthread_rwlock_acquired = 1;
            errno = 0;
            if (fcntl (fd, F_SETLK, file_lock (l_type, SEEK_SET)) == 0)
                break;
            if (errno != EAGAIN) { // any error other than inability to get the lock
                propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
                goto error;
            }
        }
        if (timeout_usec!=BLOBSTORE_NO_TIMEOUT && time_usec() >= deadline) { // we timed out waiting for the lock
            err (BLOBSTORE_ERROR_AGAIN, NULL);
            goto error;
        }
        usleep (BLOBSTORE_SLEEP_INTERVAL_MS);
    }
    pthread_mutex_lock (&(l->mutex)); // grab path-specific mutex for atomic update to the table of descriptors
    l->fd        [l->next_fd] = fd;
    l->fd_status [l->next_fd] = 1; 
    l->next_fd++;
    pthread_mutex_unlock (&(l->mutex));
    return fd;

 error:
    if (pthread_rwlock_acquired) pthread_rwlock_unlock (&(l->lock));
    pthread_mutex_lock (&_blobstore_mutex); // grab the global lock, since we may be deallocating
    if (fd>=0) // we succeded with open() but failed to get the lock
        close (fd);
    if (--l->refs==0) {
        * next_ptr = l->next;
        close_and_free_filelock (l);
    }
    pthread_mutex_unlock (&_blobstore_mutex);
    return -1;
}

static char * get_val (const char * buf, const char * key) 
{
    char * val = NULL;
    char full_key [512];
    snprintf (full_key, sizeof (full_key), "%s: ", key);
    char * val_begin = strstr (buf, full_key);
    if (val_begin) {
        val_begin += strlen (full_key);
        char * val_end = val_begin;
        while (* val_end != '\n' && * val_end != '\0') val_end++;
        val = calloc (val_end-val_begin+1, sizeof(char)); // +1 for the \0
        if (val==NULL) { err (BLOBSTORE_ERROR_NOMEM, NULL); return NULL; }
        strncpy (val, val_begin, val_end-val_begin);
    }

    return val;
}

// helper for reading a file into a buffer
// returns size read or -1 if error
static int fd_to_buf (int fd, char * buf, int size_buf)
{
    if (lseek (fd, 0, SEEK_SET)==-1) 
        { err (BLOBSTORE_ERROR_ACCES, "failed to seek in metadata file"); return -1; }

    struct stat sb;
    if (fstat (fd, &sb)==-1) 
        { err (BLOBSTORE_ERROR_ACCES, "failed to stat metadata file"); return -1; }

    if (read (fd, buf, size_buf) != sb.st_size)
        { err (BLOBSTORE_ERROR_NOENT, "failed to read metadata file"); return -1; }

    return sb.st_size;
}

static int read_store_metadata (blobstore * bs)
{
    char buf [1024];
    int size = fd_to_buf (bs->fd, buf, sizeof (buf));

    if (size == -1)
        return -1;

    if (size<30) { 
        err (BLOBSTORE_ERROR_NOENT, "metadata size is too small"); 
        return -1; 
    }
    
    char * val;
    if ((val = get_val (buf, "id"))==NULL) 
        return -1; 
    strncpy (bs->id, val, sizeof (bs->id)); 
    free (val);

    if ((val = get_val (buf, "limit"))==NULL) return -1; 
    errno = 0; bs->limit_blocks = strtoll (val, NULL, 10); free (val); if (errno!=0) { err (BLOBSTORE_ERROR_NOENT, "invalid metadata file (limit is missing)"); return -1; }

    if ((val = get_val (buf, "revocation"))==NULL) return -1; 
    errno = 0; bs->revocation_policy = strtoll (val, NULL, 10); free (val); if (errno!=0) { err (BLOBSTORE_ERROR_NOENT, "invalid metadata file (revocation is missing)"); return -1; }

    if ((val = get_val (buf, "snapshot"))==NULL) return -1; 
    errno = 0; bs->snapshot_policy = strtoll (val, NULL, 10); free (val); if (errno!=0) { err (BLOBSTORE_ERROR_NOENT, "invalid metadata file (snapshot is missing)"); return -1; }

    if ((val = get_val (buf, "format"))==NULL) return -1; 
    errno = 0; bs->format = strtoll (val, NULL, 10); free (val); if (errno!=0) { err (BLOBSTORE_ERROR_NOENT, "invalid metadata file (format is missing)"); return -1; }
    return 0;
}

static int write_store_metadata (blobstore * bs)
{
    if (ftruncate (bs->fd, 0)==-1)
        { err (BLOBSTORE_ERROR_NOENT, "failed to truncate the metadata file"); return -1; }
    char buf [1024];
    snprintf (buf, sizeof (buf), 
              "id: %s\n"      \
              "limit: %lld\n" \
              "revocation: %d\n" \
              "snapshot: %d\n" \
              "format: %d\n",
              bs->id,
              bs->limit_blocks,
              bs->revocation_policy,
              bs->snapshot_policy,
              bs->format);
    int len = write (bs->fd, buf, strlen (buf));
    if (len != strlen (buf))
        { err (BLOBSTORE_ERROR_NOENT, "failed to write to the metadata file"); return -1; }
        
    return 0;
}

static int blobstore_init (void)
{
    logfile (NULL, EUCAWARN);
    int ret = diskutil_init(); 
    if (ret) {
        err (BLOBSTORE_ERROR_UNKNOWN, "failed to initialize blobstore library");
    }
    return ret;
}

static int blobstore_cleanup (void)
{
    diskutil_cleanup();
    return 0;
}

blobstore * blobstore_open ( const char * path, 
                             unsigned long long limit_blocks,
                             blobstore_format_t format,
                             blobstore_revocation_t revocation_policy,
                             blobstore_snapshot_t snapshot_policy)
{
    if (blobstore_init()) 
        return NULL;

    blobstore * bs = calloc (1, sizeof (blobstore));
    if (bs==NULL) {
        err (BLOBSTORE_ERROR_NOMEM, NULL);
        goto out;
    }
    strncpy (bs->path, path, sizeof(bs->path)); // TODO: canonicalize path
    char meta_path [PATH_MAX];
    snprintf (meta_path, sizeof(meta_path), "%s/%s", bs->path, BLOBSTORE_METADATA_FILE);

    _blobstore_errno = BLOBSTORE_ERROR_OK;
    _err_off();
    bs->fd = open_and_lock (meta_path, BLOBSTORE_FLAG_CREAT | BLOBSTORE_FLAG_EXCL, 0, 0600);
    _err_on();
    if (bs->fd != -1) { // managed to create blobstore metadata file and got exclusive lock

        gen_id (bs->id, sizeof(bs->id));
        bs->limit_blocks = limit_blocks;
        bs->revocation_policy = (revocation_policy==BLOBSTORE_REVOCATION_ANY) ? BLOBSTORE_REVOCATION_NONE : revocation_policy;
        bs->snapshot_policy = (snapshot_policy==BLOBSTORE_SNAPSHOT_ANY) ? BLOBSTORE_SNAPSHOT_DM : snapshot_policy; // TODO: verify that DM is available?
        bs->format = (format==BLOBSTORE_FORMAT_ANY) ? BLOBSTORE_FORMAT_FILES : format;

        // write metadata to disk
        write_store_metadata (bs);
        close_and_unlock (bs->fd); // try to close, thus giving up the exclusive lock
    }
    if (_blobstore_errno != BLOBSTORE_ERROR_OK && // either open or write failed
        _blobstore_errno != BLOBSTORE_ERROR_EXIST && // it is OK if file already exists
        _blobstore_errno != BLOBSTORE_ERROR_AGAIN ) { // it is OK if we lost the race for the write lock
        err (_blobstore_errno, "failed to open or create blobstore");
        goto free;
    }
    
    // now (re)open, with a shared read lock
    bs->fd = open_and_lock (meta_path, BLOBSTORE_FLAG_RDONLY, BLOBSTORE_METADATA_TIMEOUT_MS, 0);
    if (bs->fd == -1) {
        goto free;
    }
    if ( read_store_metadata (bs) ) { // try reading metadata
        goto free;
    }

    // verify that parameters are not being changed
    if (limit_blocks && 
        limit_blocks != bs->limit_blocks) {
        err (BLOBSTORE_ERROR_INVAL, "'limit_blocks' does not match existing blobstore");
        goto free;
    }
    if (snapshot_policy != BLOBSTORE_SNAPSHOT_ANY && 
        snapshot_policy != bs->snapshot_policy) {
        err (BLOBSTORE_ERROR_INVAL, "'snapshot_policy' does not match existing blobstore");
        goto free;
    }
    if (format != BLOBSTORE_FORMAT_ANY && 
        format != bs->format) {
        err (BLOBSTORE_ERROR_INVAL, "'format' does not match existing blobstore");
        goto free;
    } 
    if (revocation_policy != BLOBSTORE_REVOCATION_ANY &&
        revocation_policy != bs->revocation_policy) {
        err (BLOBSTORE_ERROR_INVAL, "'revocation_policy' does not match existing blobstore"); // TODO: maybe make revocation_policy changeable after creation
        goto free;
    }
    int fd = bs->fd;
    bs->fd = -1;
    close_and_unlock (fd);
    goto out;

 free:
    close_and_unlock (bs->fd);
    if (bs) {
        free (bs);
        bs = NULL;
    }
 out:

    return bs;
}

// frees the blobstore handle
int blobstore_close ( blobstore * bs )
{
    free (bs);
    return 0;
}

static pthread_mutex_t _blobstore_lock_mutex = PTHREAD_MUTEX_INITIALIZER; // process-global mutex

// locks the blobstore 
int blobstore_lock ( blobstore * bs, long long timeout_usec)
{
    char meta_path [PATH_MAX];
    snprintf (meta_path, sizeof(meta_path), "%s/%s", bs->path, BLOBSTORE_METADATA_FILE);

    int fd = open_and_lock (meta_path, BLOBSTORE_FLAG_RDWR, timeout_usec, 0);
    if (fd!=-1)
        bs->fd = fd;
    return fd;
}

// unlocks the blobstore
int blobstore_unlock ( blobstore * bs )
{
    int fd = bs->fd;
    bs->fd = -1;
    return close_and_unlock (fd);
}

// if no outside references to store or blobs exist, and 
// no blobs are protected, deletes the blobs, the store metadata, 
// and frees the blobstore handle
int blobstore_delete ( blobstore * bs ) 
{
    return -1; // TODO: implement blobstore_delete
}

int blobstore_get_error (void) 
{
    return _blobstore_errno;
}

// helper for setting paths, depending on blockblob_path_t
//
//  given BLOCKBLOB_PATH_X: x = tolower(X)
//  
//  for BLOBSTORE_FORMAT_FILES:     BS/BB.x
//  for BLOBSTORE_FORMAT_DIRECTORY: BS/BB/x
//
//  where BS is blobstore path and BB is a blockblob id.
//  BB may have '/' in it, thus placing all blob-related
//  files in a deeper dir hierarchy

static int set_blockblob_metadata_path (blockblob_path_t path_t, const blobstore * bs, const char * bb_id, char * path, size_t path_size)
{
    char base [PATH_MAX];
    snprintf (base, sizeof (base), "%s/%s", bs->path, bb_id);

    char name [32];
    switch (path_t) {
    case BLOCKBLOB_PATH_BLOCKS:   strncpy (name, blobstore_metadata_suffixes[BLOCKBLOB_PATH_BLOCKS],   sizeof (name)); break;
    case BLOCKBLOB_PATH_DM:       strncpy (name, blobstore_metadata_suffixes[BLOCKBLOB_PATH_DM],       sizeof (name)); break;
    case BLOCKBLOB_PATH_DEPS:     strncpy (name, blobstore_metadata_suffixes[BLOCKBLOB_PATH_DEPS],     sizeof (name)); break;
    case BLOCKBLOB_PATH_LOOPBACK: strncpy (name, blobstore_metadata_suffixes[BLOCKBLOB_PATH_LOOPBACK], sizeof (name)); break;
    case BLOCKBLOB_PATH_SIG:      strncpy (name, blobstore_metadata_suffixes[BLOCKBLOB_PATH_SIG],      sizeof (name)); break;
    case BLOCKBLOB_PATH_REFS:     strncpy (name, blobstore_metadata_suffixes[BLOCKBLOB_PATH_REFS],     sizeof (name)); break;
    default:
        err (BLOBSTORE_ERROR_INVAL, "invalid path_t");
        return -1;
    }

    switch (bs->format) {
    case BLOBSTORE_FORMAT_FILES:
        snprintf (path, path_size, "%s.%s", base, name);
        break;
    case BLOBSTORE_FORMAT_DIRECTORY:
        snprintf (path, path_size, "%s/%s", base, name);
        break;
    default:
        err (BLOBSTORE_ERROR_INVAL, "invalid bs->format");
        return -1;
    }

    return 0;
}

// write string 'str' into a specific metadata file (based on 'path_t') of blob 'bb_id'
// returns 0 for success or -1 for error
static int write_blockblob_metadata_path (blockblob_path_t path_t, const blobstore * bs, const char * bb_id, const char * str)
{
    char path [PATH_MAX];
    set_blockblob_metadata_path (path_t, bs, bb_id, path, sizeof (path));

    FILE * FH = fopen (path, "w");
    if (FH) {
        fprintf (FH, "%s", str);
        fclose (FH);
    } else {
        propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);        
        return -1;
    }
    return 0;    
}

// reads contents of a specific metadata file (based on 'path_t') of blob 'bb_id' into string 'str' up to 'str_size'
// returns number of bytes read or -1 in case of error
static int read_blockblob_metadata_path (blockblob_path_t path_t, const blobstore * bs, const char * bb_id, char * str, int str_size)
{
    char path [PATH_MAX];
    set_blockblob_metadata_path (path_t, bs, bb_id, path, sizeof (path));

    int fd = open (path, O_RDONLY);
    if (fd == -1) {
        propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
        return -1;
    }
        
    int size = fd_to_buf (fd, str, str_size);
    close (fd);

    if (size < 1)
        { err (BLOBSTORE_ERROR_NOENT, "blockblob metadata size is too small"); return -1; }

    return size;
}

// writes strings from 'array' or size 'array_size' (which can be 0) line-by-line
// into a specific metadata file (based on 'path_t') of blob 'bb_id'
// returns 0 for success and -1 for error
static int write_array_blockblob_metadata_path (blockblob_path_t path_t, const blobstore * bs, const char * bb_id, char ** array, int array_size)
{
    int ret = 0;
    char path [MAX_PATH];
    set_blockblob_metadata_path (path_t, bs, bb_id, path, sizeof (path));

    FILE * fp = fopen (path, "w+");
    if (fp == NULL) {
        propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
        return -1;
    }

    for (int i=0; i<array_size; i++) {
        if (fprintf (fp, "%s\n", array [i]) < 0) {
            propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
            ret = -1;
            break;
        }
    }
    if (fclose (fp) == -1) {
        propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
        ret = -1;
    }
    return ret;
}

// reads lines from a specific metadata file (based on 'path_t') of blob 'bb_id',
// places each line into a newly allocated string, arranges pointers to these
// strings into a newly allocated array of pointers, and places the size into 'array_size'
// caller must deallocate the array and the strings pointed to by the array
// returns 0 for success and -1 for error
static int read_array_blockblob_metadata_path (blockblob_path_t path_t, const blobstore * bs, const char * bb_id, char *** array, int * array_size)
{
    int ret = 0;
    char path [MAX_PATH];
    set_blockblob_metadata_path (path_t, bs, bb_id, path, sizeof (path));

    FILE * fp = fopen (path, "r");
    if (fp == NULL) {
        * array = NULL;
        * array_size = 0;
        return 0;
    }

    int i;
    size_t n;
    char ** lines = NULL;
    for (i=0; !feof(fp); i++) {
        char * line = NULL;
        if (getline (&line, &n, fp)==-1) {
            if (feof(fp)) { // need this when '\n' is last char in the file
                free (line);
                break;
            }
            propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
            ret = -1;
            break;
        }
        int len = strlen (line);
        if (len && line [len-1] == '\n') {
            line [len-1] = '\0'; // chop off '\n'
        }
        char ** bigger_lines = realloc (lines, (i+1) * sizeof(char *));
        if (bigger_lines==NULL) {
            err (BLOBSTORE_ERROR_NOMEM, NULL);
            ret = -1;
            break;
        }
        lines = bigger_lines;
        lines [i] = line;
    }
    if (fclose (fp) == -1) {
        propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
        ret = -1;
    }
    if (ret == -1) {
        if (lines!=NULL) {
            for (int j=0; j<i; j++) {
                free (lines [j]);
            }
            free (lines);
        }
    } else {
        * array = lines;
        * array_size = i;
    }

    return ret;
}

static int update_entry_blockblob_metadata_path (blockblob_path_t path_t, const blobstore * bs, const char * bb_id, const char * entry, int removing)
{
    int ret = 0;

    // read in current entries from a metadata file
    char ** entries;
    int entries_size;
    if (read_array_blockblob_metadata_path (path_t, bs, bb_id, &entries, &entries_size)==-1) {
        return -1;
    }

    // see if this entry is already in the metadata file
    int found = -1;
    for (int j=0; j<entries_size; j++) {
        if (!strcmp (entry, entries [j])) {
            found = j;
            break;
        }
    }
    
    if (found==-1 && !removing) { // not in the file and adding
        entries_size++;
        char ** bigger_entries = calloc (entries_size, sizeof (char *));
        if (bigger_entries == NULL) {
            ret = -1;
            goto cleanup;
        }
        for (int i=0; i<entries_size-1; i++) { // we do not trust realloc
            bigger_entries [i] = entries [i];
        }
        if (entries) free (entries);
        entries = bigger_entries;
        entries [entries_size-1] = strdup (entry);

    } else if (found!=-1 && removing) { // in the file and deleting
        free (entries [found]);
        entries_size--;
        if (entries_size && found!=entries_size) {  // still entries left and not deleting last one
            entries [found] = entries [entries_size]; // move the last one over the one we're deleting
        }

    } else { // nothing to do
        goto cleanup;
    }
    
    // save new entries into the metadata file
    if (write_array_blockblob_metadata_path (path_t, bs, bb_id, entries, entries_size)==-1) {
        ret = -1;
    }

 cleanup:
    if (entries != NULL) {
        for (int j=0; j<entries_size; j++) {
            free (entries [j]);
        }
        free (entries);
    }
    return ret;
}

// if 'path' looks like a blockblob metadata file (based on the suffix), 
// return the type of the file and set bb_id appropriately, else
// return 0 if it is an unrecognized file, else
// return -1 for error
static int typeof_blockblob_metadata_path (const blobstore * bs, const char * path, char * bb_id, unsigned int bb_id_size)
{
    assert (path);
    assert (bs->path);
    assert (strstr (path, bs->path) == path);

    const char * rel_path = path + strlen (bs->path) + 1; // +1 for '/'
    int p_len = strlen (rel_path);

    for (int i=1; i<BLOCKBLOB_PATH_TOTAL; i++) { // start at 1 to avoid BLOCKBLOB_PATH_NONE
        char suffix [1024];
        if (bs->format == BLOBSTORE_FORMAT_DIRECTORY) {
            snprintf (suffix, sizeof (suffix), "/%s", blobstore_metadata_suffixes [i]);
        } else {
            snprintf (suffix, sizeof (suffix), ".%s", blobstore_metadata_suffixes [i]);
        }
        unsigned int s_len = strlen (suffix);
        const char * sp = suffix   + s_len - 1; // last char of suffix
        const char * pp = rel_path + p_len - 1; // last char of (relative) path
        unsigned int matched;
        for (matched=0; * sp == * pp; sp--, pp--) {
            matched++;
            if (sp == suffix) 
                break;
            if (pp == rel_path) 
                break;
        }
        if (matched==s_len // whole suffix matched
            && matched<p_len) { // there is more than the suffix
            if ((bb_id_size - 1) < ( p_len - s_len)) // not enough room in bb_id
                return -1;
            strncpy (bb_id, rel_path, p_len - s_len); // extract the name, without the suffix
            bb_id [p_len - s_len] = '\0'; // terminate the string
            return i;
        }
    }
    return 0;
}

// returns the number of files and directories deleted as part of
// removing the blob (thus, 0 means there was nothing to delete)
static int delete_blockblob_files (const blobstore * bs, const char * bb_id) 
{
    int count = 0;

    for (int path_t=1; path_t<BLOCKBLOB_PATH_TOTAL; path_t++) { // go through all types of blob-related files...
        char path [PATH_MAX];
        set_blockblob_metadata_path ((blockblob_path_t) path_t, bs, bb_id, path, sizeof (path));
        if (unlink (path)==0) // ...and try deleting them
            count++;
    }

    // delete blob's subdirectories if there are any
    char path [PATH_MAX];
    snprintf (path, sizeof (path), "%s/%s%s", bs->path, bb_id, bs->format == BLOBSTORE_FORMAT_DIRECTORY ? "/" : "");
    for (int i = strlen (path) - 1; i > 0; i--) {
        if (path [i] == '/') {
            path [i] = '\0';
            if (rmdir (path)==0) {
                count++;
            } else {
                break;
            }
        }
    }
            
    return count;
}

// given path=A/B/C and only A existing, create A/B and, unless
// is_file_path==1, also create A/B/C directory
// returns: 0 = path already existed, 1 = created OK, -1 = error
static int ensure_directories_exist (const char * path, int is_file_path, mode_t mode)
{
    int len = strlen (path);
    char * path_copy = NULL;
    int ret = 0;
    int i;

    if (len>0)
        path_copy = strdup (path);

    if (path_copy==NULL)
        return -1;

    for (i=0; i<len; i++) {
        struct stat buf;
        int try_dir = 0;

        if (path[i]=='/' && i>0) { // dir path, not root
            path_copy[i] = '\0';
            try_dir = 1;

        } else if (path[i]!='/' && i+1==len) { // last one
            if (!is_file_path)
                try_dir = 1;
        }

        if ( try_dir ) {
            if ( stat (path_copy, &buf) == -1 ) {
                logprintfl (EUCAINFO, "creating path %s\n", path_copy);

                if ( mkdir (path_copy, mode) == -1) {
                    logprintfl (EUCAERROR, "error: failed to create path %s: %s\n", path_copy, strerror (errno));

                    free (path_copy);
                    return -1;
                }
                ret = 1; // we created a directory
            }
            path_copy[i] = '/'; // restore the slash
        }
    }

    free (path_copy);
    return ret;
}

// helper for ensuring a directory required by blob exists
// returns: 0 = already existed, 1 = created OK, -1 = error
static int ensure_blockblob_metadata_path (const blobstore * bs, const char * bb_id) 
{
    char base [PATH_MAX];
    snprintf (base, sizeof (base), "%s/%s", bs->path, bb_id);
    return ensure_directories_exist (base, !(bs->format == BLOBSTORE_FORMAT_DIRECTORY), BLOBSTORE_DEFAULT_UMASK);
}

static void free_bbs ( blockblob * bbs )
{
    while (bbs) {
        blockblob * next_bb = bbs->next;
        free (bbs);
        bbs = next_bb;
    }
}

static unsigned int check_in_use ( blobstore * bs, const char * bb_id, long long timeout_usec)
{
    unsigned int in_use = 0;
    char buf [PATH_MAX];

    set_blockblob_metadata_path (BLOCKBLOB_PATH_BLOCKS, bs, bb_id, buf, sizeof (buf));

    _err_off(); // do not care if blocks file does not exist
    int fd = open_and_lock (buf, BLOBSTORE_FLAG_RDWR, timeout_usec, timeout_usec); // try opening to see what happens
    if (fd != -1) {
        close_and_unlock (fd); 
    } else {
        in_use |= BLOCKBLOB_STATUS_OPENED; // TODO: check if open failed for other reason?
    }
    
    if (read_blockblob_metadata_path (BLOCKBLOB_PATH_REFS, bs, bb_id, buf, sizeof (buf))>0) {
        in_use |= BLOCKBLOB_STATUS_MAPPED;
    }

    if (read_blockblob_metadata_path (BLOCKBLOB_PATH_DEPS, bs, bb_id, buf, sizeof (buf))>0) {
        in_use |= BLOCKBLOB_STATUS_BACKED;
    }
    _err_on();

    return in_use;
}

static void set_device_path (blockblob * bb)
{
    char ** dm_devs = NULL;
    int dm_devs_size = 0;
    
    _err_off(); // do not care if .dm file does not exist
    read_array_blockblob_metadata_path (BLOCKBLOB_PATH_DM, bb->store, bb->id, &dm_devs, &dm_devs_size);
    _err_on();
    
    if (dm_devs_size>0) { // .dm is there => set device_path to the device-mapper path
        snprintf (bb->device_path, sizeof(bb->device_path), DM_FORMAT, dm_devs [dm_devs_size-1]); // main device is the last one
        strncpy (bb->dm_name, dm_devs [dm_devs_size-1], sizeof (bb->dm_name));
        for (int i=0; i<dm_devs_size; i++) {
            free (dm_devs [i]);
        }
        free (dm_devs);
    } else { // .dm is not there => set device_path to loopback
        char lo_dev [PATH_MAX] = "";
        _err_off(); // do not care if loopback file does not exist
        read_blockblob_metadata_path (BLOCKBLOB_PATH_LOOPBACK, bb->store, bb->id, lo_dev, sizeof (lo_dev));
        _err_on();
        strncpy (bb->device_path, lo_dev, sizeof (bb->device_path));
    }
}

static blockblob ** walk_bs (blobstore * bs, const char * dir_path, blockblob ** tail_bb) 
{
    int ret = 0;
    DIR * dir;
    if ((dir=opendir(dir_path))==NULL) {
        return tail_bb; // ignore access errors in blobstore directory
    }

    struct dirent * dir_entry;
    while ((dir_entry=readdir(dir))!=NULL) {
        char * entry_name = dir_entry->d_name;

        if (!strcmp(".", entry_name) ||
            !strcmp("..", entry_name) ||
            !strcmp(BLOBSTORE_METADATA_FILE, entry_name))
            continue; // ignore known unrelated files

        // get the path of the directory item
        char entry_path [BLOBSTORE_MAX_PATH];
        snprintf (entry_path, sizeof (entry_path), "%s/%s", dir_path, entry_name);
        struct stat sb;
        if (stat(entry_path, &sb)==-1)
            continue; // ignore access errors in the blobstore directory (TODO: is this wise?)

        // recurse if this is a directory
        if (S_ISDIR (sb.st_mode)) {
            tail_bb = walk_bs (bs, entry_path, tail_bb);
            if (tail_bb == NULL)
                return NULL;
            continue;
        }
        
        char blob_id [BLOBSTORE_MAX_PATH];
        if (typeof_blockblob_metadata_path (bs, entry_path, blob_id, sizeof(blob_id)) != BLOCKBLOB_PATH_BLOCKS)
            continue; // ignore all files except .blocks file

        blockblob * bb = calloc (1, sizeof (blockblob));
        if (bb==NULL) {
            ret = 1;
            goto free;
        }
        * tail_bb = bb; // add to LL
        tail_bb = & (bb->next);
        // fill out the struct
        bb->store = bs;
        strncpy (bb->id, blob_id, sizeof(bb->id));
        strncpy (bb->blocks_path, entry_path, sizeof(bb->blocks_path));
        set_device_path (bb); // read .dm and .loopback and set bb->device_path accordingly
        bb->size_blocks = (sb.st_size/512);
        bb->last_accessed = sb.st_atime;
        bb->last_modified = sb.st_mtime;
        bb->snapshot_type = BLOBSTORE_FORMAT_ANY;
        bb->in_use = check_in_use (bs, bb->id, 0);
    }

 free:
    closedir (dir);
    return tail_bb;
}

// runs through the blobstore and puts all found blockblobs 
// into a linked list, returning its head
static blockblob * scan_blobstore ( blobstore * bs )
{
    blockblob * bbs = NULL;
    if (walk_bs (bs, bs->path, &bbs)==NULL) {
        if (bbs) 
            free_bbs (bbs);
        bbs = NULL;
    }
    
    return bbs;
}

static int compare_bbs (const void * bb1, const void * bb2) 
{
    return (int)((*(blockblob **)bb1)->last_modified - (*(blockblob **)bb2)->last_modified);
}

static long long purge_blockblobs_lru ( blobstore * bs, blockblob * bb_list, long long need_blocks ) 
{
    int list_length = 0;
    long long purged = 0;

    for (blockblob * bb = bb_list; bb; bb = bb->next) {
        list_length++;
    }

    if (list_length) {
        blockblob * bb;
        int i;

        blockblob ** bb_array = (blockblob **) calloc (list_length, sizeof (blockblob *));
        for (i=0, bb = bb_list; bb; bb = bb->next, i++) {
            bb_array [i] = bb;
        }

        qsort (bb_array, list_length, sizeof (blockblob *), compare_bbs);

        for (i=0; i<list_length; i++) {
            bb = bb_array [i];
            if (! (bb->in_use & ~BLOCKBLOB_STATUS_BACKED) ) {
                if (delete_blockblob_files (bs, bb->id)>0) {
                    purged += bb->size_blocks;
                    printf ("purged from blobstore %s blockblob %s of size %lld (total purged in this sweep %lld)\n", bs->id, bb->id, bb->size_blocks, purged);
                }
            }
            if (purged>=need_blocks)
                break;
        }
        free (bb_array);
    }

    return purged;
}

blockblob * blockblob_open ( blobstore * bs,
                             const char * id, // can be NULL if creating, in which case blobstore will pick a random ID
                             unsigned long long size_blocks, // on create: reserve this size; on open: verify the size, unless set to 0
                             unsigned int flags, // BLOBSTORE_FLAG_CREAT | BLOBSTORE_FLAG_EXCL - same semantcs as for open() flags
                             const char * sig, // if non-NULL, on create sig is recorded, on open it is verified
                             unsigned long long timeout ) // maximum wait, in milliseconds
{
    if (flags & ~(BLOBSTORE_FLAG_CREAT | BLOBSTORE_FLAG_EXCL)) {
        err (BLOBSTORE_ERROR_INVAL, "only _CREAT and _EXCL flags are allowed");
        return NULL;
    }
    if (id==NULL && !(flags & BLOBSTORE_FLAG_CREAT)) {
        err (BLOBSTORE_ERROR_INVAL, "NULL id is only allowed with _CREAT");
        return NULL;
    }
    if (size_blocks==0 && (flags & BLOBSTORE_FLAG_CREAT)) {
        err (BLOBSTORE_ERROR_INVAL, "size_blocks can be 0 only without _CREAT");
        return NULL;
    }
    if (size_blocks!=0 && (flags & BLOBSTORE_FLAG_CREAT) && (size_blocks > bs->limit_blocks)) {
        err (BLOBSTORE_ERROR_NOSPC, NULL);
 return NULL;
    }

    blockblob * bbs = NULL; // a temp LL of blockblobs, used for computing free space and for purging
    blockblob * bb = calloc (1, sizeof (blockblob));
    if (bb==NULL) {
        err (BLOBSTORE_ERROR_NOMEM, NULL);
        goto out;
    }
    
    bb->store = bs;
    if (id) {
        strncpy (bb->id, id, sizeof(bb->id));
    } else {
        gen_id (bb->id, sizeof(bb->id));
    }
    bb->size_blocks = size_blocks;
    set_blockblob_metadata_path (BLOCKBLOB_PATH_BLOCKS, bs, bb->id, bb->blocks_path, sizeof (bb->blocks_path));

    if (blobstore_lock(bs, timeout)==-1) { // lock it so we can traverse it (TODO: move this into creation-only section?)
        goto free; // failed to obtain a lock on the blobstore
    }

    int created_directory = ensure_blockblob_metadata_path (bs, bb->id);
    if (created_directory==-1) {
        propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
        goto unlock;
    }

    int created_blob = 0;
    bb->fd = open_and_lock (bb->blocks_path, flags | BLOBSTORE_FLAG_RDWR, timeout, 0600); // blobs are always opened with exclusive write access
    if (bb->fd != -1) { 
        struct stat sb;
        if (fstat (bb->fd, &sb)==-1) {
            propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
            goto clean;
        }
        
        if (sb.st_size == 0) { // new blob
            created_blob = 1;

            // put existing items in the blobstore into a LL
            _blobstore_errno = BLOBSTORE_ERROR_OK;
            bbs = scan_blobstore (bs);
            if (bbs==NULL) {
                if (_blobstore_errno != BLOBSTORE_ERROR_OK) {
                    goto clean;
                }
            }

            // analyze the LL, calculating sizes
            long long blocks_allocated = 0;
            long long blocks_inuse = 0;
            unsigned int blobs_total = 0;
            for (blockblob * abb = bbs; abb; abb=abb->next) {
                if (abb->in_use & ~BLOCKBLOB_STATUS_BACKED) {
                    blocks_inuse += abb->size_blocks; // these can't be purged if we need space (TODO: look into recursive purging of unused references?)
                } else {
                    blocks_allocated += abb->size_blocks; // these can be purged
                }
                blobs_total++;
            }

            long long blocks_free = bs->limit_blocks - (blocks_allocated + blocks_inuse);
            if (blocks_free < bb->size_blocks) {
                if (!(bs->revocation_policy==BLOBSTORE_REVOCATION_LRU) // not allowed to purge
                    ||
                    (blocks_free+blocks_allocated) < bb->size_blocks) { // not enough purgeable material
                    err (BLOBSTORE_ERROR_NOSPC, NULL);
                    goto clean;
                } 
                long long blocks_needed = bb->size_blocks-blocks_free;
                _err_off(); // do not care about errors duing purging
                long long blocks_freed = purge_blockblobs_lru (bs, bbs, blocks_needed);
                _err_on();
                if (blocks_freed < blocks_needed) {
                    err (BLOBSTORE_ERROR_NOSPC, "could not purge enough from cache");
                    goto clean;
                }
            }
            
            if (lseek (bb->fd, (bb->size_blocks*512)-1, SEEK_CUR)==(off_t) -1) { // create a file with a hole
                propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
                goto clean;
            }
            if (write (bb->fd, zero_buf, 1)!=(ssize_t)1) {
                propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
                goto clean;
            }
            if (sig)
                if (write_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb->id, sig))
                    goto clean; 

        } else { // blob existed

            char buf [1024];
            if (bb->size_blocks==0) {
                bb->size_blocks = (sb.st_size/512);
            } else if (bb->size_blocks != (sb.st_size/512)) { // check the size
                err (BLOBSTORE_ERROR_INVAL, "size of the existing blockblob does not match");
                goto clean;
            }
            if (sig) { // check the signature, if there
                int sig_size;
                if ((sig_size=read_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb->id, buf, sizeof (buf)))!=strlen(sig)
                    ||
                    (strncmp (sig, buf, sig_size) != 0)) {
                    err (BLOBSTORE_ERROR_SIGNATURE, NULL);
                    goto clean;
                }
            }
        }

        // create a loopback device, if there isn't one already
        char lo_dev [PATH_MAX] = "";
        _err_off(); // do not care if loopback file does not exist
        read_blockblob_metadata_path (BLOCKBLOB_PATH_LOOPBACK, bs, bb->id, lo_dev, sizeof (lo_dev));
        _err_on();
        if (strlen (lo_dev) > 0) {
            struct stat sb;
            if (stat (lo_dev, &sb) == -1) {
                err (BLOBSTORE_ERROR_UNKNOWN, "blockblob loopback device is recorded but does not exist");
                goto clean;
            }
            if (!S_ISBLK(sb.st_mode)) {
                err (BLOBSTORE_ERROR_UNKNOWN, "blockblob loopback path is not a block device");
                goto clean;
            }
        } else {
            if (diskutil_loop (bb->blocks_path, 0, lo_dev, sizeof (lo_dev))) {
                err (BLOBSTORE_ERROR_UNKNOWN, "failed to obtain a loopback device for a blockblob");
                goto clean;
            }
            write_blockblob_metadata_path (BLOCKBLOB_PATH_LOOPBACK, bs, bb->id, lo_dev);
        }
        set_device_path (bb); // read .dm and .loopback and set bb->device_path accordingly

    } else { // failed to open blobstore
        goto clean;
    }

    if (blobstore_unlock(bs)==-1) {
        err (BLOBSTORE_ERROR_UNKNOWN, "failed to unlock the blobstore");
    }
    goto out;

    int saved_errno = 0;
 clean:

    saved_errno = _blobstore_errno; // save it because close_and_unlock() or delete_blockblob_files() may reset it
    if (bb->fd!=-1) {
        close_and_unlock (bb->fd); 
    }
    if (created_directory || created_blob) { // only delete disk state if we created it
        delete_blockblob_files (bs, bb->id);
    }
    if (saved_errno) {
        _blobstore_errno = saved_errno;
    }

 unlock:
    saved_errno = _blobstore_errno; // save it because 
    if (blobstore_unlock (bs)==-1) {
        err (BLOBSTORE_ERROR_UNKNOWN, "failed to unlock the blobstore");
    }
    if (saved_errno) {
        _blobstore_errno = saved_errno;
    }

 free:
    if (bb) {
        free (bb);
        bb = NULL;
    }

 out:
    free_bbs (bbs);
    return bb;
}

static int loop_remove ( blobstore * bs, const char * bb_id) 
{
    char path [PATH_MAX] = "";
    int ret = 0;

    _err_off(); // do not care if loopback file does not exist
    read_blockblob_metadata_path (BLOCKBLOB_PATH_LOOPBACK, bs, bb_id, path, sizeof (path)); // loads path of /dev/loop?
    _err_on();
    
    if (strlen (path)) {
        if (diskutil_unloop (path)) {
            err (BLOBSTORE_ERROR_UNKNOWN, "failed to remove loopback device for blockblob");
            ret = -1;
        } else {
            set_blockblob_metadata_path (BLOCKBLOB_PATH_LOOPBACK, bs, bb_id, path, sizeof(path)); // load path of .../loopback file itself
            unlink (path);
        }
    }

    return ret;
}

// releases the blob locks, allowing others to open() it, and frees the blockblob handle
int blockblob_close ( blockblob * bb )
{
    if (bb==NULL) {
        err (BLOBSTORE_ERROR_INVAL,NULL);
        return -1;
    }
    int ret = 0;
    
    // do not remove /dev/loop* if it is used by device mapper 
    // (mapped to other blobs or as backing for this one)
    int in_use = check_in_use (bb->store, bb->id, 0);
    if ( !(in_use & (BLOCKBLOB_STATUS_MAPPED|BLOCKBLOB_STATUS_BACKED)) ) { 
        ret = loop_remove (bb->store, bb->id);
    }
    ret |= close_and_unlock (bb->fd);
    free (bb);
    return ret;
}

static int dm_suspend_resume (const char * dev_name)
{
    char cmd [1024];

    snprintf (cmd, sizeof (cmd), "%s suspend %s", DMSETUP, dev_name);
    int status = system (cmd);
    if (status == -1 || WEXITSTATUS(status) != 0) {
        err (BLOBSTORE_ERROR_UNKNOWN, "failed to suspend device with 'dmsetup'");
        return -1;
    }
    snprintf (cmd, sizeof (cmd), "%s resume %s", DMSETUP, dev_name);
    status = system (cmd);
    if (status == -1 || WEXITSTATUS(status) != 0) {
        err (BLOBSTORE_ERROR_UNKNOWN, "failed to resume device with 'dmsetup'");
        return -1;
    }
    return 0;
}

static int dm_delete_devices (char * dev_names[], int size)
{
    if (size<1) return 0;
    int ret = 0;

    // construct list of device names in the order that they should be removed
    int devices = 0;
    char ** dev_names_removable = calloc (size, sizeof(char *));
    if (dev_names_removable==NULL) {
        err (BLOBSTORE_ERROR_NOMEM, NULL);
        return -1;
    }
    for (int i=size-1; i>=0; i--) {
        char * name = dev_names [i];
        int seen = 0;
        for (int j=i+1; j<size; j++) {
            if (!strcmp (name, dev_names [j])) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            dev_names_removable [devices++] = name;
        }
    }

    for (int i=0; i<devices; i++) {
        char cmd [1024];
        int retries = 1;
    try_again:
        snprintf (cmd, sizeof (cmd), "%s remove %s", DMSETUP, dev_names_removable [i]);
        int status = system (cmd);
        if (status == -1 || WEXITSTATUS(status) != 0) {
            if (retries--) {
                usleep (100);
                goto try_again;
            }
            err (BLOBSTORE_ERROR_UNKNOWN, "failed to remove device mapper device with 'dmsetup'");
            ret = -1;
        }
    }
    free (dev_names_removable);

    return ret;
}

static int dm_create_devices (char * dev_names[], char * dm_tables[], int size)
{
    int i;

    for (i=0; i<size; i++) {    
        int pipefds [2];
        
        if (pipe (pipefds) == -1) {
            propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
            goto cleanup;
        }

        pid_t cpid = fork();

        if (cpid<0) {
            propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
            goto cleanup;

        } else if (cpid==0) { // child
            close (pipefds [1]);
            if (dup2 (pipefds [0], 0) == -1) {
                propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
                _exit (1);
            }
            _exit (execl (DMSETUP, DMSETUP, "create", dev_names[i], NULL));

        } else { // parent
            close (pipefds [0]);
            write (pipefds [1], dm_tables [i], strlen (dm_tables [i]));
            close (pipefds [1]);
            /*
            printf ("%s create %s\n===\n", DMSETUP, dev_names[i]);
            write (1, dm_tables [i], strlen (dm_tables [i]));
            printf ("===\n");
            fsync (1);
            */
            int status;
            if (waitpid (cpid, &status, 0) == -1) {
                propagate_system_errno (BLOBSTORE_ERROR_UNKNOWN);
                goto cleanup;
            }
            if (WEXITSTATUS(status) != 0) {
                err (BLOBSTORE_ERROR_UNKNOWN, "failed to set up device mapper table with 'dmsetup'");
                goto cleanup;
            }

        }
    }

    return 0;
 cleanup:
    _err_off();
    dm_delete_devices (dev_names, i+1);
    _err_on();
    return -1;
}

static char * dm_get_zero (void)
{
    static char dev_zero [] = DM_PATH EUCA_ZERO; 
    
    struct stat sb;
    int tried = 0;
    while (stat (dev_zero, &sb)==-1) {
        if (tried) {
            err (BLOBSTORE_ERROR_UNKNOWN, "failed to create blockblob zero block device");
            return NULL;
        }
        
        char * dm_tables [1] = { "0 " EUCA_ZERO_SIZE " zero" };
        char * dm_names  [1] = { EUCA_ZERO };
        dm_create_devices (dm_names, dm_tables, 1);
        
        tried = 1;
    }

    if (!S_ISBLK(sb.st_mode)) {
        err (BLOBSTORE_ERROR_UNKNOWN, "blockblob zero is not a block device");
        return NULL;
    }
    
    return dev_zero;
}

// if no outside references to the blob exist, and blob is not protected, 
// deletes the blob, its metadata, and frees the blockblob handle
int blockblob_delete ( blockblob * bb, long long timeout_usec )
{
    char ** array = NULL;
    int array_size = 0;

    if (bb==NULL) {
        err (BLOBSTORE_ERROR_INVAL,NULL);
        return -1;
    }
    blobstore * bs = bb->store;
    int ret = 0;
    if (blobstore_lock(bs, timeout_usec)==-1) { // lock it so we can traverse it (TODO: move this into creation-only section?)
        ret = -1;
        goto error; // failed to obtain a lock on the blobstore
    }

    // do not delete the blob if it is used by another one
    bb->in_use = check_in_use (bs, bb->id, timeout_usec); // update in_use status
    if (bb->in_use & ~(BLOCKBLOB_STATUS_OPENED|BLOCKBLOB_STATUS_BACKED)) { // in use other than opened or backed (by us)
        err (BLOBSTORE_ERROR_AGAIN, NULL);
        ret = -1;
        goto unlock;
    }

    // delete dm devices listed in .dm of this blob
    if (read_array_blockblob_metadata_path (BLOCKBLOB_PATH_DM, bb->store, bb->id, &array, &array_size)==-1
        ||
        dm_delete_devices (array, array_size)==-1) {
        ret = -1;
        goto unlock;
    }
    for (int i=0; i<array_size; i++) {
        free (array[i]);
    }
    if (array)
        free (array);
    array_size = 0;
    array = NULL;

    // update .refs on dependencies
    if (read_array_blockblob_metadata_path (BLOCKBLOB_PATH_DEPS, bb->store, bb->id, &array, &array_size)==-1) {
        ret = -1;
        goto unlock;
    }
    char my_ref [BLOBSTORE_MAX_PATH+MAX_DM_NAME+1];
    snprintf (my_ref, sizeof (my_ref), "%s %s", bb->store->path, bb->id);
    for (int i=0; i<array_size; i++) {
        char * store_path = array [i];
        char * blob_id = strrchr (array [i], ' ');
        if (blob_id) {
            * blob_id = '\0';
            blob_id++;
        }
        if (strlen (store_path)<1 || strlen (blob_id)<1)
            continue; // TODO: print a warning about store/blob corruption?

        blobstore * dep_bs = bs;
        if (strcmp (bs->path, store_path)) { // if deleting reference in a different blobstore
            // need to open it
            dep_bs = blobstore_open (store_path, 0, BLOBSTORE_FORMAT_ANY, BLOBSTORE_REVOCATION_ANY, BLOBSTORE_SNAPSHOT_ANY);
            if (dep_bs == NULL)
                continue; // TODO: print a warning about store/blob corruption?
        }

        // update .refs on deps (TODO: put other .refs updates under global lock?)
        if (update_entry_blockblob_metadata_path (BLOCKBLOB_PATH_REFS, dep_bs, blob_id, my_ref, 1)==-1) {
            // TODO: print a warning about store/blob corruption?
        }

        if (!check_in_use(dep_bs, blob_id, 0)) {
            loop_remove (dep_bs, blob_id); // TODO: do we care about errors?
        }
        if (dep_bs != bs) {
            blobstore_close (dep_bs);
        }
    }
    
    if (loop_remove (bs, bb->id) == -1) {
        ret = -1;
    }
    ret |= close_and_unlock (bb->fd);
    ret |= (delete_blockblob_files (bs, bb->id)<1)?(-1):(0);
    free (bb);
    
    int saved_errno = 0;
 unlock:
    saved_errno = _blobstore_errno; // save it because 
    if (blobstore_unlock (bs)==-1) {
        err (BLOBSTORE_ERROR_UNKNOWN, "failed to unlock the blobstore");
    }
    if (saved_errno) {
        _blobstore_errno = saved_errno;
    }

    for (int i=0; i<array_size; i++) {
        free (array[i]);
    }
    if (array)
        free (array);

 error:
    return ret;
}

int blockblob_clone ( blockblob * bb, // destination blob, which blocks may be used as backing
                      const blockmap * map, // map of blocks from other blobs/devices to be copied/mapped/snapshotted
                      unsigned int map_size ) // size of the map []
{
    int ret = 0;

    if (bb==NULL) {
        err (BLOBSTORE_ERROR_INVAL, "blockblob pointer is NULL");
        return -1;
    }
    if (map==NULL || map_size<1 || map_size>MAX_BLOCKMAP_SIZE) {
        err (BLOBSTORE_ERROR_INVAL, "invalid blockbmap or its size");
        return -1;
    }

    // verify dependencies (block devices present, blob sizes make sense, zero device present)
    char * zero_dev = NULL;
    for (int i=0; i<map_size; i++) {
        const blockmap * m = map + i;

        if (m->relation_type!=BLOBSTORE_COPY && bb->store->snapshot_policy!=BLOBSTORE_SNAPSHOT_DM) {
            err (BLOBSTORE_ERROR_INVAL, "relation type is incompatible with snapshot policy");
            return -1;
        }

        switch (m->source_type) {
        case BLOBSTORE_DEVICE: {
            const char * path = m->source.device_path;
            if (path==NULL) {
                err (BLOBSTORE_ERROR_INVAL, "one of the device paths is NULL");
                return -1;
            }
            struct stat sb;
            if (stat (path, &sb)==-1) {
                propagate_system_errno (BLOBSTORE_ERROR_NOENT);
                return -1;
            }
            if (!S_ISBLK(sb.st_mode)) {
                err (BLOBSTORE_ERROR_INVAL, "one of the device paths is not a block device");
                return -1;
            }
            break;
        }
        case BLOBSTORE_BLOCKBLOB: {
            const blockblob * sbb = m->source.blob;
            if (sbb==NULL) {
                err (BLOBSTORE_ERROR_INVAL, "one of the source blockblob pointers is NULL");
                return -1;
            }
            if (sbb->fd==-1) {
                err (BLOBSTORE_ERROR_INVAL, "one of the source blockblobs is not open");
                return -1;
            }
            struct stat sb;
            if (fstat (sbb->fd, &sb)==-1) {
                propagate_system_errno (BLOBSTORE_ERROR_NOENT);
                return -1;
            }
            if (sb.st_size/512 < sbb->size_blocks) {
                err (BLOBSTORE_ERROR_INVAL, "one of the source blockblobs has backing that is too small");
                return -1;
            }
            if (stat (sbb->device_path, &sb)==-1) {
                propagate_system_errno (BLOBSTORE_ERROR_NOENT);
                return -1;
            }
            if (!S_ISBLK(sb.st_mode)) {
                err (BLOBSTORE_ERROR_INVAL, "one of the source blockblobs is missing a loopback block device");
                return -1;
            }
            if (sbb->size_blocks < (m->first_block_src + m->len_blocks)) {
                err (BLOBSTORE_ERROR_INVAL, "one of the source blockblobs is too small for the map");
                return -1;
            }
            if (bb->size_blocks < (m->first_block_dst + m->len_blocks)) {
                err (BLOBSTORE_ERROR_INVAL, "the destination blockblob is too small for the map");
                return -1;
            }
            if (m->relation_type==BLOBSTORE_SNAPSHOT && m->len_blocks < MIN_BLOCKS_SNAPSHOT) {
                err (BLOBSTORE_ERROR_INVAL, "snapshot size is too small");
                return -1;
            }
            break;
        } 
        case BLOBSTORE_ZERO:
            if (m->relation_type!=BLOBSTORE_COPY) {
                zero_dev = dm_get_zero ();
                if (zero_dev == NULL) {
                    return -1;
                }
            }
            break;
        default:
            err (BLOBSTORE_ERROR_INVAL, "invalid map entry type");
            return -1;
        }
    }

    // compute the base name of the device mapper device
    char dm_base [MAX_DM_LINE];
    snprintf (dm_base, sizeof(dm_base), "euca-%s", bb->id);
    for (char * c = dm_base; * c != '\0'; c++)
        if ( * c == '/' ) // if the ID has slashes,
            * c = '-'; // replace them with hyphens

    int devices = 0;
    int mapped_or_snapshotted = 0;
    char buf [MAX_DM_LINE];
    char * main_dm_table = NULL;
    char ** dev_names = calloc (map_size*4+1, sizeof(char *)); // for device mapper dev names we will create
    if (dev_names==NULL) {
        err (BLOBSTORE_ERROR_NOMEM, NULL);
        return -1;
    }
    char ** dm_tables = calloc (map_size*4+1, sizeof(char *)); // for device mapper tables 
    if (dm_tables==NULL) {
        err (BLOBSTORE_ERROR_NOMEM, NULL);
        return -1;
    }

    // either does copies or computes the device mapper tables
    for (int i=0; i<map_size; i++) {
        const blockmap * m = map + i;
        const blockblob * sbb = m->source.blob;
        const char * dev;

        switch (m->source_type) {
        case BLOBSTORE_DEVICE:
            dev = m->source.device_path; 
            break;
        case BLOBSTORE_BLOCKBLOB:
            dev = m->source.blob->device_path; 
            break;
        case BLOBSTORE_ZERO:
            dev = zero_dev;
            break;
        default:
            err (BLOBSTORE_ERROR_INVAL, "invalid device map source type");
            ret = -1;
            goto free;
        } 

        long long first_block_src = m->first_block_src;
        switch (m->relation_type) {
        case BLOBSTORE_COPY:
            // do the copy
            if (diskutil_dd2 (dev, bb->device_path, 512, m->len_blocks, m->first_block_dst, m->first_block_src)) {
                err (BLOBSTORE_ERROR_INVAL, "failed to copy a section");
                ret = -1;
                goto free;
            }
            // append to the main dm table (we do this here even if we never end up using the device mapper because all segments were copied)
            snprintf (buf, sizeof(buf), "%lld %lld linear %s %lld\n", m->first_block_dst, m->len_blocks, bb->device_path, m->first_block_dst);
            main_dm_table = strdupcat (main_dm_table, buf);
            break;

        case BLOBSTORE_SNAPSHOT: {
            int granularity = 16; // coarser granularity does not work
            while (m->len_blocks % granularity) { // do we need to do this?
                granularity /= 2;
            }

            // with a linear map, create a backing device for the snapshot
            snprintf (buf, sizeof(buf), "%s-p%d-back", dm_base, i);
            dev_names [devices] = strdup (buf);
            char * backing_dev = dev_names [devices];
            snprintf (buf, sizeof(buf), "0 %lld linear %s %lld\n", m->len_blocks, bb->device_path, m->first_block_dst);
            dm_tables [devices] = strdup (buf);
            devices++;

            // if there is an offset in the source device, create another map (since snapshots cannot be done at offsets)
            const char * snapshotted_dev = dev;
            if (m->first_block_src > 0 && m->source_type != BLOBSTORE_ZERO) {
                snprintf (buf, sizeof(buf), "%s-p%d-real", dm_base, i);
                dev_names [devices] = strdup (buf);
                snapshotted_dev = dev_names [devices];
                snprintf (buf, sizeof(buf), "0 %lld linear %s %lld\n", m->len_blocks, dev, m->first_block_src);
                dm_tables [devices] = strdup (buf);
                devices++;
            }

            // take a snapshot of the source
            snprintf (buf, sizeof(buf), "%s-p%d-snap", dm_base, i);
            dev_names [devices] = strdup (buf);
            dev = dev_names [devices];
            snprintf (buf, sizeof(buf), "0 %lld snapshot %s%s " DM_PATH "%s p %d\n", m->len_blocks, snapshotted_dev[0]=='e'?DM_PATH:"", snapshotted_dev, backing_dev, granularity);
            dm_tables [devices] = strdup (buf);
            devices++;
            
            first_block_src = 0; // for snapshots the mapping goes from the -snap device at offset 0
            // yes, fall through
        }

        case BLOBSTORE_MAP:
            // append to the main dm table
            snprintf (buf, sizeof(buf), "%lld %lld linear %s%s %lld\n", m->first_block_dst, m->len_blocks, dev[0]=='e'?DM_PATH:"", dev, first_block_src);
            main_dm_table = strdupcat (main_dm_table, buf);
            mapped_or_snapshotted++;
            break;

        default:
            err (BLOBSTORE_ERROR_INVAL, "invalid device map source type");
            ret = -1;
            goto free;
        }
    }

    if (mapped_or_snapshotted) { // we must use the device mapper
        strncpy (bb->dm_name, dm_base, sizeof(bb->dm_name));
        dev_names [devices] = strdup (dm_base);
        dm_tables [devices] = main_dm_table;
        devices++;

        // change device_path from loopback to the device-mapper path
        snprintf (bb->device_path, sizeof(bb->device_path), DM_FORMAT, dm_base);

        if (dm_create_devices (dev_names, dm_tables, devices)) {
            ret = -1;
            goto free;
        }

        // record new devices in .dm of this blob
        if (write_array_blockblob_metadata_path (BLOCKBLOB_PATH_DM, bb->store, bb->id, dev_names, devices)==-1) {
            ret = -1;
            goto cleanup;
        }
        
        // update .refs on dependencies and create .deps for this blob
        char my_ref [BLOBSTORE_MAX_PATH+MAX_DM_NAME+1];
        snprintf (my_ref, sizeof (my_ref), "%s %s", bb->store->path, bb->id); // TODO: use store ID to proof against moving blobstore?
        for (int i=0; i<map_size; i++) {
            const blockmap * m = map + i;
            const blockblob * sbb = m->source.blob;
            
            if (m->source_type != BLOBSTORE_BLOCKBLOB) // only blobstores have references
                continue;

            if (m->relation_type == BLOBSTORE_COPY) // copies do not create references
                continue;
            
            // update .refs
            if (update_entry_blockblob_metadata_path (BLOCKBLOB_PATH_REFS, sbb->store, sbb->id, my_ref, 0)==-1) {
                ret = -1;
                goto cleanup; // TODO: remove .refs entries from this batch that succeeded, if any?
            }
            
            // record the dependency in .deps (redundant entries will be filtered out)
            char dep_ref [BLOBSTORE_MAX_PATH+MAX_DM_NAME+1];
            snprintf (dep_ref, sizeof (dep_ref), "%s %s", sbb->store->path, sbb->id);
            if (update_entry_blockblob_metadata_path (BLOCKBLOB_PATH_DEPS, bb->store, bb->id, dep_ref, 0)==-1) {
                ret = -1;
                goto cleanup; // ditto
            }
        }
    }

    goto free;

 cleanup:
    // TODO: delete .dm file?
    dm_delete_devices (dev_names, devices);

 free:
    for (int i=0; i<devices; i++) {
        free (dev_names[i]);
        free (dm_tables[i]);
    }
    free (dev_names);
    free (dm_tables);

    return ret;
}

// returns a block device pointing to the blob
const char * blockblob_get_dev ( blockblob * bb )
{
    if (bb==NULL) {
        err (BLOBSTORE_ERROR_INVAL,NULL);
        return NULL;
    }
    return bb->device_path;
}

// returns a path to the file containg the blob, but only if snapshot_type={ANY|NONE}
const char * blockblob_get_file ( blockblob * bb )
{
    if (bb==NULL) {
        err (BLOBSTORE_ERROR_INVAL,NULL);
        return NULL;
    }
    if (bb->snapshot_type!=BLOBSTORE_SNAPSHOT_ANY && bb->snapshot_type!=BLOBSTORE_SNAPSHOT_DM) {
        err (BLOBSTORE_ERROR_INVAL,"device paths only supported for blockblobs with snapshots");
        return NULL;
    }
    return bb->blocks_path;
}

// size of blob in blocks
unsigned long long blockblob_get_size ( blockblob * bb)
{
    if (bb==NULL) {
        err (BLOBSTORE_ERROR_INVAL,NULL);
        return 0;
    }
    return bb->size_blocks;
}

/////////////////////////////////////////////// unit testing code ///////////////////////////////////////////////////

#ifdef _UNIT_TEST

#define F1 "/tmp/blobstore_test_1"
#define F2 "/tmp/blobstore_test_2"
#define F3 "/tmp/blobstore_test_3"

#define _UNEXPECTED printf ("======================> UNEXPECTED RESULT (errors=%d)!!!\n", ++errors);

#define _CHKMETA(ST,RE) snprintf (entry_path, sizeof(entry_path), "%s/%s", bs->path, ST); \
    if (RE!=typeof_blockblob_metadata_path (bs, entry_path, blob_id, sizeof(blob_id))) _UNEXPECTED;

#define _OPEN(FD,FI,FL,TI,RE) _blobstore_errno=0;                       \
    printf ("%d: open (" FI " flags=%d timeout=%d)", getpid(), FL, TI); \
    FD=open_and_lock(FI,FL,TI,0600);                                    \
    printf ("=%d errno=%d '%s'\n", FD, _blobstore_errno, blobstore_get_error_str(_blobstore_errno)); \
    if ((FD==-1) && (_blobstore_errno==0)) printf ("======================> UNSET errno ON ERROR (errors=%d)!!!\n", ++errors); \
    else if ((RE==-1 && FD!=-1) || (RE==0 && FD<0)) _UNEXPECTED;

#define _CLOS(FD,FI) ret=close_and_unlock(FD); \
    printf ("%d: close (%d " FI ")=%d\n", getpid(), FD, ret); 

#define _PARENT_WAITS \
            int status, ret; \
            printf ("waiting for child pid=%d\n", pid); \
            ret = wait (&status); \
            printf ("waited for child pid=%d ret=%d\n", ret, WEXITSTATUS(status)); \
            errors += WEXITSTATUS(status);

#define _R BLOBSTORE_FLAG_RDONLY
#define _W BLOBSTORE_FLAG_RDWR
#define _C BLOBSTORE_FLAG_CREAT|BLOBSTORE_FLAG_EXCL|BLOBSTORE_FLAG_RDWR
#define _CBB BLOBSTORE_FLAG_CREAT|BLOBSTORE_FLAG_EXCL

#define B1 "BLOCKBLOB-01"
#define B2 "FOO/BLOCKBLOB-02"
#define B3 "FOO/BAR/BLOCKBLOB-03"
#define B4 "FOO/BAR/BAZ/BLOCKBLOB-04"
#define B5 "BLOCKBLOB-05"
#define B6 "BLOCKBLOB-06"

#define _OPENBB(BB,ID,SI,SG,FL,TI,RE) _blobstore_errno=0;               \
    printf ("%d: bb_open (%s size=%d flags=%d timeout=%d)", getpid(), (ID==NULL)?("null"):(ID), SI, FL, TI); \
    BB=blockblob_open (bs, ID, SI, FL, SG, TI); \
    printf ("=%s errno=%d '%s'\n", (BB==NULL)?("NULL"):("OK"), _blobstore_errno, blobstore_get_error_str(_blobstore_errno)); \
    if ((BB==NULL) && (_blobstore_errno==0)) printf ("======================> UNSET errno ON ERROR (errors=%d)!!!\n", ++errors); \
    else if ((RE==-1 && BB!=NULL) || (RE==0 && BB==NULL)) _UNEXPECTED;

#define _CLOSBB(BB,ID) ret=blockblob_close(BB); \
    printf("%d: bb_close (%lu %s)=%d errno=%d '%s'\n", getpid(), (unsigned long)BB, (ID==NULL)?("null"):(ID), ret, _blobstore_errno, blobstore_get_error_str(_blobstore_errno));

#define _DELEBB(BB,ID,RE) ret=blockblob_delete(BB, 3000);                 \
    printf("%d: bb_delete (%lu %s)=%d errno=%d '%s'\n", getpid(), (unsigned long)BB, (ID==NULL)?("null"):(ID), ret, _blobstore_errno, blobstore_get_error_str(_blobstore_errno)); \
    if (ret!=RE) _UNEXPECTED;

#define _CLONBB(BB,ID,MP,RE) _blobstore_errno=0; \
    printf ("%d: bb_clone (%s map=%lu)", getpid(), (ID==NULL)?("null"):(ID), (unsigned long)MP); \
    ret=blockblob_clone(BB,MP,sizeof(MP)/sizeof(blockmap));    \
    printf ("=%d errno=%d '%s'\n", ret, _blobstore_errno, blobstore_get_error_str(_blobstore_errno)); \
    if ((ret==-1) && (_blobstore_errno==0)) printf ("======================> UNSET errno ON ERROR (errors=%d)!!!\n", ++errors); \
    else if (RE!=ret) _UNEXPECTED;
    
#define BS_SIZE 30
#define BB_SIZE 10
#define CBB_SIZE 32
#define STRESS_BS_SIZE 1000000
#define STRESS_MIN_BB  64
#define STRESS_BLOBS   80

static void _fill_blob (blockblob * bb, char c)
{
    const char * path = blockblob_get_dev (bb);
    char buf [1];
    buf [0] = c;

    int fd = open (path, O_WRONLY);
    if (fd!=-1) {
        for (int i=0; i<bb->size_blocks*512; i++) {
            write (fd, buf, 1);
        }
    }
    fsync (fd);
    close (fd);
}

static blobstore * create_teststore (int size, const char * base, const char * name, blobstore_format_t format, blobstore_revocation_t revocation, blobstore_snapshot_t snapshot)
{
    static int ts = 0;
    static int counter = 0;

    if (ts==0) {
        ts = ((int)time(NULL))-1292630988;
        //ts = (((int)time(NULL))<<24)>>24;
    }

    char bs_path [PATH_MAX];
    snprintf (bs_path, sizeof (bs_path), "%s/test_blobstore_%05d_%s_%03d", base, ts, name, counter++);
    if (mkdir (bs_path, BLOBSTORE_DEFAULT_UMASK) == -1) {
        printf ("failed to create %s\n", bs_path);
        return NULL;
    }
    printf ("created %s\n", bs_path);
    blobstore * bs = blobstore_open (bs_path, size, format, revocation, snapshot);
    if (bs==NULL) {
        printf ("ERROR: %s\n", blobstore_get_error_str(blobstore_get_error()));
        return NULL;
    }
    return bs;
}

static int write_byte (blockblob *bb, int seek, char c)
{
    const char * dev = blockblob_get_dev (bb);
    int fd = open (dev, O_WRONLY);
    if (fd==-1) { 
        printf ("ERROR: failed to open the blockblob dev %s\n", dev);
        return -1;
    }
    if (lseek (fd, seek, SEEK_SET)==-1) {
        printf ("ERROR: failed to lseek in blockblob dev %s\n", dev);
        close (fd);
        return -1;
    }
    if (write (fd, &c, 1)!=1) {
        printf ("ERROR: failed to write to blockblob dev %s\n", dev);
        close (fd);
        return -1;
    }
    fsync (fd);
    close (fd);

    return 0;
}

static char read_byte (blockblob *bb, int seek)
{
    const char * dev = blockblob_get_dev (bb);
    int fd = open (dev, O_RDONLY);
    if (fd==-1) { 
        printf ("ERROR: failed to open the blockblob dev %s\n", dev);
        return -1;
    }
    if (lseek (fd, seek, SEEK_SET)==-1) {
        printf ("ERROR: failed to lseek in blockblob dev %s\n", dev);
        close (fd);
        return -1;
    }
    char buf [1];
    if (read (fd, buf, 1)!=1) {
        printf ("ERROR: failed to write to blockblob dev %s\n", dev);
        close (fd);
        return -1;
    }
    close (fd);

    return buf [0];
}

static int do_clone_stresstest (const char * base, const char * name, blobstore_format_t format, blobstore_revocation_t revocation, blobstore_snapshot_t snapshot)
{
    int ret;
    int errors = 0;
    printf ("commencing cloning stress-test...\n");

    blobstore * bs1 = create_teststore (STRESS_BS_SIZE, base, name, BLOBSTORE_FORMAT_DIRECTORY, BLOBSTORE_REVOCATION_NONE,  BLOBSTORE_SNAPSHOT_DM);
    if (bs1==NULL) { errors++; goto done; }
    blobstore * bs2 = create_teststore (STRESS_BS_SIZE, base, name, BLOBSTORE_FORMAT_DIRECTORY, BLOBSTORE_REVOCATION_LRU, BLOBSTORE_SNAPSHOT_DM);
    if (bs2==NULL) { errors++; goto done; }

    blockblob * bbs1 [STRESS_BLOBS];
    long long bbs1_sizes [STRESS_BLOBS];
    blockblob * bbs2 [STRESS_BLOBS*2];
    long long bbs2_sizes [STRESS_BLOBS*2];

    // calculate sizes
    long long avg = STRESS_BS_SIZE / STRESS_BLOBS;
    if (avg < STRESS_MIN_BB*2) {
        printf ("ERROR: average blob size %lld for stress test is too small (<%d)\n", avg, STRESS_MIN_BB*2);
        errors++;
        goto done;
    }
    for (int i=0; i<STRESS_BLOBS; i++) { 
        bbs1_sizes [i] = avg;
        bbs1 [i] = NULL;
        bbs2 [i] = NULL;
        bbs2 [i+STRESS_BLOBS] = NULL;
    }
    srandom (time(NULL));
    for (int i=0; i<STRESS_BLOBS*3; i++) { // run over the array a few times
        int j = i % (STRESS_BLOBS/2); // modify pairs from array
        int k = j + (STRESS_BLOBS/2);
#define MIN(a,b) a>b?b:a
        long long max_delta = MIN(bbs1_sizes[j]-STRESS_MIN_BB, bbs1_sizes[k]-STRESS_MIN_BB);
        long long delta = max_delta*(((double)random()/RAND_MAX)-0.5);
        bbs1_sizes [j] -= delta;
        bbs2_sizes [j] = bbs1_sizes [j]/2;
        bbs2_sizes [j+STRESS_BLOBS] = bbs1_sizes [j] - bbs1_sizes [j]/2;

        bbs1_sizes [k] += delta;
        bbs2_sizes [k] = bbs1_sizes [k]/2;
        bbs2_sizes [k+STRESS_BLOBS] = bbs1_sizes [k] - bbs1_sizes [k]/2;
    }
    long long bbs1_totals = 0;
    for (int i=0; i<STRESS_BLOBS; i++) {
        bbs1_totals += bbs1_sizes [i];
        long long pair = bbs2_sizes [i] +  bbs2_sizes [i+STRESS_BLOBS];
        assert (pair==bbs1_sizes [i]);
        printf ("%lld ", bbs1_sizes [i]);
    }
    assert (bbs1_totals==STRESS_BS_SIZE);
    printf ("\n");

    // fill the stores
    for (int i=0; i<STRESS_BLOBS; i++) {
#define _OPENERR(BS,BB,BBSIZE)                                          \
        BB = blockblob_open (BS, NULL, BBSIZE, BLOBSTORE_FLAG_CREAT | BLOBSTORE_FLAG_EXCL, NULL, 1000); \
        if (BB == NULL) {                                               \
            printf ("ERROR: failed to create blockblob i=%d\n", i);       \
            errors++;                                                   \
            goto drain;                                                 \
        }
        printf ("allocating slot %d\n", i);
        _OPENERR(bs1,bbs1[i],bbs1_sizes[i]);
        _OPENERR(bs2,bbs2[i],bbs2_sizes[i]);
        _OPENERR(bs2,bbs2[i+STRESS_BLOBS],bbs2_sizes[i+STRESS_BLOBS]);
        write_byte (bbs2[i+STRESS_BLOBS], 0, 'b'); // write a byte into beginning of blob that will be snapshotted
        blockmap map [] = {
            {BLOBSTORE_MAP,      BLOBSTORE_BLOCKBLOB, {blob:bbs2[i]},              0, 0, bbs2_sizes[i]},
            {BLOBSTORE_SNAPSHOT, BLOBSTORE_BLOCKBLOB, {blob:bbs2[i+STRESS_BLOBS]}, 0, bbs2_sizes[i], bbs2_sizes[i+STRESS_BLOBS]},
        };        
        if (blockblob_clone (bbs1[i], map, 2)==-1) {
            printf ("ERROR: failed to clone on iteration %i\n", i);
            errors++;
            goto drain;
        }
        // verify that mapping works
        write_byte (bbs2[i], bbs2_sizes[i]*512-1, 'a'); // write a byte into the end of the blob that is being mapped
        dm_suspend_resume (bbs1[i]->dm_name);
        char c1 = read_byte (bbs1[i], bbs2_sizes[i]*512-1); // read that byte back via bbs1
        char c2 = read_byte (bbs1[i], bbs2_sizes[i]*512); // read the byte written before the snapshot
        if (c1!='a' || c2!='b') {
            printf ("ERROR: clone verification failed (c1=='%c', c2=='%c')\n", c1, c2);
            errors++;
            goto drain;
        }
    }
    
    // induce churn in stores
    for (int k=0; k<STRESS_BLOBS*1; k++) {
        usleep (100);
        // randomly free a few random blobs
        int to_free = (int)((STRESS_BLOBS/2)*((double)random()/RAND_MAX));
        printf ("will free %d random blobs\n", to_free);
        for (int j=0; j<to_free; j++) {
            int i = (int)((STRESS_BLOBS-1)*((double)random()/RAND_MAX));
            if (bbs1 [i] != NULL) {
                printf ("freeing slot %d\n", i);
#define _DELWARN(BB) if (BB && blockblob_delete (BB, 1000) == -1) { printf ("WARNING: failed to delete blockblob %s i=%d\n", BB->id, i); } BB=NULL
                _DELWARN(bbs1[i]);
                blockblob_close (bbs2[i]); // so it can be purged with LRU 
                bbs2[i] = NULL;
                blockblob_close (bbs2[i+STRESS_BLOBS]); // so it can be purged with LRU 
                bbs2[i+STRESS_BLOBS]= NULL;
            }
        }
        
        // re-allocate those sizes
        for (int i=0; i<STRESS_BLOBS; i++) {
            if (bbs1 [i]!=NULL)
                continue;
            printf ("allocating slot %d\n", i);
            _OPENERR(bs1,bbs1[i],bbs1_sizes[i]);
            _OPENERR(bs2,bbs2[i],bbs2_sizes[i]);
            _OPENERR(bs2,bbs2[i+STRESS_BLOBS],bbs2_sizes[i+STRESS_BLOBS]);
            write_byte (bbs2[i+STRESS_BLOBS], 0, 'b'); // write a byte into beginning of blob that will be snapshotted
            blockmap map [] = {
                {BLOBSTORE_MAP,      BLOBSTORE_BLOCKBLOB, {blob:bbs2[i]},              0, 0, bbs2_sizes[i]},
                {BLOBSTORE_SNAPSHOT, BLOBSTORE_BLOCKBLOB, {blob:bbs2[i+STRESS_BLOBS]}, 0, bbs2_sizes[i], bbs2_sizes[i+STRESS_BLOBS]},
            };        
            if (blockblob_clone (bbs1[i], map, 2)==-1) {
                printf ("ERROR: failed to clone on iteration %i\n", i);
                errors++;
                goto drain;
            }
            // verify that mapping works
            write_byte (bbs2[i], bbs2_sizes[i]*512-1, 'a'); // write a byte into the end of the blob that is being mapped
            dm_suspend_resume (bbs1[i]->dm_name);
            char c1 = read_byte (bbs1[i], bbs2_sizes[i]*512-1); // read that byte back via bbs1
            char c2 = read_byte (bbs1[i], bbs2_sizes[i]*512); // read the byte written before the snapshot
            if (c1!='a' || c2!='b') {
                printf ("ERROR: clone verification failed (c1=='%c', c2=='%c')\n", c1, c2);
                errors++;
                goto drain;
            }
        }
    }

    // drain the stores
 drain:
    printf ("resting before draining...\n");
    sleep (1); 
    for (int i=0; i<STRESS_BLOBS; i++) {
        printf ("freeing slot %d\n", i);
        _DELWARN(bbs1[i]);
        _DELWARN(bbs2[i]);
        _DELWARN(bbs2[i+STRESS_BLOBS]);
    }

    blobstore_close (bs1);
    blobstore_close (bs2);

    printf ("completed cloning stress-test\n");
 done:
    return errors;
}

static int do_clone_test (const char * base, const char * name, blobstore_format_t format, blobstore_revocation_t revocation, blobstore_snapshot_t snapshot)
{
    int ret;
    int errors = 0;
    printf ("commencing cloning test\n");

    blobstore * bs = create_teststore (CBB_SIZE*6, base, name, BLOBSTORE_FORMAT_DIRECTORY, BLOBSTORE_REVOCATION_ANY, BLOBSTORE_SNAPSHOT_ANY);
    if (bs==NULL) { errors++; goto done; }

    blockblob * bb1, * bb2, * bb3, * bb4, * bb5, * bb6;

    // these are to be mapped to others
    _OPENBB(bb1,B1,CBB_SIZE,NULL,_CBB,0,0); // bs size: 1
    _fill_blob (bb1, '1');
    _OPENBB(bb2,B2,CBB_SIZE,NULL,_CBB,0,0); // bs size: 2
    _fill_blob (bb2, '2');
    _OPENBB(bb3,B3,CBB_SIZE,NULL,_CBB,0,0); // bs size: 3
    _fill_blob (bb3, '3');

    // these are to be clones
    _OPENBB(bb4,B4,CBB_SIZE*3,NULL,_CBB,0,0); // bs size: 6
    blockmap bm1 [] = { 
        {BLOBSTORE_MAP, BLOBSTORE_BLOCKBLOB, {blob:bb1}, 0, 0, CBB_SIZE},
        {BLOBSTORE_COPY, BLOBSTORE_BLOCKBLOB, {blob:bb2}, 0, CBB_SIZE, CBB_SIZE},
        {BLOBSTORE_SNAPSHOT, BLOBSTORE_BLOCKBLOB, {blob:bb3}, 0, CBB_SIZE*2, CBB_SIZE},
    };
    _CLONBB(bb4,B4,bm1,0);

    // see if cloning worked
    const char * dev = blockblob_get_dev (bb4);
    if (dev!=NULL) {
        int fd = open (dev, O_RDONLY);
        if (fd != -1) {
            for (int i=1; i<4; i++) {
                for (int j=0; j<CBB_SIZE; j++) {
                    char buf [512];
                    int r = read (fd, buf, sizeof (buf));
                    if (r < 1) {
                        printf ("ERROR: failed to read bock device %s\n", dev);
                        errors++;
                        goto stop_comparing;
                    }
                    if (buf [0] != '0' + i) {
                        printf ("ERROR: block device %s has unexpected data ('%c' (%d) != '%c')\n", dev, buf [0], buf [0], '0' + i);
                        errors++;
                        goto stop_comparing;
                    }
                }
            }
        stop_comparing:
            close (fd);
        } else {
            printf ("ERROR: failed to open block device %s for the clone\n", dev);
            errors++;
        }
    } else {
        printf ("ERROR: failed to get a block device for the clone\n");
        errors++;
    }

    _DELEBB(bb1,B1,-1); // referenced, not deletable
    _DELEBB(bb2,B2,0); // not referenced, deletable
    _DELEBB(bb3,B3,-1); // referenced, not deletable
    _CLOSBB(bb3,B3);
    _CLOSBB(bb4,B4);
    _DELEBB(bb1,B1,-1); // still referenced, not deletable
    _OPENBB(bb4,B4,0,NULL,0,0,0); // re-open so we can delete it
    _DELEBB(bb4,B4,0); // delete #4
    _DELEBB(bb1,B1,0); // now it should work
    _OPENBB(bb3,B3,0,NULL,0,0,0); // re-open so we can map it

    // open a second blobstore to test cross-references
    blobstore * bs2 = create_teststore (CBB_SIZE*6, base, name, BLOBSTORE_FORMAT_DIRECTORY, BLOBSTORE_REVOCATION_ANY, BLOBSTORE_SNAPSHOT_ANY);
    if (bs2==NULL) {
        errors++; 
        goto done; 
    }
    bb5 = blockblob_open (bs2, B5, CBB_SIZE*3, BLOBSTORE_FLAG_CREAT, NULL, 0);
    if (bb5==NULL) {
        errors++;
        goto done;
    }

    blockmap bm2 [] = { 
        {BLOBSTORE_SNAPSHOT, BLOBSTORE_BLOCKBLOB, {blob:bb3}, 0, 0, CBB_SIZE},
        {BLOBSTORE_SNAPSHOT, BLOBSTORE_ZERO,      {blob:NULL}, 0, CBB_SIZE, CBB_SIZE},
        //        {BLOBSTORE_SNAPSHOT,      BLOBSTORE_DEVICE,    {device_path:"/dev/sda2"}, 0, CBB_SIZE*2, CBB_SIZE}
    };
    _CLONBB(bb5,B5,bm2,0);
    
    _DELEBB(bb3,B3,-1); // referenced, so not deletable
    _CLOSBB(bb3,B3);
    _OPENBB(bb3,B3,0,NULL,0,0,0); // re-open so we can try to delete it
    _DELEBB(bb3,B3,-1); // ditto
    _CLOSBB(bb3,B3);
    sleep (1); // otherwise the next delete occasionally fails with 'device busy'
    _DELEBB(bb5,B5,0); // delete #5
    _OPENBB(bb3,B3,0,NULL,0,0,0); // re-open so we can finally delete it
    _DELEBB(bb3,B3,0); // should work now

    blobstore_close (bs);
    blobstore_close (bs2);

    printf ("completed cloning test\n");
 done:
    return errors;
}

static int do_metadata_test (const char * base, const char * name)
{
    int ret;
    int errors = 0;

    printf ("\nrunning do_metadata_test()\n");
    
    blobstore * bs = create_teststore (BS_SIZE, base, name, BLOBSTORE_FORMAT_FILES, BLOBSTORE_REVOCATION_ANY, BLOBSTORE_SNAPSHOT_ANY);
    if (bs==NULL) { errors++; goto done; }

    char blob_id [MAX_PATH];
    char entry_path [MAX_PATH];
    _CHKMETA("foo",0);
    _CHKMETA(".dm",0);
    _CHKMETA(".loopback",0);
    _CHKMETA(".sig",0);
    _CHKMETA(".refs",0);
    _CHKMETA(".dmfoo",0);
    _CHKMETA("foo.blocks",BLOCKBLOB_PATH_BLOCKS);
    _CHKMETA("foo.dm",BLOCKBLOB_PATH_DM);
    _CHKMETA("foo.loopback",BLOCKBLOB_PATH_LOOPBACK);
    _CHKMETA("foo.sig",BLOCKBLOB_PATH_SIG);
    _CHKMETA("foo.refs",BLOCKBLOB_PATH_REFS);
    _CHKMETA("foo.dm.foo.dm",BLOCKBLOB_PATH_DM);
    _CHKMETA("foo/dm/dm.foo.loopback",BLOCKBLOB_PATH_LOOPBACK);
    _CHKMETA("foo/dm/dm.dm.sig",BLOCKBLOB_PATH_SIG);
    _CHKMETA("foo/dm/dm.dm.dm.refs",BLOCKBLOB_PATH_REFS);
    _CHKMETA(".dm.dm",BLOCKBLOB_PATH_DM);
    _CHKMETA(".foo.dm",BLOCKBLOB_PATH_DM);
    blobstore_close (bs);

    bs = create_teststore (BS_SIZE, base, name, BLOBSTORE_FORMAT_DIRECTORY, BLOBSTORE_REVOCATION_ANY, BLOBSTORE_SNAPSHOT_ANY);
    if (bs==NULL) { errors++; goto done; }
    _CHKMETA("foo",0);
    _CHKMETA(".dm",0);
    _CHKMETA(".loopback",0);
    _CHKMETA(".sig",0);
    _CHKMETA(".refs",0);
    _CHKMETA(".dmfoo",0);
    _CHKMETA("foo/blocks",BLOCKBLOB_PATH_BLOCKS);
    _CHKMETA("foo/dm",BLOCKBLOB_PATH_DM);
    _CHKMETA("foo/loopback",BLOCKBLOB_PATH_LOOPBACK);
    _CHKMETA("foo/sig",BLOCKBLOB_PATH_SIG);
    _CHKMETA("foo/refs",BLOCKBLOB_PATH_REFS);
    _CHKMETA("foo.dm.foo/dm",BLOCKBLOB_PATH_DM);
    _CHKMETA("foo/dm/dm.foo/loopback",BLOCKBLOB_PATH_LOOPBACK);
    _CHKMETA("foo/dm/dm.dm/sig",BLOCKBLOB_PATH_SIG);
    _CHKMETA("foo/dm/dm.dm.dm/refs",BLOCKBLOB_PATH_REFS);
    _CHKMETA(".dm/dm",BLOCKBLOB_PATH_DM);
    _CHKMETA(".foo/dm",BLOCKBLOB_PATH_DM);
    if (errors) return errors;

    printf ("\ntesting metadata manipulation\n");

    blockblob * bb1;
    _OPENBB(bb1,B1,BB_SIZE,NULL,_CBB,0,0); // bs size: 10
    
    int t = 1;
    char ** array;
    int array_size;
    char buf [1024];
    bzero (buf, sizeof(buf));
#define _BADMETACMD { printf ("UNEXPECTED RESULT LINE %d (errors=%d, errno=%d %s)\n", t, errors++, _blobstore_errno, blobstore_get_error_str(_blobstore_errno)); } t++
#define _STR1 "teststringtwo"
#define _STR2 "test\nstring\none\n"
    /* 1 */ if (read_blockblob_metadata_path  (BLOCKBLOB_PATH_SIG, bs, bb1->id, buf, sizeof(buf)) != -1) _BADMETACMD; // open nonexisting file
    if (write_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, "") != 0) _BADMETACMD; // delete nonexisting file
    if (write_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, _STR1) != 0) _BADMETACMD; // write new file
    if (write_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, _STR2) != 0) _BADMETACMD; // overwrite file
    /* 5 */ if (read_blockblob_metadata_path  (BLOCKBLOB_PATH_SIG, bs, bb1->id, buf, sizeof(buf)) != strlen(_STR2)) _BADMETACMD; // read file
    if (strcmp (buf, _STR2)) _BADMETACMD;
    if (read_array_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, &array, &array_size) != 0) _BADMETACMD; // read file line-by-line
    if (array_size!=3) _BADMETACMD;
    for (int i=0; i<array_size; i++) {
        free (array [i]);
    }
    free (array);
    if (update_entry_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, "test", 1) != 0) _BADMETACMD; // delete first line
    /* 10 */ if (update_entry_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, "one", 1) != 0) _BADMETACMD; // delete last line
    if (update_entry_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, "string", 1) != 0) _BADMETACMD; // delete only line
    if (write_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, "") != 0) _BADMETACMD; // delete existing file
    if (read_blockblob_metadata_path  (BLOCKBLOB_PATH_SIG, bs, bb1->id, buf, sizeof(buf)) != -1) _BADMETACMD; // open nonexisting file
    if (update_entry_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, "test", 0) != 0) _BADMETACMD; // add first line
    /* 15 */ if (update_entry_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, "string", 0) != 0) _BADMETACMD; // add second line
    if (update_entry_blockblob_metadata_path (BLOCKBLOB_PATH_SIG, bs, bb1->id, "one", 0) != 0) _BADMETACMD; // add third line
    if (read_blockblob_metadata_path  (BLOCKBLOB_PATH_SIG, bs, bb1->id, buf, sizeof(buf)) != strlen(_STR2)) _BADMETACMD; // read file
    if (strcmp (buf, _STR2)) _BADMETACMD;
    _CLOSBB(bb1,B1);

    blobstore_close (bs);
    printf ("completed metadata test\n");
 done:
    return errors;
}

static int do_blobstore_test (const char * base, const char * name, blobstore_format_t format, blobstore_revocation_t revocation)
{
    int ret;
    int errors = 0;

    printf ("\ntesting blockblob creation (name=%s, format=%d, revocation=%d)\n", name, format, revocation);

    blobstore * bs = create_teststore (BS_SIZE, base, name, format, revocation, BLOBSTORE_SNAPSHOT_ANY);
    if (bs==NULL) { errors++; goto done; }

    blockblob * bb1, * bb2, * bb3, * bb4, * bb5, * bb6;
    _OPENBB(bb1,NULL,0,NULL,_CBB,0,-1); // creating with size=0,
    _OPENBB(bb1,NULL,BS_SIZE+1,NULL,_CBB,0,-1); // too big for blobstore
    _OPENBB(bb1,NULL,BB_SIZE,NULL,_CBB|BLOBSTORE_FLAG_RDWR,0,-1); // bad flag

    _OPENBB(bb1,B2,BB_SIZE,NULL,_CBB,0,0); // bs size: 10
    sleep(1); // to ensure mod time of bb1 and bb2 is different
    _OPENBB(bb2,B3,BB_SIZE,"sig",_CBB,0,0); // bs size: 20
    _OPENBB(bb3,B1,BB_SIZE,B1,_CBB,0,0); // bs size: 30

    _OPENBB(bb4,NULL,BB_SIZE,B1,0,0,-1); // null ID without create
    _OPENBB(bb4,B1,BB_SIZE+1,B1,0,0,-1); // wrong size
    _OPENBB(bb4,B1,BB_SIZE,"foo",0,0,-1); // wrong sig
    _OPENBB(bb4,NULL,BB_SIZE,NULL,_CBB,0,-1); // blobstore full, all blobs in use
    _CLOSBB(bb1,NULL);
    _CLOSBB(bb2,NULL);
    if (revocation == BLOBSTORE_REVOCATION_LRU) {
        printf ("=== starting revocation sub-test\n");
        _OPENBB(bb4,NULL,BB_SIZE,NULL,_CBB,0,0); // blobstore full, 2 blobs purgeable
        _OPENBB(bb5,B2,0,B2,0,0,-1); // should not exist due to purging
        _OPENBB(bb5,NULL,BB_SIZE,NULL,_CBB,0,0); // blobstore full, 1 blob purgeable
        _OPENBB(bb6,NULL,BB_SIZE,NULL,_CBB,0,-1); // blobstore full, nothing purgeable
        _CLOSBB(bb4,NULL);
        _OPENBB(bb4,NULL,BB_SIZE,NULL,_CBB,0,0); // blobstore full, 1 blob purgeable
        _CLOSBB(bb4,NULL);
        _CLOSBB(bb5,NULL);
        _OPENBB(bb6,B2,BB_SIZE*2,NULL,_CBB,0,0); // blobstore full, 2 blobs purgeable
        _CLOSBB(bb6,NULL);
        printf ("=== done with revocation sub-test\n");
    } else {
        printf ("=== starting no-revocation sub-test\n");
        _OPENBB(bb4,NULL,BB_SIZE,NULL,_CBB,0,-1); // blobstore full, cannot purge
        _OPENBB(bb2,B3,0,NULL,0,0,0); // open existing with any size (0)
        _DELEBB(bb2,B3,0);
        _OPENBB(bb1,B2,BB_SIZE,NULL,0,0,0); // open existing with the right size
        _DELEBB(bb1,B2,0);
        _OPENBB(bb6,B2,BB_SIZE*2,NULL,_CBB,0,0); // blobstore has room for 20
        _CLOSBB(bb6,B2);
        printf ("=== done with no-revocation sub-test\n");
    }
    _CLOSBB(bb3,B1);
    _OPENBB(bb3,B1,BB_SIZE,B1,0,0,0); // open existing with the right size
    _CLOSBB(bb3,B1);
    _OPENBB(bb3,B1,0,B1,0,0,0); // open existing with any size (0)
    _DELEBB(bb3,B1,0); // delete it
    _OPENBB(bb3,B1,0,B1,0,0,-1); // open non-existining one

    blobstore_lock (bs, 3000);
    // TODO: test locking?
    blobstore_unlock (bs);
    blobstore_close (bs);

    printf ("completed blobstore test\n");
 done:
    return errors;
}

static void * thread_function( void * ptr )
{
    printf ("this is a thread\n");
    int pid, ret, errors = 0;
    int fd1, fd2, fd3;

    _OPEN(fd2,F2,_W,0,-1);
    _OPEN(fd1,F1,_R,0,0);
    _CLOS(fd1,F1);
    _OPEN(fd3,F3,_W,0,0);
    * (int *) ptr = fd3;
    return NULL;
}

int do_file_lock_test (void)
{
    int pid, ret, errors = 0;
    int fd1, fd2, fd3;

    for (int i=0; i<5; i++) {
        printf ("\nintra-process locks cycle=%d\n", i);
        _OPEN(fd1,F1,_W,300,-1);
        _OPEN(fd1,F1,_R,300,-1); 
        _OPEN(fd2,F1,_C,0,0);
        _OPEN(fd1,F1,_C,0,-1);
        _OPEN(fd1,F1,_W,300,-1);
        _OPEN(fd1,F1,_R,300,-1); 
        _CLOS(fd2,F1);
        _OPEN(fd2,F1,_R,0,0);
        _OPEN(fd1,F1,_W,300,-1);
        _OPEN(fd1,F1,_R,300,0);
        _OPEN(fd3,F1,_R,300,0);
        _CLOS(fd3,F1);
        _CLOS(fd2,F1);
        _CLOS(fd1,F1);
        _OPEN(fd1,F1,_W,300,0);
        _OPEN(fd2,F2,_C,0,0);
        _OPEN(fd3,F3,_C,0,0);
        _CLOS(fd2,F2);
        _CLOS(fd3,F3);
        _CLOS(fd1,F1);
        remove (F1);
        remove (F2);

        printf ("opening maximum number of descriptors\n");
        int fd [BLOBSTORE_MAX_CONCURRENT];
        for (int j=0; j<BLOBSTORE_MAX_CONCURRENT; j++) {
            fd [j] = open_and_lock (F3, _R, 0, 0);
            if (fd [j] == -1) {
                _UNEXPECTED;
                printf ("opened %d descriptors (max is %d)\n", j+1, BLOBSTORE_MAX_CONCURRENT);
            }
        }
        _OPEN(fd3,F3,_R,0,-1);
        for (int j=0; j<BLOBSTORE_MAX_CONCURRENT; j++) {
            if (close_and_unlock (fd [(j+9)%BLOBSTORE_MAX_CONCURRENT]) == -1) { // close them in different order
                _UNEXPECTED;
            }
        }
        remove (F3);
    }

    for (int i=0; i<5; i++) {
        printf ("\ninter-process locks cycle=%d\n", i);
        _OPEN(fd1,F1,_W,300,-1);
        _OPEN(fd1,F1,_R,300,-1); 
        _OPEN(fd1,F1,_C,0,0);
        pid = fork();
        if (pid) {
            _PARENT_WAITS;
        } else {
            errors = 0;
            close_and_unlock (fd1);
            _OPEN(fd1,F1,_C,0,-1);
            _OPEN(fd1,F1,_W,300,-1);
            _OPEN(fd1,F1,_R,3000,-1); 
            _OPEN(fd1,F2,_C,0,0); // test unlocking upon exit
            _OPEN(fd2,F3,_C,0,0);
            _CLOS(fd2,F3);
            _OPEN(fd2,F3,_W,0,0); // test unlocking upon exit
            _exit (errors);
        }
        _CLOS(fd1,F1);
        _OPEN(fd2,F2,_R,0,0);
        _OPEN(fd3,F3,_W,0,0);
        pid = fork();
        if (pid) {
            _PARENT_WAITS;
        } else {
            errors = 0;
            close_and_unlock (fd2);
            close_and_unlock (fd3);
            _OPEN(fd2,F2,_W,300,-1);
            _OPEN(fd2,F2,_R,0,0);
            _OPEN(fd3,F2,_W,300,-1);
            _OPEN(fd3,F3,_W,3000,-1);
            _exit (errors);
        }
        _CLOS(fd3,F3);
        _CLOS(fd2,F2);
        _OPEN(fd3,F3,_W,0,0);
        _CLOS(fd3,F3);

        pid = fork();
        if (pid) {
            _PARENT_WAITS;
        } else {
            _OPEN(fd2,F2,_W,0,0);
            pid = * (int *)0; // crash!
        }
        _OPEN(fd2,F2,_W,0,0);
        _OPEN(fd1,F1,_R,0,0);
        pthread_t thread;
        int fd_thread;
        pthread_create (&thread, NULL, thread_function, (void *)&fd_thread);
        pthread_join (thread, NULL);
        printf ("waited for thread (returned fd=%d)\n", fd_thread);
        _OPEN(fd3,F3,_R,3000,-1);
        _OPEN(fd3,F3,_W,3000,-1);
        _CLOS(fd_thread,F3);
        _OPEN(fd3,F3,_R,3000,0);
        _CLOS(fd3,F3);
        _CLOS(fd2,F2);
        _CLOS(fd1,F1);
        remove (F1);
        remove (F2);
        remove (F3);
    }
    return errors;
}

int main (int argc, char ** argv)
{
    int errors = 0;

    printf ("testing blobstore.c\n");

    errors += do_file_lock_test ();
    if (errors) goto done; // no point in doing blobstore test if above isn't working

    char cwd [1024];
    getcwd (cwd, sizeof (cwd));

    errors += do_metadata_test (cwd, "directory-meta");
    if (errors) goto done; // no point in doing blobstore test if above isn't working

    errors += do_blobstore_test (cwd, "directory-norevoc", BLOBSTORE_FORMAT_DIRECTORY,   BLOBSTORE_REVOCATION_NONE);
    errors += do_blobstore_test (cwd, "lru-directory", BLOBSTORE_FORMAT_DIRECTORY,   BLOBSTORE_REVOCATION_LRU);
    errors += do_blobstore_test (cwd, "lru-visible", BLOBSTORE_FORMAT_FILES, BLOBSTORE_REVOCATION_LRU);

    errors += do_clone_test (cwd, "clone", BLOBSTORE_FORMAT_DIRECTORY, BLOBSTORE_REVOCATION_LRU, BLOBSTORE_SNAPSHOT_DM);

    errors += do_clone_stresstest (cwd, "clonestress", BLOBSTORE_FORMAT_DIRECTORY, BLOBSTORE_REVOCATION_LRU, BLOBSTORE_SNAPSHOT_DM);
 done:
    printf ("done testing blobstore.c (errors=%d)\n", errors);
    blobstore_cleanup();
    _exit(errors);
}
#endif

