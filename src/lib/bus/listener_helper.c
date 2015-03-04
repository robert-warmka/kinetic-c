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
#include "listener_helper.h"
#include "listener_task.h"
#include "syscall.h"
#include "atomic.h"

#include <assert.h>

listener_msg *listener_helper_get_free_msg(listener *l) {
    struct bus *b = l->bus;

    BUS_LOG_SNPRINTF(b, 4, LOG_LISTENER, b->udata, 128,
        "get_free_msg -- in use: %d", l->msgs_in_use);

    for (;;) {
        listener_msg *head = l->msg_freelist;
        if (head == NULL) {
            BUS_LOG(b, 3, LOG_LISTENER, "No free messages!", b->udata);
            return NULL;
        } else if (ATOMIC_BOOL_COMPARE_AND_SWAP(&l->msg_freelist, head, head->next)) {
            for (;;) {
                int16_t miu = l->msgs_in_use;
                
                if (ATOMIC_BOOL_COMPARE_AND_SWAP(&l->msgs_in_use, miu, miu + 1)) {
                    BUS_LOG(l->bus, 5, LOG_LISTENER, "got free msg", l->bus->udata);

                    /* Add counterpressure between the client and the listener.
                     * 10 * ((n >> 1) ** 2) microseconds */
                    int16_t delay = 10 * (miu >> 1) * (miu >> 1);
                    if (delay > 0) {
                        struct timespec ts = {
                            .tv_sec = 0,
                            .tv_nsec = 1000L * delay,
                        };
                        nanosleep(&ts, NULL);
                    }
                    BUS_ASSERT(b, b->udata, head->type == MSG_NONE);
                    memset(&head->u, 0, sizeof(head->u));
                    return head;
                }
            }
        }
    }
}

bool listener_helper_push_message(struct listener *l, listener_msg *msg, int *reply_fd) {
    struct bus *b = l->bus;
    BUS_ASSERT(b, b->udata, msg);
  
    uint8_t msg_buf[sizeof(msg->id)];
    msg_buf[0] = msg->id;

    if (reply_fd) { *reply_fd = msg->pipes[0]; }

    for (;;) {
        ssize_t wr = syscall_write(l->commit_pipe, msg_buf, sizeof(msg_buf));
        if (wr == sizeof(msg_buf)) {
            return true;  // committed
        } else {
            if (errno == EINTR) { /* signal interrupted; retry */
                errno = 0;
            } else {
                BUS_LOG_SNPRINTF(b, 10, LOG_LISTENER, b->udata, 64,
                    "write_commit error, errno %d", errno);
                errno = 0;
                ListenerTask_ReleaseMsg(l, msg);
                return false;
            }
        }
    }
}

rx_info_t *listener_helper_get_free_rx_info(struct listener *l) {
    struct bus *b = l->bus;

    struct rx_info_t *head = l->rx_info_freelist;
    if (head == NULL) {
        BUS_LOG(b, 6, LOG_SENDER, "No rx_info cells left!", b->udata);
        return NULL;
    } else {
        l->rx_info_freelist = head->next;
        head->next = NULL;
        l->rx_info_in_use++;
        BUS_LOG(l->bus, 4, LOG_LISTENER, "reserving RX info", l->bus->udata);
        BUS_ASSERT(b, b->udata, head->state == RIS_INACTIVE);
        if (l->rx_info_max_used < head->id) {
            BUS_LOG_SNPRINTF(b, 5, LOG_LISTENER, b->udata, 128,
                "rx_info_max_used <- %d", head->id);
            l->rx_info_max_used = head->id;
            BUS_ASSERT(b, b->udata, l->rx_info_max_used < MAX_PENDING_MESSAGES); 
        }

        BUS_LOG_SNPRINTF(b, 5, LOG_LISTENER, b->udata, 128,
            "got free rx_info_t %d (%p)", head->id, (void *)head);
        BUS_ASSERT(b, b->udata, head == &l->rx_info[head->id]);
        return head;
    }
}

rx_info_t *listener_helper_get_hold_rx_info(listener *l, int fd, int64_t seq_id) {
    for (int i = 0; i <= l->rx_info_max_used; i++) {
        rx_info_t *info = &l->rx_info[i];
        if (info->state == RIS_HOLD &&
            info->u.hold.fd == fd &&
            info->u.hold.seq_id == seq_id) {
            return info;
        }
    }
    return NULL;
}