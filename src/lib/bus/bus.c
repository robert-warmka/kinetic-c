/*
* kinetic-c-client
* Copyright (C) 2014 Seagate Technology.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <assert.h>
#include <limits.h>

#include "bus.h"
#include "sender.h"
#include "listener.h"
#include "threadpool.h"
#include "bus_internal_types.h"

/* Function pointers for pthreads. */
void *listener_mainloop(void *arg);
void *sender_mainloop(void *arg);

static bool poll_on_completion(struct bus *b, int fd);
static int sender_id_of_socket(struct bus *b, int fd);
static int listener_id_of_socket(struct bus *b, int fd);
static void noop_log_cb(log_event_t event,
        int log_level, const char *msg, void *udata);
static void noop_error_cb(bus_unpack_cb_res_t result, void *socket_udata);

static void set_defaults(bus_config *cfg) {
    if (cfg->sender_count == 0) { cfg->sender_count = 1; }
    if (cfg->listener_count == 0) { cfg->listener_count = 1; }
}

bool bus_init(bus_config *config, struct bus_result *res) {
    if (res == NULL) { return false; }
    if (config == NULL) {
        res->status = BUS_INIT_ERROR_NULL;
        return false;
    }
    set_defaults(config);
    if (config->sink_cb == NULL) {
        res->status = BUS_INIT_ERROR_MISSING_SINK_CB;
        return false;
    }
    if (config->unpack_cb == NULL) {
        res->status = BUS_INIT_ERROR_MISSING_UNPACK_CB;
        return false;
    }
    if (config->log_cb == NULL) {
        config->log_cb = noop_log_cb;
        config->log_level = INT_MIN;
    }
    if (config->error_cb == NULL) {
        config->error_cb = noop_error_cb;
    }

    res->status = BUS_INIT_ERROR_ALLOC_FAIL;

    bool log_lock_init = false;
    struct sender **ss = NULL;
    struct listener **ls = NULL;
    struct threadpool *tp = NULL;
    bool *j = NULL;
    pthread_t *pts = NULL;

    bus *b = calloc(1, sizeof(*b));
    if (b == NULL) { goto cleanup; }

    b->sink_cb = config->sink_cb;
    b->unpack_cb = config->unpack_cb;
    b->unexpected_msg_cb = config->unexpected_msg_cb;
    b->error_cb = config->error_cb;
    b->log_cb = config->log_cb;
    b->log_level = config->log_level;
    b->udata = config->bus_udata;
    if (0 != pthread_mutex_init(&b->log_lock, NULL)) {
        res->status = BUS_INIT_ERROR_MUTEX_INIT_FAIL;
        goto cleanup;
    }
    log_lock_init = true;

    BUS_LOG_SNPRINTF(b, 3, LOG_INITIALIZATION, b->udata, 64,
        "Initialized bus at %p", b);

    pthread_mutex_lock(&b->log_lock);
    pthread_mutex_unlock(&b->log_lock);

    ss = calloc(config->sender_count, sizeof(*ss));
    if (ss == NULL) {
        goto cleanup;
    }

    for (int i = 0; i < config->sender_count; i++) {
        ss[i] = sender_init(b, config);
        if (ss[i] == NULL) {
            res->status = BUS_INIT_ERROR_SENDER_INIT_FAIL;
            goto cleanup;
        } else {
            BUS_LOG_SNPRINTF(b, 3, LOG_INITIALIZATION, b->udata, 64,
                "Initialized sender %d at %p", i, ss[i]);
        }
    }

    ls = calloc(config->listener_count, sizeof(*ls));
    if (ls == NULL) {
        goto cleanup;
    }
    for (int i = 0; i < config->listener_count; i++) {
        ls[i] = listener_init(b, config);
        if (ls[i] == NULL) {
            res->status = BUS_INIT_ERROR_LISTENER_INIT_FAIL;
            goto cleanup;
        } else {
            BUS_LOG_SNPRINTF(b, 3, LOG_INITIALIZATION, b->udata, 64,
                "Initialized listener %d at %p", i, ls[i]);
        }
    }

    tp = threadpool_init(&config->threadpool_cfg);
    if (tp == NULL) {
        res->status = BUS_INIT_ERROR_THREADPOOL_INIT_FAIL;
        goto cleanup;
    }

    int thread_count = config->sender_count + config->listener_count;
    j = calloc(thread_count, sizeof(bool));
    pts = calloc(thread_count, sizeof(pthread_t));
    if (j == NULL || pts == NULL) {
        goto cleanup;
    }

    b->sender_count = config->sender_count;
    b->senders = ss;
    b->listener_count = config->listener_count;
    b->listeners = ls;
    b->threadpool = tp;
    b->joined = j;
    b->threads = pts;

    for (int i = 0; i < b->sender_count; i++) {
        int pcres = pthread_create(&b->threads[i], NULL,
            sender_mainloop, (void *)b->senders[i]);
        if (pcres != 0) {
            res->status = BUS_INIT_ERROR_PTHREAD_INIT_FAIL;
            goto cleanup;
        }
    }

    for (int i = 0; i < b->listener_count; i++) {
        int pcres = pthread_create(&b->threads[i + b->sender_count], NULL,
            listener_mainloop, (void *)b->listeners[i]);
        if (pcres != 0) {
            res->status = BUS_INIT_ERROR_PTHREAD_INIT_FAIL;
            goto cleanup;
        }
    }

    res->bus = b;
    BUS_LOG(b, 1, LOG_INITIALIZATION, "initialized", config->bus_udata);
    return true;
cleanup:
    if (ss) {
        for (int i = 0; i < config->sender_count; i++) {
            if (ss[i]) { sender_free(ss[i]); }
        }
        free(ss);
    }
    if (ls) {
        for (int i = 0; i < config->listener_count; i++) {
            if (ls[i]) { listener_free(ls[i]); }
        }
        free(ls);
    }
    if (tp) { threadpool_free(tp); }
    if (j) { free(j); }
    if (b) {
        if (log_lock_init) {
            pthread_mutex_destroy(&b->log_lock);
        }
        free(b);
    }

    if (pts) { free(pts); }

    return false;
}

