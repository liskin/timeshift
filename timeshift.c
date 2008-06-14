/*
 * timeshift - Win32
 *
 * Author: Tomas Janousek <tomi@nomi.cz>
 * License: GPL
 *
 * Superpipe that reads everything and remembers it so you can read it anytime
 * you want. Useful for time-shifting.
 * (this is the HTTP branch which listens on its own socket and forwards the
 * request to another HTTP server, pipecaching its reply)
 *
 * A cache directory is needed.
 *
 * Example usage:
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

char *cachedir = 0, *recorddir = 0;
int chunksize = 16 * 1024 * 1024;
unsigned short port = 8080;
struct sockaddr_in dst;

/**
 * Structure for storage.
 */
struct storage_t {
    struct storage_t *next;

    char name[32];
    int fdr, fdw;
    int offr, offw;
};

struct thread_t {
    struct storage_t *storage, *last_storage;
#if 0
    FILE *record; ///< Recording filehandle.
#endif
};


/**
 * Alloc new thread_t.
 */
struct thread_t * new_thread_t(void) {
    struct thread_t *th = malloc(sizeof(struct thread_t));
    if (!th)
        perror("malloc"), abort();

    th->storage = 0;
    th->last_storage = 0;
#if 0
    th->record = 0;
#endif

    return th;
}

/**
 * Alloc a new storage and push it to the list.
 */
void alloc_storage(struct thread_t *th)
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

    struct storage_t **sp = &th->storage;
    while (*sp)
        sp = &(*sp)->next;
    *sp = s;
    th->last_storage = s;
}

/**
 * Drop the first storage from the list.
 */
void drop_storage(struct thread_t *th)
{
    if (!th->storage)
        fprintf(stderr, "No storage to drop!\n"), abort();

    struct storage_t *s = th->storage;

    close(s->fdw);
    close(s->fdr);
    unlink(s->name);

    th->storage = th->storage->next;
    free(s);
    if (s == th->last_storage)
        th->last_storage = 0;
}

/**
 * Clean up the storage.
 */
void drop_all_storage(struct thread_t *th)
{
    while (th->storage)
        drop_storage(th);
}

/**
 * Drop all filled and read storage.
 */
void drop_used_storage(struct thread_t *th)
{
    while (th->storage && th->storage->offr == th->storage->offw &&
            th->storage->offw == chunksize) {
        drop_storage(th);
    }
}

/**
 * Write the data to one storage, alloc it if needed.
 * \return The amount of data that actually fit into this storage.
 */
int do_storage_write(struct thread_t *th, const char *buf, int bufsz)
{
    if (!th->last_storage || th->last_storage->offw == chunksize)
        alloc_storage(th);

    struct storage_t *s = th->last_storage;
    const char *p = buf;
    int sz;

    while (s->offw < chunksize && (p - buf) < bufsz) {
        int towr = MIN(chunksize - s->offw, bufsz - (p - buf));
        sz = write(s->fdw, p, towr);
        if (sz == -1) {
	    /* Silently fail if the disk is full. */
	    if (errno == ENOSPC)
		return bufsz;
	    else
		perror("write"), abort();
	}
        p += sz;
        s->offw += sz;
    }

    return p - buf;
}

/**
 * Write all the data to the storage.
 */
void write_storage(struct thread_t *th, const char *buf, int bufsz)
{
    const char *p = buf;

    while ((p - buf) < bufsz) {
        int sz = do_storage_write(th, p, bufsz - (p - buf));
        p += sz;
    }
}

/**
 * Return if there is data available.
 */
int data_available(struct thread_t *th)
{
    drop_used_storage(th);

    if (th->storage)
        return th->storage->offw - th->storage->offr;
    else
        return 0;
}

/**
 * Read data from storage. Does not advance the read offset.
 * \return The amount of data read.
 */
int read_storage(struct thread_t *th, char *buf, int bufsz)
{
    if (!th->storage)
        fprintf(stderr, "No storage to read from!\n"), abort();

    struct storage_t *s = th->storage;

    int tord = MIN(s->offw - s->offr, bufsz);
    int sz = read(s->fdr, buf, tord);
    if (sz == -1)
        perror("read"), abort();
    if (sz == 0)
        fprintf(stderr, "End of file in read_storage, shouldn't happen.\n"),
	    abort();

    if (lseek(s->fdr, -sz, SEEK_CUR) == -1)
        perror("lseek"), abort();

    return sz;
}

/**
 * Advance read offset.
 */
void advance_storage(struct thread_t *th, int sz)
{
    if (!th->storage)
        fprintf(stderr, "No storage to advance!\n"), abort();

    th->storage->offr += sz;

    if (lseek(th->storage->fdr, sz, SEEK_CUR) == -1)
        perror("lseek"), abort();
}

#if 0
/**
 * Stop recording, if any.
 */
void stop_recording(struct thread_t *th)
{
    if (th->record) {
        fclose(th->record);
        th->record = 0;
    }
}

/**
 * Start new recording.
 */
void start_recording(struct thread_t *th)
{
    stop_recording(th);

    char name[strlen(recorddir) + 20];
    strcpy(name, recorddir);
    strcat(name, "/recordXXXXXX");

    int fd = mkstemp(name);
    if (fd == -1)
        perror("mkstemp"), abort();

    th->record = fdopen(fd, "ab");
    if (!th->record)
        perror("fdopen"), abort();
}
#endif

/**
 * Send the whole buffer, even if that requires blocking.
 */
