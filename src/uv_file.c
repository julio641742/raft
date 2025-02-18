#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <uv.h>

#include "aio.h"
#include "assert.h"
#include "os.h"
#include "uv_file.h"

/* Support the version of libuv in Ubuntu 18.04 */
#if !defined(uv_translate_sys_error)
int uv_translate_sys_error(int sys_errno)
{
    return sys_errno <= 0 ? sys_errno : -sys_errno;
}
#endif

/* State codes */
enum { CREATING = 1, READY, ERRORED, CLOSED };

/* Run blocking syscalls involved in file creation (e.g. posix_fallocate()). */
static void createWorkCb(uv_work_t *work)
{
    struct uvFileCreate *req; /* Create file request object */
    struct uvFile *f;         /* File handle */
    osDir dir;
    int rv;

    req = work->data;
    f = req->file;

    assert(f->state == CREATING);

    /* Allocate the desired size. */
    rv = posix_fallocate(f->fd, 0, req->size);
    if (rv != 0) {
        /* From the manual page:
         *
         *   posix_fallocate() returns zero on success, or an error number on
         *   failure.  Note that errno is not set.
         */
        goto err;
    }

    /* Sync the file and its directory */
    rv = fsync(f->fd);
    if (rv == -1) {
        /* UNTESTED: should fail only in case of disk errors */
        rv = errno;
        goto err;
    }
    osDirname(req->path, dir);
    rv = osSyncDir(dir);
    if (rv != 0) {
        /* UNTESTED: should fail only in case of disk errors */
        goto err;
    }

    /* Set direct I/O if available. */
    if (f->direct) {
        rv = osSetDirectIO(f->fd);
        if (rv != 0) {
            goto err;
        }
    }

    req->status = 0;
    return;

err:
    req->status = uv_translate_sys_error(rv);
}

/* Run blocking syscalls involved in a file write request.
 *
 * Perform a KAIO write request and synchronously wait for it to complete. */
static void writeWorkCb(uv_work_t *work)
{
    struct uvFileWrite *req; /* Write file request object */
    struct uvFile *f;        /* File object */
    aio_context_t ctx;       /* KAIO handle */
    struct iocb *iocbs;      /* Pointer to KAIO request object */
    struct io_event event;   /* KAIO response object */
    int rv;

    req = work->data;
    f = req->file;
    assert(f->state == READY);

    iocbs = &req->iocb;

    /* If more than one write in parallel is allowed, submit the AIO request
     * using a dedicated context, to avoid synchronization issues between
     * threads when multiple writes are submitted in parallel. This is
     * suboptimal but in real-world users should use file systems and kernels
     * with proper async write support. */
    if (f->n_events > 1) {
        ctx = 0;
        rv = io_setup(1 /* Maximum concurrent requests */, &ctx);
        if (rv == -1) {
            /* UNTESTED: should fail only with ENOMEM */
            goto out;
        }
    } else {
        ctx = f->ctx;
    }

    /* Submit the request */
    rv = io_submit(ctx, 1, &iocbs);
    if (rv == -1) {
        /* UNTESTED: since we're not using NOWAIT and the parameters are valid,
         * this shouldn't fail. */
        goto out_after_io_setup;
    }

    /* Wait for the request to complete */
    do {
        rv = io_getevents(ctx, 1, 1, &event, NULL);
    } while (rv == -1 && errno == EINTR);
    assert(rv == 1);

    rv = 0;

out_after_io_setup:
    if (f->n_events > 1) {
        io_destroy(ctx);
    }

out:
    if (rv != 0) {
        req->status = uv_translate_sys_error(errno);
    } else {
        req->status = event.res;
    }

    return;
}

/* Remove the request from the queue of inflight writes and invoke the request
 * callback if set. */
static void writeFinish(struct uvFileWrite *req)
{
    QUEUE_REMOVE(&req->queue);
    req->cb(req, req->status);
}

/* Invoked at the end of the closing sequence. It invokes the close callback. */
static void pollCloseCb(struct uv_handle_s *handle)
{
    struct uvFile *f = handle->data;
    int rv;

    assert(f->closing);
    assert(f->state != CLOSED);
    assert(QUEUE_IS_EMPTY(&f->write_queue));

    rv = close(f->event_fd);
    assert(rv == 0);
    if (f->ctx != 0) {
        rv = io_destroy(f->ctx);
        assert(rv == 0);
    }
    free(f->events);

    f->state = CLOSED;

    if (f->close_cb != NULL) {
        f->close_cb(f);
    }
}