/* Pack message to deliver on behalf of the user into an envelope
 * that can track status / routing along the way.
 *
 * The box should only ever be accessible on a single thread at a time. */
static boxed_msg *box_msg(struct bus *b, bus_user_msg *msg) {
    boxed_msg *box = calloc(1, sizeof(*box));
    if (box == NULL) { return NULL; }

    BUS_LOG_SNPRINTF(b, 3, LOG_MEMORY, b->udata, 64,
        "Allocated boxed message -- %p", box);

    box->fd = msg->fd;
    assert(msg->fd != 0);
    box->out_seq_id = msg->seq_id;
    box->out_msg_size = msg->msg_size;
    /* FIXME: should this be copied by pointer or value? */
    box->out_msg = msg->msg;

    box->cb = msg->cb;
    box->udata = msg->udata;
    return box;
}

static bool is_resumable_io_error(int errno_) {
    return errno_ == EAGAIN || errno_ == EINTR || errno_ == EWOULDBLOCK;
}

bool bus_send_request(struct bus *b, bus_user_msg *msg)
{
    if (b == NULL || msg == NULL || msg->fd == 0) {
        return false;
    }

    boxed_msg *box = box_msg(b, msg);
    if (box == NULL) {
        return false;
    }

    int complete_fd = 0;

    int s_id = sender_id_of_socket(b, msg->fd);
    struct sender *s = b->senders[s_id];

    /* Pass boxed message to the sender */
    if (!sender_enqueue_message(s, box, &complete_fd)) {
        BUS_LOG(b, 3, LOG_SENDING_REQUEST, "sender_enqueue_message failed", b->udata);
        return false;
    }

    /* block until delivery status */
    BUS_LOG(b, 3, LOG_SENDING_REQUEST, "Sending request...", b->udata);
    return poll_on_completion(b, complete_fd);
}