int sendall(int s, const char* buf, const int size)
{
    int sz = size;
    const char *p = buf;
    while (sz) {
	int ret = TEMP_FAILURE_RETRY(write(s, p, sz));
	if (ret == -1)
	    return ret;

	p += ret;
	sz -= ret;
    }

    return size;
}

/**
 * Do the pipecaching on the in/out fds.
 */
void do_timeshift(int fdin, int fdout)
{
    struct thread_t *th = new_thread_t();

    int in = 1, data = 0;

    while ((data = data_available(th)) || in) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        if (in) {
	    FD_SET(fdin, &rd);
	    FD_SET(fdout, &rd);
	}
        FD_ZERO(&wr);
        if (data) FD_SET(fdout, &wr);

        int ret = TEMP_FAILURE_RETRY(select(MAX(fdin, fdout) + 1, &rd, &wr, 0, 0));
        if (ret == -1)
            perror("select"), abort();

        if (FD_ISSET(fdin, &rd)) {
            char buffer[4096];
            int sz = read(fdin, buffer, 4096);
            if (sz == -1 || sz == 0)
                in = 0;
            else
                write_storage(th, buffer, sz);
        }

        if (FD_ISSET(fdout, &rd)) {
            char buffer[4096];
            int sz = read(fdout, buffer, 4096);
            if (sz == -1 || sz == 0)
                goto exit;
            if (sendall(fdin, buffer, sz) == -1)
		goto exit;
        }

        if (FD_ISSET(fdout, &wr)) {
            char buffer[4096];
            int sz = read_storage(th, buffer, 4096);
            int wsz = write(fdout, buffer, sz);
            if (wsz == -1)
                goto exit;
            advance_storage(th, wsz);

#if 0
            /*
             * Record.
             */
            if (th->record)
                if (fwrite(buffer, wsz, 1, th->record) != 1)
                    fprintf(stderr, "Recording error"), stop_recording(th);
#endif
        }
    }

exit:
    drop_all_storage(th);
    free(th);
}

/**
 * The client thread. Connect to the destination, pass the request and then do
 * the pipecaching.
 */
void thread(int cl)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1)
        perror("socket"), abort();

    if (connect(s, &dst, sizeof(dst)) == -1)
        goto exit;

    do_timeshift(s, cl);

exit:
    close(cl);
    close(s);
}

/**
 * Initialize the listening socket.
 */
int init_server_socket(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == -1)
        perror("socket"), abort();

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(port);

    if (bind(s, &sa, sizeof(sa)) == -1)
        perror("bind"), abort();

    if (listen(s, 5) == -1)
        perror("listen"), abort();

    return s;
}

/**
 * Resolve the hostname.
 */
unsigned long resolv(const char *host)
{
    struct hostent *hp;
    unsigned long host_ip = 0;

    hp = gethostbyname(host);
    if (!hp)
	fprintf(stderr, "Could not resolve hostname %s\n", host);
    else
	host_ip = *(unsigned long *)(hp->h_addr);

    return host_ip;
}

/**
 * Clean the zombies.
 */
void sigchld(int s)
{
    int status, serrno;
    serrno = errno;
    while (1)
	if (waitpid (WAIT_ANY, &status, WNOHANG) <= 0)
	    break;
    errno = serrno;
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, sigchld);
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;

    while (1) {
        char c;

        if ((c = getopt(argc, argv, "hd:r:s:l:t:p:")) == -1)
            break;

        switch (c) {
            case 'd':
                cachedir = optarg;
                break;

            case 'r':
                recorddir = optarg;
                break;

            case 's':
                chunksize = atoi(optarg);
                if (!chunksize) {
                    fprintf(stderr, "Bad chunksize\n");
                    return -1;
                }
                break;

            case 'l':
                port = atoi(optarg);
                if (!port) {
                    fprintf(stderr, "Bad port\n");
                    return -1;
                }
                break;

            case 't':
                dst.sin_addr.s_addr = resolv(optarg);
                if (!dst.sin_addr.s_addr)
                    return -1;
                break;

            case 'p':
                dst.sin_port = htons(atoi(optarg));
                if (!dst.sin_port) {
                    fprintf(stderr, "Bad port\n");
                    return -1;
                }
                break;

            case 'h':
                fprintf(stderr, "Usage: %s [options]\n", argv[0]);
                fprintf(stderr, " -h - this message\n");
                fprintf(stderr, " -d dir - cache dir\n");
                fprintf(stderr, " -r dir - recording dir\n");
                fprintf(stderr, " -s sz - chunk size\n");
                fprintf(stderr, " -l port - listen port\n");
                fprintf(stderr, " -t host - dst host\n");
                fprintf(stderr, " -p port - dst port\n");
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

    if (!dst.sin_addr.s_addr || !dst.sin_port) {
        fprintf(stderr, "Destination not specified\n");
        return -1;
    }

    if (!recorddir)
        recorddir = cachedir;

    if (chdir(cachedir) == -1)
        perror("chdir"), abort();

    int s = init_server_socket();

    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);

        int ret = TEMP_FAILURE_RETRY(select(s + 1, &fds, 0, 0, 0));
        if (ret == -1)
            perror("select"), abort();

        if (FD_ISSET(s, &fds)) {
            int cl = accept(s, 0, 0);
            if (cl == -1)
                perror("accept"), abort();

	    pid_t pid;
	    if ((pid = fork()) == 0) {
		close(s);
                thread(cl);
		return 0;
	    } else if (pid == -1)
                perror("fork"), abort();
            else
                close(cl);
        }
    }

    return 0;
}