/* Close the poller if the closing flag is on and there's no infiglht create or
   write request. */
static void maybeClosed(struct uvFile *f)
{
    assert(f->state != CLOSED);

    if (!f->closing) {
        return;
    }

    /* If are creating the file we need to wait for the create to finish. */
    if (f->state == CREATING) {
        return;
    }

    /* If are writing we need to wait for the writes to finish. */
    if (!QUEUE_IS_EMPTY(&f->write_queue)) {
        return;
    }

    if (!uv_is_closing((struct uv_handle_s *)&f->event_poller)) {
        uv_close((struct uv_handle_s *)&f->event_poller, pollCloseCb);
    }
}

/* Callback run after writeWorkCb has returned. It normally invokes the write
 * request callback. */
static void writeAfterWorkCb(uv_work_t *work, int status)
{
    struct uvFileWrite *req; /* Write file request object */
    struct uvFile *f;

    assert(status == 0); /* We don't cancel worker requests */

    req = work->data;
    f = req->file;

    assert(f->state == READY);

    /* If we were closed, let's mark the request as canceled, regardless of the
     * actual outcome. */
    if (req->file->closing) {
        req->status = UV_ECANCELED;
    }

    writeFinish(req);
    maybeClosed(f);
}

/* Callback fired when the event fd associated with AIO write requests should be
 * ready for reading (i.e. when a write has completed). */
static void writePollCb(uv_poll_t *poller, int status, int events)
{
    struct uvFile *f = poller->data; /* File handle */
    uint64_t completed;              /* True if the write is complete */
    int rv;
    unsigned i;

    assert(f != NULL);
    assert(f->event_fd >= 0);
    assert(f->state == READY);

    /* TODO: it's not clear when polling could fail. In this case we should
     * probably mark all pending requests as failed. */
    assert(status == 0);

    assert(events & UV_READABLE);

    /* Read the event file descriptor */
    rv = read(f->event_fd, &completed, sizeof completed);
    if (rv != sizeof completed) {
        /* UNTESTED: According to eventfd(2) this is the only possible failure
         * mode, meaning that epoll has indicated that the event FD is not yet
         * ready. */
        assert(errno == EAGAIN);
        return;
    }

    /* TODO: this assertion fails in unit tests */
    /* assert(completed == 1); */

    /* Try to fetch the write responses.
     *
     * If we got here at least one write should have completed and io_events
     * should return immediately without blocking. */
    do {
        rv = io_getevents(f->ctx, 1, f->n_events, f->events, NULL);
    } while (rv == -1 && errno == EINTR);

    assert(rv >= 1);

    for (i = 0; i < (unsigned)rv; i++) {
        struct io_event *event = &f->events[i];
        struct uvFileWrite *req = *((void **)&event->data);

        /* If we are closing, we mark the write as canceled, although
         * technically it might have worked. */
        if (f->closing) {
            req->status = UV_ECANCELED;
            goto finish;
        }

#if defined(RWF_NOWAIT)
        /* If we got EAGAIN, it means it was not possible to perform the write
         * asynchronously, so let's fall back to the threadpool. */
        if (event->res == -EAGAIN) {
            req->iocb.aio_flags &= ~IOCB_FLAG_RESFD;
            req->iocb.aio_resfd = 0;
            req->iocb.aio_rw_flags &= ~RWF_NOWAIT;
            req->work.data = req;
            rv = uv_queue_work(f->loop, &req->work, writeWorkCb,
                               writeAfterWorkCb);
            if (rv != 0) {
                /* UNTESTED: with the current libuv implementation this should
                 * never fail. */
                req->status = rv;
                goto finish;
            }
            return;
        }
#endif /* RWF_NOWAIT */

        req->status = event->res;

    finish:
        writeFinish(req);
    }

    /* If we've been closed, let's see if we can stop the poller and fire the
     * close callback. */
    maybeClosed(f);
}

/** Main loop callback run after @createWorkCb has returned. It normally starts
 * the eventfd poller to receive notifications about completed writes and invoke
 * the create request callback. */