static bool poll_on_completion(struct bus *b, int fd) {
    /* POLL in a pipe */
    struct pollfd fds[1];
    fds[0].fd = fd;
    fds[0].events = POLLIN;

    /* FIXME: compare this to TCP timeouts -- try to prevent sender
     * succeeding but failing to notify client. */
    const int TIMEOUT_SECONDS = 10;
    const int ONE_SECOND = 1000;  // msec

    for (int i = 0; i < TIMEOUT_SECONDS; i++) {
        BUS_LOG(b, 5, LOG_SENDING_REQUEST, "Polling on completion...tick...", b->udata);
        int res = poll(fds, 1, ONE_SECOND);
        if (res == -1) {
            if (is_resumable_io_error(errno)) {
                BUS_LOG(b, 3, LOG_SENDING_REQUEST, "Polling on completion...EAGAIN", b->udata);
                errno = 0;
            } else {
                assert(false);
                break;
            }
        } else if (res > 0) {
            uint8_t read_buf[2];
            
            BUS_LOG(b, 3, LOG_SENDING_REQUEST, "Reading alert pipe...", b->udata);
            ssize_t sz = read(fd, read_buf, 2);

            if (sz == 2) {
                /* Payload: little-endian uint16_t, msec of backpressure. */
                uint16_t msec = (read_buf[0] << 0) + (read_buf[1] << 8);
                if (msec > 0) {
                    BUS_LOG_SNPRINTF(b, 5, LOG_SENDING_REQUEST, b->udata, 64,
                        " -- backpressure of %d msec", msec);
                    usleep(1000L * msec);
                    //(void)poll(fds, 0, msec);
                }
                BUS_LOG(b, 3, LOG_SENDING_REQUEST, "sent!", b->udata);
                return true;
            } else if (sz == -1) {
                if (is_resumable_io_error(errno)) {
                    errno = 0;
                } else {
                    assert(false);
                    break;
                }
            }
        }
    }
    BUS_LOG(b, 2, LOG_SENDING_REQUEST, "failed to send (timeout)", b->udata);
    return false;
}

static int sender_id_of_socket(struct bus *b, int fd) {
    /* Just evenly divide sockets between senders by file descriptor. */
    /* could also use sequence ID */
    return fd % b->sender_count;
}

static int listener_id_of_socket(struct bus *b, int fd) {
    /* Just evenly divide sockets between listeners by file descriptor. */
    return fd % b->listener_count;
}

struct sender *bus_get_sender_for_socket(struct bus *b, int fd) {
    return b->senders[sender_id_of_socket(b, fd)];
}

struct listener *bus_get_listener_for_socket(struct bus *b, int fd) {
    return b->listeners[listener_id_of_socket(b, fd)];
}

/* Get the string key for a log event ID. */
const char *bus_log_event_str(log_event_t event) {
    switch (event) {
    case LOG_INITIALIZATION: return "INITIALIZATION";
    case LOG_NEW_CLIENT: return "NEW_CLIENT";
    case LOG_SOCKET_REGISTERED: return "SOCKET_REGISTERED";
    case LOG_SENDING_REQUEST: return "SEND_REQUEST";
    case LOG_SHUTDOWN: return "SHUTDOWN";
    case LOG_SENDER: return "SENDER";
    case LOG_LISTENER: return "LISTENER";
    case LOG_MEMORY: return "MEMORY";
    default:
        return "UNKNOWN";
    }
}

bool bus_register_socket(struct bus *b, int fd, void *udata) {
    /* Register a socket internally with a listener. */
    int l_id = listener_id_of_socket(b, fd);

    BUS_LOG_SNPRINTF(b, 2, LOG_SOCKET_REGISTERED, b->udata, 64,
        "registering socket %d", fd);

    /* Spread sockets throughout the different listener processes. */
    struct listener *l = b->listeners[l_id];
    
    int pipes[2];
    if (0 != pipe(pipes)) {
        BUS_LOG(b, 1, LOG_SOCKET_REGISTERED, "pipe failure", b->udata);
        return false;
    }

    int pipe_out = pipes[0];
    int pipe_in = pipes[1];

    connection_info *ci = malloc(sizeof(*ci));
    if (ci == NULL) { goto cleanup; }

    ci->fd = fd;
    ci->to_read_size = 0;
    ci->udata = udata;

    bool res = listener_add_socket(l, ci, pipe_in);
    if (!res) { goto cleanup; }

    BUS_LOG(b, 2, LOG_SOCKET_REGISTERED, "polling on socket add...", b->udata);
    bool completed = poll_on_completion(b, pipe_out);
    if (!completed) { goto cleanup; }

    close(pipe_out);
    close(pipe_in);

    BUS_LOG(b, 2, LOG_SOCKET_REGISTERED, "successfully added socket", b->udata);
    return true;
cleanup:
    if (ci) {
        free(ci);
    }
    close(pipe_out);
    close(pipe_in);
    BUS_LOG(b, 2, LOG_SOCKET_REGISTERED, "failed to add socket", b->udata);
    return false;
}

