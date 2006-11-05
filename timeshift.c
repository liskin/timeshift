/*
 * timeshift
 *
 * Author: Tomas Janousek <tomi@nomi.cz>
 * License: GPL
 *
 * Superpipe that reads everything and remembers it so you can read it anytime
 * you want. Useful for time-shifting.
 *
 * A cache directory is needed.
 *
 * Example usage:
 * mplayer -dumpstream -dumpfile /dev/fd/3 dvb://channel 3>&1 |
 *      ./timeshift -d cache | mplayer -
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

char *cachedir = 0;
int chunksize = 4 * 1024 * 1024;

/**
 * Structure for storage.
 */
struct storage_t {
    struct storage_t *next;
    
    char name[32];
    int fdr, fdw;
    int offr, offw;
};

struct storage_t *storage = 0, *last_storage = 0;

/**
 * Alloc a new storage and push it to the list.
 */
void alloc_storage(void)
{
    struct storage_t *s = malloc(sizeof(struct storage_t));
    if (!s)
        perror("malloc"), abort();

    s->next = 0;
    strcpy(s->name, "timeshiftXXXXXX");
    s->fdw = mkstemp(s->name);
    if (s->fdw == -1)
        perror("mkstemp"), abort();
    s->fdr = open(s->name, O_RDONLY);
    if (s->fdr == -1)
        perror("open"), abort();
    s->offr = s->offw = 0;

    struct storage_t **sp = &storage;
    while (*sp)
        sp = &(*sp)->next;
    *sp = s;
    last_storage = s;
}

/**
 * Drop the first storage from the list.
 */
void drop_storage(void)
{
    if (!storage)
        fprintf(stderr, "No storage to drop!\n"), abort();

    struct storage_t *s = storage;

    close(s->fdw);
    close(s->fdr);
    unlink(s->name);

    storage = storage->next;
    free(s);
    if (s == last_storage)
        last_storage = 0;
}

/**
 * Clean up the storage.
 */
void drop_all_storage(void)
{
    while (storage)
        drop_storage();
}

/**
 * Drop all filled and read storage.
 */
void drop_used_storage(void)
{
    while (storage && storage->offr == storage->offw &&
            storage->offw == chunksize) {
        drop_storage();
    }
}

/**
 * Write the data to one storage, alloc it if needed.
 * \return The amount of data that actually fit into this storage.
 */
int do_storage_write(const char *buf, int bufsz)
{
    if (!last_storage || last_storage->offw == chunksize)
        alloc_storage();

    struct storage_t *s = last_storage;
    const char *p = buf;
    int sz;

    while (s->offw < chunksize && (p - buf) < bufsz) {
        int towr = MIN(chunksize - s->offw, bufsz - (p - buf));
        sz = write(s->fdw, p, towr);
        if (sz == -1)
            perror("write"), abort();
        p += sz;
        s->offw += sz;
    }

    return p - buf;
}

/**
 * Write all the data to the storage.
 */
void write_storage(const char *buf, int bufsz)
{
    const char *p = buf;

    while ((p - buf) < bufsz) {
        int sz = do_storage_write(p, bufsz - (p - buf));
        p += sz;
    }
}

/**
 * Return if there is data available.
 */
int data_available()
{
    drop_used_storage();

    if (storage)
        return storage->offw - storage->offr;
    else
        return 0;
}

/**
 * Read data from storage. Does not advance the read offset.
 * \return The amount of data read.
 */
int read_storage(char *buf, int bufsz)
{
    if (!storage)
        fprintf(stderr, "No storage to read from!\n"), abort();

    struct storage_t *s = storage;

    int tord = MIN(s->offw - s->offr, bufsz);
    int sz = read(s->fdr, buf, tord);
    if (sz == -1)
        perror("read"), abort();
    if (sz == 0)
        fprintf(stderr, "End of file in read_storage, shouldn't happen.\n"), abort();

    if (lseek(s->fdr, -sz, SEEK_CUR) == -1)
        perror("lseek"), abort();

    return sz;
}

/**
 * Advance read offset.
 */
void advance_storage(int sz)
{
    if (!storage)
        fprintf(stderr, "No storage to advance!\n"), abort();

    storage->offr += sz;

    if (lseek(storage->fdr, sz, SEEK_CUR) == -1)
        perror("lseek"), abort();
}

/**
 * Signal handler. Free storage and quit.
 */
void sig(int num) {
    drop_all_storage();
    exit(0);
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    while (1) {
        char c;

        if ((c = getopt(argc, argv, "hd:s:")) == -1)
            break;

        switch (c) {
            case 'd':
                cachedir = optarg;
                break;

            case 's':
                chunksize = atoi(optarg);
                if (!chunksize) {
                    fprintf(stderr, "Bad chunksize\n");
                    return -1;
                }
                break;

            case 'h':
                fprintf(stderr, "Usage: %s [options]\n", argv[0]);
                fprintf(stderr, " -h - this message\n");
                fprintf(stderr, " -d dir - cache dir\n");
                fprintf(stderr, " -s sz - chunk size\n");
                return 0;
                
            case ':':
            case '?':
                fprintf(stderr, "Use %s -h for help\n", argv[0]);
                return -1;
        }
    }

    if (!cachedir) {
        fprintf(stderr, "Cache dir not specified\n");
        return -1;
    }

    if (chdir(cachedir) == -1)
        perror("chdir"), abort();

    int in = 1, data = 0;

    while ((data = data_available()) || in) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        if (in) FD_SET(0, &rd);
        FD_ZERO(&wr);
        if (data) FD_SET(1, &wr);

        int ret = select(2, &rd, &wr, 0, 0);
        if (ret == -1)
            perror("select"), abort();

        if (FD_ISSET(0, &rd)) {
            char buffer[4096];
            int sz = read(0, &buffer, 4096);
            if (sz == -1)
                perror("read"), abort();
            else if (sz == 0)
                in = 0;
            else
                write_storage(buffer, sz);
        }

        if (FD_ISSET(1, &wr)) {
            char buffer[4096];
            int sz = read_storage(buffer, 4096);
            int wsz = write(1, buffer, sz);
            if (wsz == -1) {
                if (errno == EPIPE)
                    goto exit;
                perror("write"), abort();
            }
            advance_storage(wsz);
        }
    }

exit:
    drop_all_storage();

    return 0;
}