static void createAfterWorkCb(uv_work_t *work, int status)
{
    struct uvFileCreate *req;
    struct uvFile *f;
    int rv;

    assert(status == 0); /* We don't cancel worker requests */
    req = work->data;
    assert(req != NULL);
    f = req->file;

    /* If we were closed, abort here. */
    if (f->closing) {
        unlink(req->path);
        req->status = UV_ECANCELED;
        goto out;
    }

    /* If no error occurred, start polling the event file descriptor. */
    if (req->status == 0) {
        rv = uv_poll_start(&f->event_poller, UV_READABLE, writePollCb);
        if (rv != 0) {
            /* UNTESTED: the underlying libuv calls should never fail. */
            req->status = rv;

            io_destroy(f->ctx);
            close(f->event_fd);
            close(f->fd);
            unlink(req->path);
        }
    }

out:
    if (req->status == 0) {
        f->state = READY;
    } else {
        f->state = ERRORED;
    }

    if (req->cb != NULL) {
        req->cb(req, req->status);
    }

    maybeClosed(f);
}

int uvFileInit(struct uvFile *f,
               struct uv_loop_s *loop,
               bool direct,
               bool async)
{
    int rv;

    f->loop = loop;
    f->fd = -1;
    f->direct = direct;
    f->async = async;
    f->event_fd = -1;

    /* Create an event file descriptor to get notified when a write has
     * completed. */
    f->event_fd = eventfd(0, EFD_NONBLOCK);
    if (f->event_fd < 0) {
        /* UNTESTED: should fail only with ENOMEM */
        rv = uv_translate_sys_error(errno);
        goto err;
    }

    rv = uv_poll_init(f->loop, &f->event_poller, f->event_fd);
    if (rv != 0) {
        /* UNTESTED: with the current libuv implementation this should never
         * fail. */
        goto err_after_event_fd;
    }
    f->event_poller.data = f;

    f->ctx = 0;
    f->events = NULL;
    f->n_events = 0;
    QUEUE_INIT(&f->write_queue);
    f->closing = false;
    f->close_cb = NULL;

    return 0;

err_after_event_fd:
    close(f->event_fd);
err:
    assert(rv != 0);
    return rv;
}

int uvFileCreate(struct uvFile *f,
                 struct uvFileCreate *req,
                 osPath path,
                 size_t size,
                 unsigned max_n_writes,
                 uvFileCreateCb cb)
{
    int flags = O_WRONLY | O_CREAT | O_EXCL; /* Common open flags */
    int rv;

    assert(path != NULL);
    assert(size > 0);
    assert(!f->closing);
    assert(strnlen(path, OS_MAX_PATH_LEN + 1) <= OS_MAX_PATH_LEN);

    f->state = CREATING;

#if !defined(RWF_DSYNC)
    /* If per-request synchronous I/O is not supported, open the file with the
     * sync flag. */
    flags |= O_DSYNC;
#endif

    f->events = NULL; /* We'll allocate this in the create callback */
    f->n_events = max_n_writes;

    /* Try to create a brand new file. */
    f->fd = open(path, flags, S_IRUSR | S_IWUSR);
    if (f->fd == -1) {
        rv = uv_translate_sys_error(errno);
        goto err;
    }

    /* Setup the AIO context. */
    rv = io_setup(f->n_events /* Maximum concurrent requests */, &f->ctx);
    if (rv == -1) {
        /* UNTESTED: should fail only with ENOMEM */
        rv = uv_translate_sys_error(errno);
        goto err_after_open;
    }

    /* Initialize the array of re-usable event objects. */
    f->events = calloc(f->n_events, sizeof *f->events);
    if (f->events == NULL) {
        /* UNTESTED: define a configurable allocator that can fail? */
        rv = UV_ENOMEM;
        goto err_after_io_setup;
    }

    req->file = f;
    req->cb = cb;
    strcpy(req->path, path);
    req->size = size;
    req->status = 0;
    req->work.data = req;

    rv = uv_queue_work(f->loop, &req->work, createWorkCb, createAfterWorkCb);
    if (rv != 0) {
        /* UNTESTED: with the current libuv implementation this can't fail. */
        goto err_after_open;
    }

    return 0;

err_after_io_setup:
    io_destroy(f->ctx);
    f->ctx = 0;
err_after_open:
    close(f->fd);
    unlink(path);
    f->fd = -1;
err:
    assert(rv != 0);
    f->state = 0;
    return rv;
}

