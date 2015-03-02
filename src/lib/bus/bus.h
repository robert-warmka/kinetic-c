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
#ifndef BUS_H
#define BUS_H

#include "bus_types.h"

/* This opaque bus struct represents the only user-facing interface to
 * the network handling code. Callbacks are provided to react to network
 * events. */

/* Initialize a bus, based on configuration in *config. Returns a bool
 * indicating whether the construction succeeded, and the bus pointer
 * and/or a status code indicating the cause of failure in *res. */
bool bus_init(bus_config *config, struct bus_result *res);

/* Send a request. Blocks until the request has been transmitted.
 * 
 * Assumes the FD has been registered with bus_register_socket;
 * sending to an unregistered socket is an error.
 *
 * Returns true if the request has been accepted and the bus will
 * attempt to handle the request and response. They can still fail,
 * but the error status will be passed to the result handler callback.
 *
 * Returns false if the request has been rejected, due to a memory
 * allocation error or invalid arguments.
 * */
bool bus_send_request(struct bus *b, bus_user_msg *msg);

/* Get the string key for a log event ID. */
const char *bus_log_event_str(log_event_t event);

/* Register a socket connected to an endpoint, and data that will be passed
 * to all interactions on that socket.
 * 
 * The socket will have request -> response messages with timeouts, as
 * well as unsolicited status messages.
 *
 * If USES_SSL is true, then the function will block until the initial
 * SSL/TLS connection handshake has completed. */
bool bus_register_socket(struct bus *b, bus_socket_t type, int fd, void *socket_udata);

/* Free metadata about a socket that has been disconnected. */
bool bus_release_socket(struct bus *b, int fd, void **socket_udata_out);

/* Begin shutting the system down. Returns true once everything pending
 * has resolved. */
bool bus_shutdown(struct bus *b);

/* For a given file descriptor, get the listener ID to use.
 * This will level sockets between multiple threads. */
struct listener *bus_get_listener_for_socket(struct bus *b, int fd);

/* Schedule a task in the bus's threadpool. */
bool bus_schedule_threadpool_task(struct bus *b, struct threadpool_task *task,
    size_t *backpressure);

/* Lock / unlock the log mutex, since logging can occur on several threads. */
void bus_lock_log(struct bus *b);
void bus_unlock_log(struct bus *b);

/* Free internal data structures for the bus. */
void bus_free(struct bus *b);

/* Deliver a boxed message to the thread pool to execute. */
bool bus_process_boxed_message(struct bus *b,
    struct boxed_msg *box, size_t *backpressure);

void bus_backpressure_delay(struct bus *b, size_t backpressure, uint8_t shift);

#endif
