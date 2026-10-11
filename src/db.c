/*
 * Record storage on top of raw system calls (open/pread/pwrite/fcntl/fstat).
 *
 * Concurrency model: each client is served by its own process (fork), so
 * POSIX fcntl() record locks give real mutual exclusion between clients.
 *   - F_RDLCK on a record  : many readers at once
 *   - F_WRLCK on a record  : exclusive read-modify-write of one record
 *   - F_WRLCK on the file  : appending a new record (allocates the next id)
 * Note: closing ANY descriptor of a file drops all of this process's locks on
 * that file, so callers never open the same file twice while holding a lock.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bank.h"

int db_open(const char *path) { return open(path, O_RDWR | O_CREAT, 0600); }

static int set_lock(int fd, short type, off_t start, off_t len, int cmd)
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    while (fcntl(fd, cmd, &fl) == -1) {
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static off_t rec_offset(int id, size_t size) { return (off_t)(id - 1) * (off_t)size; }

int lock_rec(int fd, int id, size_t size, short type) { return set_lock(fd, type, rec_offset(id, size), (off_t)size, F_SETLKW); }

int unlock_rec(int fd, int id, size_t size) { return set_lock(fd, F_UNLCK, rec_offset(id, size), (off_t)size, F_SETLK); }

int lock_all(int fd, short type) { return set_lock(fd, type, 0, 0, F_SETLKW); }

int unlock_all(int fd) { return set_lock(fd, F_UNLCK, 0, 0, F_SETLK); }

int read_rec(int fd, int id, void *rec, size_t size)
{
    if (id <= 0) return -1;
    ssize_t n = pread(fd, rec, size, rec_offset(id, size));
    if (n != (ssize_t)size) return -1;
    return *(int *)rec == id ? 0 : -1;
}

int write_rec(int fd, int id, const void *rec, size_t size)
{
    if (pwrite(fd, rec, size, rec_offset(id, size)) != (ssize_t)size) return -1;
    return fdatasync(fd);   /* durability: the record is on disk before we report success */
}

int rec_count(int fd, size_t size)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return 0;
    return (int)(st.st_size / (off_t)size);
}

/* Appends `rec`, assigning it the next id (stored in its first int). Returns the id or -1. */
int append_rec(const char *path, void *rec, size_t size)
{
    int fd = db_open(path);
    if (fd < 0) return -1;
    int id = -1;
    if (lock_all(fd, F_WRLCK) == 0) {
        int next = rec_count(fd, size) + 1;
        memcpy(rec, &next, sizeof next);
        if (write_rec(fd, next, rec, size) == 0) id = next;
        unlock_all(fd);
    }
    close(fd);
    return id;
}

/* Reads one record under a shared lock. Returns 0 if it exists. */
int load_rec(const char *path, int id, void *rec, size_t size)
{
    int fd = db_open(path);
    if (fd < 0) return -1;
    int rc = -1;
    if (id > 0 && lock_rec(fd, id, size, F_RDLCK) == 0) {
        rc = read_rec(fd, id, rec, size);
        unlock_rec(fd, id, size);
    }
    close(fd);
    return rc;
}

/*
 * One session per user: a logged-in user holds a non-blocking write lock on
 * byte `id` of the role's session file. A second login attempt fails to get
 * the lock. The kernel releases it automatically if the handler process dies.
 */
int session_acquire(int role, int id)
{
    const char *path = role == ROLE_CUSTOMER ? SESSION_CUST : role == ROLE_ADMIN ? SESSION_ADMIN : SESSION_EMP;
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    if (set_lock(fd, F_WRLCK, id, 1, F_SETLK) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void session_release(int fd)
{
    if (fd < 0) return;
    unlock_all(fd);
    close(fd);
}

/* FNV-1a with a fixed salt: keeps plain-text passwords out of the data files. */
unsigned long long hash_password(const char *pw)
{
    static const char salt[] = "BMS#CS513:";
    unsigned long long h = 1469598103934665603ULL;
    for (const char *p = salt; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    for (const char *p = pw; *p; p++)   { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    return h;
}

void log_transaction(int customer_id, int type, double amount, double balance_after, int other)
{
    Transaction t = {0};
    t.customer_id = customer_id;
    t.type = type;
    t.amount = amount;
    t.balance_after = balance_after;
    t.other_party = other;
    t.timestamp = time(NULL);
    append_rec(TXN_FILE, &t, sizeof t);
}