int uvFileWrite(struct uvFile *f,
                struct uvFileWrite *req,
                const uv_buf_t bufs[],
                unsigned n,
                size_t offset,
                uvFileWriteCb cb)
{
    int rv;
#if defined(RWF_NOWAIT)
    struct iocb *iocbs = &req->iocb;
#endif /* RWF_NOWAIT */

    assert(!f->closing);
    assert(f->state == READY);

    /* TODO: at the moment we are not leveraging the support for concurrent
     *       writes, so ensure that we're getting write requests
     *       sequentially. */
    if (f->n_events == 1) {
        assert(QUEUE_IS_EMPTY(&f->write_queue));
    }

    assert(f->fd >= 0);
    assert(f->event_fd >= 0);
    assert(f->ctx != 0);
    assert(req != NULL);
    assert(bufs != NULL);
    assert(n > 0);

    req->file = f;
    req->cb = cb;
    memset(&req->iocb, 0, sizeof req->iocb);
    req->iocb.aio_fildes = f->fd;
    req->iocb.aio_lio_opcode = IOCB_CMD_PWRITEV;
    req->iocb.aio_reqprio = 0;
    *((void **)(&req->iocb.aio_buf)) = (void *)bufs;
    req->iocb.aio_nbytes = n;
    req->iocb.aio_offset = offset;
    *((void **)(&req->iocb.aio_data)) = (void *)req;

    QUEUE_PUSH(&f->write_queue, &req->queue);

#if defined(RWF_HIPRI)
    /* High priority request, if possible */
    req->iocb.aio_rw_flags |= RWF_HIPRI;
#endif

#if defined(RWF_DSYNC)
    /* Use per-request synchronous I/O if available. Otherwise, we have opened
     * the file with O_DSYNC. */
    req->iocb.aio_rw_flags |= RWF_DSYNC;
#endif

#if defined(RWF_NOWAIT)
    /* If io_submit can be run in a 100% non-blocking way, we'll try to write
     * without using the threadpool. */
    if (f->async) {
        req->iocb.aio_flags |= IOCB_FLAG_RESFD;
        req->iocb.aio_resfd = f->event_fd;
        req->iocb.aio_rw_flags |= RWF_NOWAIT;
    }
#else
    /* Since there's no support for NOWAIT, io_submit might occasionally block
     * and we need to run it in the threadpool. */
    assert(f->async == false);
#endif /* RWF_NOWAIT */

#if defined(RWF_NOWAIT)
    /* Try to submit the write request asynchronously */
    if (f->async) {
        rv = io_submit(f->ctx, 1, &iocbs);

        /* If no error occurred, we're done, the write request was
         * submitted. */
        if (rv != -1) {
            assert(rv == 1); /* TODO: can 0 be returned? */
            goto done;
        }

        /* Check the reason of the error. */
        switch (errno) {
            case EOPNOTSUPP:
                /* NOWAIT is not supported, this should occur because we checked
                 * it in osProbeIO. */
                assert(0);
                break;
            case EAGAIN:
                break;
            default:
                /* Unexpected error */
                rv = uv_translate_sys_error(errno);
                goto err;
        }

        /* Submitting the write would block, or NOWAIT is not
         * supported. Let's run this request in the threadpool. */
        req->iocb.aio_flags &= ~IOCB_FLAG_RESFD;
        req->iocb.aio_resfd = 0;
        req->iocb.aio_rw_flags &= ~RWF_NOWAIT;
    }
#endif /* RWF_NOWAIT */

    /* If we got here it means we need to run io_submit in the threadpool. */
    req->work.data = req;

    rv = uv_queue_work(f->loop, &req->work, writeWorkCb, writeAfterWorkCb);
    if (rv != 0) {
        /* UNTESTED: with the current libuv implementation this can't fail. */
        goto err;
    }

#if defined(RWF_NOWAIT)
done:
#endif /* RWF_NOWAIT */
    return 0;

err:
    assert(rv != 0);
    QUEUE_REMOVE(&req->queue);
    return rv;
}

void uvFileClose(struct uvFile *f, uvFileCloseCb cb)
{
    int rv;
    assert(!f->closing);

    f->closing = true;
    f->close_cb = cb;

    if (f->fd != -1) {
        rv = close(f->fd);
        assert(rv == 0);
    }

    maybeClosed(f);
}