bool bus_schedule_threadpool_task(struct bus *b, struct threadpool_task *task,
        size_t *backpressure) {
    return threadpool_schedule(b->threadpool, task, backpressure);
}

bool bus_shutdown(bus *b) {
    BUS_LOG(b, 2, LOG_SHUTDOWN, "shutting down sender threads", b->udata);
    for (int i = 0; i < b->sender_count; i++) {
        int off = 0;
        if (!b->joined[i + off]) {
            while (!sender_shutdown(b->senders[i])) {
                sleep(1);
            }
            void *unused = NULL;
            int res = pthread_join(b->threads[i + off], &unused);
            assert(res == 0);
            b->joined[i + off] = true;
        }
    }

    BUS_LOG(b, 2, LOG_SHUTDOWN, "shutting down listener threads", b->udata);
    for (int i = 0; i < b->listener_count; i++) {
        int off = b->sender_count;
        if (!b->joined[i + off]) {
            while (!listener_shutdown(b->listeners[i])) {
                sleep(1);
            }
            void *unused = NULL;
            int res = pthread_join(b->threads[i + off], &unused);
            assert(res == 0);
            b->joined[i + off] = true;
        }
    }

    BUS_LOG(b, 2, LOG_SHUTDOWN, "done with shutdown", b->udata);
    return true;
}

void bus_lock_log(struct bus *b) {
    pthread_mutex_lock(&b->log_lock);
}

void bus_unlock_log(struct bus *b) {
    pthread_mutex_unlock(&b->log_lock);
}

static void box_execute_cb(void *udata) {
    boxed_msg *box = (boxed_msg *)udata;

    void *out_udata = box->udata;
    bus_msg_result_t res = box->result;
    bus_msg_cb *cb = box->cb;

    free(box);
    cb(&res, out_udata);
}

static void box_cleanup_cb(void *udata) {
    boxed_msg *box = (boxed_msg *)udata;
    free(box);
}

/* Deliver a boxed message to the thread pool to execute.
 * The boxed message will be freed by the threadpool. */
bool bus_process_boxed_message(struct bus *b,
        struct boxed_msg *box, size_t *backpressure) {
    assert(box);
    assert(box->result.status != BUS_SEND_UNDEFINED);

    struct threadpool_task task = {
        .task = box_execute_cb,
        .cleanup = box_cleanup_cb,
        .udata = box,
    };

    BUS_LOG_SNPRINTF(b, 3, LOG_MEMORY, b->udata, 128,
        "Scheduling boxed message -- %p -- where it will be freed", box);
    return bus_schedule_threadpool_task(b, &task, backpressure);
}

void bus_free(bus *b) {
    if (b == NULL) { return; }
    bus_shutdown(b);

    for (int i = 0; i < b->sender_count; i++) {
        sender_free(b->senders[i]);
    }
    free(b->senders);

    for (int i = 0; i < b->listener_count; i++) {
        listener_free(b->listeners[i]);
    }
    free(b->listeners);

    threadpool_free(b->threadpool);

    free(b->joined);
    free(b->threads);

    pthread_mutex_destroy(&b->log_lock);

    free(b);
}

static void noop_log_cb(log_event_t event,
        int log_level, const char *msg, void *udata) {
    (void)event;
    (void)log_level;
    (void)msg;
    (void)udata;
}

static void noop_error_cb(bus_unpack_cb_res_t result, void *socket_udata) {
    (void)result;
    (void)socket_udata;
}