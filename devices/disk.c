/*
 * WindDroid - disk backends: raw image through a file descriptor (works with
 * Android's ParcelFileDescriptor) and an in-memory disk for tests.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/internal.h"

typedef struct { int fd; int own; } fd_disk;

static int fd_read(void *o, uint64_t off, void *buf, size_t len)
{
    fd_disk *d = o;
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(d->fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) { memset(p, 0, len); return 0; }     /* past the end: zeros */
        p += n; off += (uint64_t)n; len -= (size_t)n;
    }
    return 0;
}

static int fd_write(void *o, uint64_t off, const void *buf, size_t len)
{
    fd_disk *d = o;
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = pwrite(d->fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; off += (uint64_t)n; len -= (size_t)n;
    }
    return 0;
}

static int fd_flush(void *o)
{
    fd_disk *d = o;
    return fsync(d->fd) == 0 || errno == EINVAL || errno == EROFS ? 0 : -1;
}

static void fd_close(void *o)
{
    fd_disk *d = o;
    if (d->own) close(d->fd);
    free(d);
}

int wd_disk_from_fd(int fd, int readonly, int take_ownership, wd_disk *out)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0) size = st.st_size;
    if (size <= 0) return -1;
    fd_disk *d = calloc(1, sizeof *d);
    if (!d) return -1;
    d->fd = fd;
    d->own = take_ownership;
    memset(out, 0, sizeof *out);
    out->opaque = d;
    out->size = (uint64_t)size;
    out->readonly = readonly;
    out->read = fd_read;
    out->write = readonly ? NULL : fd_write;
    out->flush = fd_flush;
    out->close = fd_close;
    return 0;
}

typedef struct { uint8_t *mem; } mem_disk;

static int mem_read(void *o, uint64_t off, void *buf, size_t len) { memcpy(buf, ((mem_disk *)o)->mem + off, len); return 0; }
static int mem_write(void *o, uint64_t off, const void *buf, size_t len) { memcpy(((mem_disk *)o)->mem + off, buf, len); return 0; }
static int mem_flush(void *o) { (void)o; return 0; }
static void mem_close(void *o) { free(((mem_disk *)o)->mem); free(o); }

int wd_disk_memory(uint64_t size, wd_disk *out)
{
    mem_disk *d = calloc(1, sizeof *d);
    if (!d) return -1;
    d->mem = calloc(1, (size_t)size);
    if (!d->mem) { free(d); return -1; }
    memset(out, 0, sizeof *out);
    out->opaque = d;
    out->size = size;
    out->read = mem_read;
    out->write = mem_write;
    out->flush = mem_flush;
    out->close = mem_close;
    return 0;
}
