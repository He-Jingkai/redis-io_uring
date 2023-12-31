#include <asm-generic/int-ll64.h>
#include <sys/epoll.h> //used for define EPOLLIN and EPOLLOUT
#include <sys/select.h>
#include "ae.h"
#include "zmalloc.h"
#include "liburing.h"

#define BACKLOG 8192
#define MAX_ENTRIES 16384 /* entries should be configured by users */
#define ENABLE_SQPOLL 1

static int register_files = 1;

typedef struct uring_event {
    int fd;
    int type;
} uring_event;

typedef struct aeApiState {
    int urfd;
    struct io_uring *ring;
    struct uring_event *events;
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;

    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }

    state->ring = zmalloc(sizeof(struct io_uring));
    if (!state->ring) {
        zfree(state->events);
        zfree(state);
        return -1;
    }

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    if (eventLoop->extflags & ENABLE_SQPOLL)
        params.flags |= IORING_SETUP_SQPOLL;
    state->urfd = io_uring_queue_init_params(MAX_ENTRIES, state->ring, &params);
    if (state->urfd == -1) {
        zfree(state->ring);
        zfree(state->events);
        zfree(state);
        return -1;
    }
    if (!(params.features & IORING_FEAT_FAST_POLL)) {
        io_uring_queue_exit(state->ring);
        zfree(state->ring);
        zfree(state->events);
        zfree(state);
        return -1;
    }

    eventLoop->apidata = state;

    if (register_files) {
        int *files = malloc(MAX_ENTRIES * sizeof(int));
        if (!files) {
            register_files = 0;
            return 0;
        }
        for (int i = 0; i < MAX_ENTRIES; i++)
            files[i] = -1;
        int ret = io_uring_register_files(state->ring, files, MAX_ENTRIES);
        if (ret < 0) {
            fprintf(stderr, "error register_files %d\n", ret);
            register_files = 0;
        }
    }
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    (void)eventLoop;
    if (setsize >= FD_SETSIZE) return -1;
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->urfd);
    zfree(state->events);
    zfree(state->ring);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask,
                         struct iovec *iovecs) {
    aeApiState *state = eventLoop->apidata;

    struct io_uring_sqe *sqe = io_uring_get_sqe(state->ring);
    if (!sqe) return -1;

    /* NULL iovec means doing poll_add behavior */
    if (!iovecs) {
        unsigned int poll_mask = 0;
        if (mask == AE_READABLE) poll_mask |= EPOLLIN;
        if (mask == AE_WRITABLE) poll_mask |= EPOLLOUT;
        io_uring_prep_poll_add(sqe, fd, poll_mask);
    } else {
        if (mask & AE_READABLE)
            io_uring_prep_readv(sqe, fd, iovecs, 1, 0);
        if (mask & AE_WRITABLE)
            io_uring_prep_writev(sqe, fd, iovecs, 1, 0);
    }

    uring_event *ev = &state->events[fd];
    ev->fd = fd;
    ev->type = mask;
    if (!iovecs)
        ev->type |= AE_POLLABLE;
    if (register_files)
        sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, (void *)ev);

    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    (void)delmask;
    aeApiState *state = eventLoop->apidata;

    struct io_uring_sqe *sqe = io_uring_get_sqe(state->ring);
    if (!sqe) exit(1);

    uring_event *ev = &state->events[fd];
    io_uring_prep_poll_remove(sqe, (__u64)(void *)ev);
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    /* TODO: handle timeout */
    (void)tvp;

    retval = io_uring_submit_and_wait(state->ring, 1);
    if (retval < 0) {
        return numevents;
    }

    struct io_uring_cqe *cqes[BACKLOG];
    int cqe_count = io_uring_peek_batch_cqe(state->ring, cqes, sizeof(cqes) / sizeof(cqes[0]));

    /* go through all the cqe's */
    for (int i = 0; i < cqe_count; ++i) {
        int mask = 0;
        struct io_uring_cqe *cqe = cqes[i];
        uring_event *ev = io_uring_cqe_get_data(cqe);
        if (!ev) {
            io_uring_cqe_seen(state->ring, cqe);
            continue;
        }

        if (ev->type & AE_POLLABLE) {
            if (cqe->res < 0) {
                io_uring_cqe_seen(state->ring, cqe);
                continue;
            }

            if (cqe->res & EPOLLIN) mask |= AE_READABLE | AE_POLLABLE;
            if (cqe->res & EPOLLOUT) mask |= AE_WRITABLE | AE_POLLABLE;
            if (cqe->res & EPOLLERR) mask |= AE_WRITABLE | AE_READABLE | AE_POLLABLE;
            if (cqe->res & EPOLLHUP) mask |= AE_WRITABLE | AE_READABLE | AE_POLLABLE;
        } else {
            mask = ev->type;
            eventLoop->fired[numevents].res = cqe->res;
        }

        eventLoop->fired[numevents].fd = ev->fd;
        eventLoop->fired[numevents].mask = mask;

        io_uring_cqe_seen(state->ring, cqe);
        numevents++;
    }

    return numevents;
}

void aeApiRegisterFile(aeEventLoop *eventLoop, int fd){
    aeApiState *state = eventLoop->apidata;
    if (register_files) {
        int ret = io_uring_register_files_update(state->ring, fd, &fd, 1);
        if (ret < 0) {
               fprintf(stderr, "lege io_uring_register_files_update failed: %d %d\n", fd, ret);
               register_files = 0;
        }
    }
}

static char *aeApiName(void) {
    return "io_uring";
}
