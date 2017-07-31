/*
 * Copyright 2008-2017 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */
#include <aerospike/as_event.h>
#include <aerospike/as_event_internal.h>
#include <aerospike/as_admin.h>
#include <aerospike/as_async.h>
#include <aerospike/as_log_macros.h>
#include <aerospike/as_pipe.h>
#include <aerospike/as_proto.h>
#include <aerospike/as_socket.h>
#include <aerospike/as_status.h>
#include <aerospike/as_tls.h>
#include <citrusleaf/alloc.h>
#include <citrusleaf/cf_byte_order.h>
#include <errno.h>

/******************************************************************************
 * GLOBALS
 *****************************************************************************/

extern int as_event_send_buffer_size;
extern int as_event_recv_buffer_size;
extern bool as_event_threads_created;

/******************************************************************************
 * LIBEV FUNCTIONS
 *****************************************************************************/

#if defined(AS_USE_LIBEV)

static void
as_ev_close_loop(as_event_loop* event_loop)
{
	ev_async_stop(event_loop->loop, &event_loop->wakeup);
	
	// Only stop event loop if client created event loop.
	if (as_event_threads_created) {
		ev_unloop(event_loop->loop, EVUNLOOP_ALL);
	}
	
	// Cleanup event loop resources.
	as_queue_destroy(&event_loop->queue);
	as_queue_destroy(&event_loop->pipe_cb_queue);
	pthread_mutex_destroy(&event_loop->lock);
}

static void
as_ev_wakeup(struct ev_loop* loop, ev_async* wakeup, int revents)
{
	// Read command pointers from queue.
	as_event_loop* event_loop = wakeup->data;
	as_event_commander cmd;
	uint32_t i = 0;

	// Only process original size of queue.  Recursive pre-registration errors can
	// result in new commands being added while the loop is in process.  If we process
	// them, we could end up in an infinite loop.
	pthread_mutex_lock(&event_loop->lock);
	uint32_t size = as_queue_size(&event_loop->queue);
	bool status = as_queue_pop(&event_loop->queue, &cmd);
	pthread_mutex_unlock(&event_loop->lock);

	while (status) {
		if (! cmd.executable) {
			// Received stop signal.
			as_ev_close_loop(event_loop);
			return;
		}
		cmd.executable(cmd.udata);

		if (++i < size) {
			pthread_mutex_lock(&event_loop->lock);
			status = as_queue_pop(&event_loop->queue, &cmd);
			pthread_mutex_unlock(&event_loop->lock);
		}
		else {
			break;
		}
	}
}

static void*
as_ev_worker(void* udata)
{
	struct ev_loop* loop = udata;
	ev_loop(loop, 0);
	ev_loop_destroy(loop);
	as_tls_thread_cleanup();
	return NULL;
}

static inline void
as_ev_init_loop(as_event_loop* event_loop)
{
	ev_async_init(&event_loop->wakeup, as_ev_wakeup);
	event_loop->wakeup.data = event_loop;
	ev_async_start(event_loop->loop, &event_loop->wakeup);	
}

bool
as_event_create_loop(as_event_loop* event_loop)
{
	event_loop->loop = ev_loop_new(EVFLAG_AUTO);
	
	if (! event_loop->loop) {
		as_log_error("Failed to create event loop");
		return false;
	}
	as_ev_init_loop(event_loop);
	
	return pthread_create(&event_loop->thread, NULL, as_ev_worker, event_loop->loop) == 0;
}

void
as_event_register_external_loop(as_event_loop* event_loop)
{
	// This method is only called when user sets an external event loop.
	as_ev_init_loop(event_loop);
}

bool
as_event_execute(as_event_loop* event_loop, as_event_executable executable, void* udata)
{
	// Send command through queue so it can be executed in event loop thread.
	pthread_mutex_lock(&event_loop->lock);
	as_event_commander qcmd = {.executable = executable, .udata = udata};
	bool queued = as_queue_push(&event_loop->queue, &qcmd);
	pthread_mutex_unlock(&event_loop->lock);

	if (queued) {
		ev_async_send(event_loop->loop, &event_loop->wakeup);
	}
	return queued;
}

static inline void
as_ev_watch_write(as_event_command* cmd)
{
	as_event_connection* conn = cmd->conn;
	int watch = cmd->pipe_listener != NULL ? EV_WRITE | EV_READ : EV_WRITE;

	// Skip if we're already watching the right stuff.
	if (watch == conn->watching) {
		return;
	}
	conn->watching = watch;

	ev_io_stop(cmd->event_loop->loop, &conn->watcher);
	ev_io_set(&conn->watcher, conn->socket.fd, watch);
	ev_io_start(cmd->event_loop->loop, &conn->watcher);
}

static inline void
as_ev_watch_read(as_event_command* cmd)
{
	as_event_connection* conn = cmd->conn;
	int watch = EV_READ;

	// Skip if we're already watching the right stuff.
	if (watch == conn->watching) {
		return;
	}
	conn->watching = watch;

	ev_io_stop(cmd->event_loop->loop, &conn->watcher);
	ev_io_set(&conn->watcher, conn->socket.fd, watch);
	ev_io_start(cmd->event_loop->loop, &conn->watcher);
}

#define AS_EVENT_WRITE_COMPLETE 0
#define AS_EVENT_WRITE_INCOMPLETE 1
#define AS_EVENT_WRITE_ERROR 2

#define AS_EVENT_READ_COMPLETE 3
#define AS_EVENT_READ_INCOMPLETE 4
#define AS_EVENT_READ_ERROR 5

#define AS_EVENT_TLS_NEED_READ 6
#define AS_EVENT_TLS_NEED_WRITE 7

#define AS_EVENT_COMMAND_DONE 8

static int
as_ev_write(as_event_command* cmd)
{
	uint8_t* buf = (uint8_t*)cmd + cmd->write_offset;

	if (cmd->conn->socket.ctx) {
		do {
			int rv = as_tls_write_once(&cmd->conn->socket, buf + cmd->pos, cmd->len - cmd->pos);
			if (rv > 0) {
				as_ev_watch_write(cmd);
				cmd->pos += rv;
				continue;
			}
			else if (rv == -1) {
				// TLS sometimes need to read even when we are writing.
				as_ev_watch_read(cmd);
				return AS_EVENT_TLS_NEED_READ;
			}
			else if (rv == -2) {
				// TLS wants a write, we're all set for that.
				as_ev_watch_write(cmd);
				return AS_EVENT_WRITE_INCOMPLETE;
			}
			else if (rv < -2) {
				if (! as_event_socket_retry(cmd)) {
					as_error err;
					as_socket_error(cmd->conn->socket.fd, cmd->node, &err, AEROSPIKE_ERR_TLS_ERROR, "TLS write failed", rv);
					as_event_socket_error(cmd, &err);
				}
				return AS_EVENT_WRITE_ERROR;
			}
			// as_tls_write_once can't return 0
		} while (cmd->pos < cmd->len);
	}
	else {
		int fd = cmd->conn->socket.fd;
		ssize_t bytes;
	
		do {
#if defined(__linux__)
			bytes = send(fd, buf + cmd->pos, cmd->len - cmd->pos, MSG_NOSIGNAL);
#else
			bytes = write(fd, buf + cmd->pos, cmd->len - cmd->pos);
#endif
			if (bytes > 0) {
				cmd->pos += bytes;
				continue;
			}
		
			if (bytes < 0) {
				if (errno == EWOULDBLOCK) {
					as_ev_watch_write(cmd);
					return AS_EVENT_WRITE_INCOMPLETE;
				}

				if (! as_event_socket_retry(cmd)) {
					as_error err;
					as_socket_error(fd, cmd->node, &err, AEROSPIKE_ERR_ASYNC_CONNECTION, "Socket write failed", errno);
					as_event_socket_error(cmd, &err);
				}
				return AS_EVENT_WRITE_ERROR;
			}
			else {
				if (! as_event_socket_retry(cmd)) {
					as_error err;
					as_socket_error(fd, cmd->node, &err, AEROSPIKE_ERR_ASYNC_CONNECTION, "Socket write closed by peer", 0);
					as_event_socket_error(cmd, &err);
				}
				return AS_EVENT_WRITE_ERROR;
			}
		} while (cmd->pos < cmd->len);
	}

	// Socket timeout applies only to read events.
	// Reset event received because we are switching from a write to a read state.
	// This handles case where write succeeds and read event does not occur.  If we didn't reset,
	// the socket timeout would go through two iterations (double the timeout) because a write
	// event occurred in the first timeout period.
	cmd->flags &= ~AS_ASYNC_FLAGS_EVENT_RECEIVED;
	return AS_EVENT_WRITE_COMPLETE;
}

static int
as_ev_read(as_event_command* cmd)
{
	cmd->flags |= AS_ASYNC_FLAGS_EVENT_RECEIVED;

	if (cmd->conn->socket.ctx) {
		do {
			int rv = as_tls_read_once(&cmd->conn->socket, cmd->buf + cmd->pos, cmd->len - cmd->pos);
			if (rv > 0) {
				as_ev_watch_read(cmd);
				cmd->pos += rv;
				continue;
			}
			else if (rv == -1) {
				// TLS wants a read
				as_ev_watch_read(cmd);
				return AS_EVENT_READ_INCOMPLETE;
			}
			else if (rv == -2) {
				// TLS sometimes needs to write, even when the app is reading.
				as_ev_watch_write(cmd);
				return AS_EVENT_TLS_NEED_WRITE;
			}
			else if (rv < -2) {
				if (! as_event_socket_retry(cmd)) {
					as_error err;
					as_socket_error(cmd->conn->socket.fd, cmd->node, &err, AEROSPIKE_ERR_TLS_ERROR, "TLS read failed", rv);
					as_event_socket_error(cmd, &err);
				}
				return AS_EVENT_READ_ERROR;
			}
			// as_tls_read_once doesn't return 0
		} while (cmd->pos < cmd->len);
	}
	else {
		int fd = cmd->conn->socket.fd;
		ssize_t bytes;
	
		do {
			bytes = read(fd, cmd->buf + cmd->pos, cmd->len - cmd->pos);
		
			if (bytes > 0) {
				cmd->pos += bytes;
				continue;
			}
		
			if (bytes < 0) {
				if (errno == EWOULDBLOCK) {
					as_ev_watch_read(cmd);
					return AS_EVENT_READ_INCOMPLETE;
				}

				if (! as_event_socket_retry(cmd)) {
					as_error err;
					as_socket_error(fd, cmd->node, &err, AEROSPIKE_ERR_ASYNC_CONNECTION, "Socket read failed", errno);
					as_event_socket_error(cmd, &err);
				}
				return AS_EVENT_READ_ERROR;
			}
			else {
				if (! as_event_socket_retry(cmd)) {
					as_error err;
					as_socket_error(fd, cmd->node, &err, AEROSPIKE_ERR_ASYNC_CONNECTION, "Socket read closed by peer", 0);
					as_event_socket_error(cmd, &err);
				}
				return AS_EVENT_READ_ERROR;
			}
		} while (cmd->pos < cmd->len);
	}
	
	return AS_EVENT_READ_COMPLETE;
}

static inline void
as_ev_command_read_start(as_event_command* cmd)
{
	cmd->len = sizeof(as_proto);
	cmd->pos = 0;
	cmd->state = AS_ASYNC_STATE_COMMAND_READ_HEADER;

	as_ev_watch_read(cmd);
	
	if (cmd->pipe_listener != NULL) {
		as_pipe_read_start(cmd);
	}
}

void
as_event_command_write_start(as_event_command* cmd)
{
	as_event_set_write(cmd);
	cmd->state = AS_ASYNC_STATE_COMMAND_WRITE;
	as_ev_watch_write(cmd);

	if (as_ev_write(cmd) == AS_EVENT_WRITE_COMPLETE) {
		// Done with write. Register for read.
		as_ev_command_read_start(cmd);
	}
}

static int
as_ev_command_peek_block(as_event_command* cmd)
{
	// Batch, scan, query may be waiting on end block.
	// Prepare for next message block.
	cmd->len = sizeof(as_proto);
	cmd->pos = 0;
	cmd->state = AS_ASYNC_STATE_COMMAND_READ_HEADER;

	int rv = as_ev_read(cmd);
	if (rv != AS_EVENT_READ_COMPLETE) {
		return rv;
	}
	
	as_proto* proto = (as_proto*)cmd->buf;
	as_proto_swap_from_be(proto);
	size_t size = proto->sz;
	
	cmd->len = (uint32_t)size;
	cmd->pos = 0;
	cmd->state = AS_ASYNC_STATE_COMMAND_READ_BODY;
	
	// Check for end block size.
	if (cmd->len == sizeof(as_msg)) {
		// Look like we received end block.  Read and parse to make sure.
		rv = as_ev_read(cmd);
		if (rv != AS_EVENT_READ_COMPLETE) {
			return rv;
		}

		if (! cmd->parse_results(cmd)) {
			// We did not finish after all. Prepare to read next header.
			cmd->len = sizeof(as_proto);
			cmd->pos = 0;
			cmd->state = AS_ASYNC_STATE_COMMAND_READ_HEADER;
		}
		else {
			return AS_EVENT_COMMAND_DONE;
		}
	}
	else {
		// Received normal data block.  Stop reading for fairness reasons and wait
		// till next iteration.
		if (cmd->len > cmd->read_capacity) {
			if (cmd->flags & AS_ASYNC_FLAGS_FREE_BUF) {
				cf_free(cmd->buf);
			}
			cmd->buf = cf_malloc(size);
			cmd->read_capacity = cmd->len;
			cmd->flags |= AS_ASYNC_FLAGS_FREE_BUF;
		}
	}

	return AS_EVENT_READ_COMPLETE;
}

static int
as_ev_parse_authentication(as_event_command* cmd)
{
	int rv;
	if (cmd->state == AS_ASYNC_STATE_AUTH_READ_HEADER) {
		// Read response length
		rv = as_ev_read(cmd);
		if (rv != AS_EVENT_READ_COMPLETE) {
			return rv;
		}
		as_event_set_auth_parse_header(cmd);

		if (cmd->len > cmd->read_capacity) {
			as_error err;
			as_error_update(&err, AEROSPIKE_ERR_CLIENT, "Authenticate response size is corrupt: %u", cmd->len);
			as_event_parse_error(cmd, &err);
			return AS_EVENT_READ_ERROR;
		}
	}

	rv = as_ev_read(cmd);
	if (rv != AS_EVENT_READ_COMPLETE) {
		return rv;
	}
	
	// Parse authentication response.
	uint8_t code = cmd->buf[AS_ASYNC_AUTH_RETURN_CODE];
	
	if (code) {
		// Can't authenticate socket, so must close it.
		as_error err;
		as_error_update(&err, code, "Authentication failed: %s", as_error_string(code));
		as_event_parse_error(cmd, &err);
		return AS_EVENT_READ_ERROR;
	}
	
	as_event_command_write_start(cmd);
	return AS_EVENT_READ_COMPLETE;
}

static int
as_ev_command_read(as_event_command* cmd)
{
	int rv;

	if (cmd->state == AS_ASYNC_STATE_COMMAND_READ_HEADER) {
		// Read response length
		rv = as_ev_read(cmd);
		if (rv != AS_EVENT_READ_COMPLETE) {
			return rv;
		}
		
		as_proto* proto = (as_proto*)cmd->buf;
		as_proto_swap_from_be(proto);
		size_t size = proto->sz;
		
		cmd->len = (uint32_t)size;
		cmd->pos = 0;
		cmd->state = AS_ASYNC_STATE_COMMAND_READ_BODY;
		
		if (cmd->len > cmd->read_capacity) {
			if (cmd->flags & AS_ASYNC_FLAGS_FREE_BUF) {
				cf_free(cmd->buf);
			}
			cmd->buf = cf_malloc(size);
			cmd->read_capacity = cmd->len;
			cmd->flags |= AS_ASYNC_FLAGS_FREE_BUF;
		}
	}
	
	// Read response body
	rv = as_ev_read(cmd);
	if (rv != AS_EVENT_READ_COMPLETE) {
		return rv;
	}

	if (! cmd->parse_results(cmd)) {
		// Batch, scan, query is not finished.
		return as_ev_command_peek_block(cmd);
	}

	return AS_EVENT_COMMAND_DONE;		
}

bool
as_ev_tls_connect(as_event_command* cmd, as_event_connection* conn)
{
	int rv = as_tls_connect_once(&conn->socket);
	if (rv < -2) {
		if (! as_event_socket_retry(cmd)) {
			// Failed, error has been logged.
			as_error err;
			as_error_set_message(&err, AEROSPIKE_ERR_TLS_ERROR, "TLS connection failed");
			as_event_socket_error(cmd, &err);
		}
		return false;
	}
	else if (rv == -1) {
		// TLS needs a read.
		as_ev_watch_read(cmd);
	}
	else if (rv == -2) {
		// TLS needs a write.
		as_ev_watch_write(cmd);
	}
	else if (rv == 0) {
		if (! as_event_socket_retry(cmd)) {
			as_error err;
			as_error_set_message(&err, AEROSPIKE_ERR_TLS_ERROR, "TLS connection shutdown");
			as_event_socket_error(cmd, &err);
		}
		return false;
	}
	else
	{
		// TLS connection established.
		if (cmd->cluster->user) {
			as_event_set_auth_write(cmd);
			cmd->state = AS_ASYNC_STATE_AUTH_WRITE;
		}
		else {
			cmd->state = AS_ASYNC_STATE_COMMAND_WRITE;
		}
		as_ev_watch_write(cmd);
	}
	return true;
}

static void
as_ev_callback_common(as_event_command* cmd, as_event_connection* conn) {
	switch (cmd->state) {
	case AS_ASYNC_STATE_TLS_CONNECT:
		do {
			if (! as_ev_tls_connect(cmd, conn)) {
				return;
			}
		} while (as_tls_read_pending(&cmd->conn->socket) > 0);
		break;

	case AS_ASYNC_STATE_AUTH_READ_HEADER:
	case AS_ASYNC_STATE_AUTH_READ_BODY:
		// If we're using TLS we must loop until there are no bytes
		// left in the encryption buffer because we won't get another
		// read event from libev.
		do {
			switch (as_ev_parse_authentication(cmd)) {
				case AS_EVENT_COMMAND_DONE:
				case AS_EVENT_READ_ERROR:
					// Do not touch cmd again because it's been deallocated.
					return;

				case AS_EVENT_READ_COMPLETE:
					as_ev_watch_read(cmd);
					break;

				default:
					break;
			}
		} while (as_tls_read_pending(&cmd->conn->socket) > 0);
		break;

	case AS_ASYNC_STATE_COMMAND_READ_HEADER:
	case AS_ASYNC_STATE_COMMAND_READ_BODY:
		// If we're using TLS we must loop until there are no bytes
		// left in the encryption buffer because we won't get another
		// read event from libev.
		do {
			switch (as_ev_command_read(cmd)) {
			case AS_EVENT_COMMAND_DONE:
			case AS_EVENT_READ_ERROR:
				// Do not touch cmd again because it's been deallocated.
				return;
			
			case AS_EVENT_READ_COMPLETE:
				as_ev_watch_read(cmd);
				break;
				
			default:
				break;
			}
		} while (as_tls_read_pending(&cmd->conn->socket) > 0);
		break;

	case AS_ASYNC_STATE_AUTH_WRITE:
	case AS_ASYNC_STATE_COMMAND_WRITE:
		as_ev_watch_write(cmd);
		
		if (as_ev_write(cmd) == AS_EVENT_WRITE_COMPLETE) {
			// Done with write. Register for read.
			if (cmd->state == AS_ASYNC_STATE_AUTH_WRITE) {
				as_event_set_auth_read_header(cmd);
				as_ev_watch_read(cmd);
			}
			else {
				as_ev_command_read_start(cmd);
			}
		}
		break;

	default:
		as_log_error("unexpected cmd state %d", cmd->state);
		break;
	}
}

static void
as_ev_callback(struct ev_loop* loop, ev_io* watcher, int revents)
{
	if (revents & EV_READ) {
		as_event_connection* conn = watcher->data;
		as_event_command* cmd;
		
		if (conn->pipeline) {
			as_pipe_connection* pipe = (as_pipe_connection*)conn;
			
			if (pipe->writer && cf_ll_size(&pipe->readers) == 0) {
				// Authentication response will only have a writer.
				cmd = pipe->writer;
			}
			else {
				// Next response is at head of reader linked list.
				cf_ll_element* link = cf_ll_get_head(&pipe->readers);
				
				if (link) {
					cmd = as_pipe_link_to_command(link);
				}
				else {
					as_log_debug("Pipeline read event ignored");
					return;
				}
			}
		}
		else {
			cmd = ((as_async_connection*)conn)->cmd;
		}

		as_ev_callback_common(cmd, conn);
	}
	else if (revents & EV_WRITE) {
		as_event_connection* conn = watcher->data;
		
		as_event_command* cmd = conn->pipeline ?
			((as_pipe_connection*)conn)->writer :
			((as_async_connection*)conn)->cmd;

		as_ev_callback_common(cmd, conn);
	}
	else if (revents & EV_ERROR) {
		as_log_error("Async error occurred: %d", revents);
	}
	else {
		as_log_warn("Unknown event received: %d", revents);
	}
}

static void
as_ev_watcher_init(as_event_command* cmd, as_socket* sock)
{
	as_event_connection* conn = cmd->conn;
	memcpy(&conn->socket, sock, sizeof(as_socket));

	if (cmd->cluster->tls_ctx.ssl_ctx) {
		cmd->state = AS_ASYNC_STATE_TLS_CONNECT;
	}
	else if (cmd->cluster->user) {
		as_event_set_auth_write(cmd);
		cmd->state = AS_ASYNC_STATE_AUTH_WRITE;
	}
	else {
		as_event_set_write(cmd);
		cmd->state = AS_ASYNC_STATE_COMMAND_WRITE;
	}

	int watch = cmd->pipe_listener != NULL ? EV_WRITE | EV_READ : EV_WRITE;
	conn->watching = watch;
	
	ev_io_init(&conn->watcher, as_ev_callback, conn->socket.fd, watch);
	conn->watcher.data = conn;
	ev_io_start(cmd->event_loop->loop, &conn->watcher);
}

static int
as_ev_try_connections(int fd, as_address* addresses, socklen_t size, int i, int max)
{
	while (i < max) {
		if (connect(fd, (struct sockaddr*)&addresses[i].addr, size) == 0 || errno == EINPROGRESS) {
			return i;
		}
		i++;
	}
	return -1;
}

static int
as_ev_try_family_connections(as_event_command* cmd, int family, int begin, int end, int index, as_address* primary, as_socket* sock)
{
	// Create a non-blocking socket.
	int fd = as_socket_create_fd(family);

	if (fd < 0) {
		return fd;
	}

	if (cmd->pipe_listener && ! as_pipe_modify_fd(fd)) {
		return -1000;
	}

	if (! as_socket_wrap(sock, family, fd, &cmd->cluster->tls_ctx, cmd->node->tls_name)) {
		return -1001;
	}

	// Try addresses.
	as_address* addresses = cmd->node->addresses;
	socklen_t size = (family == AF_INET)? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
	int rv;
	
	if (index >= 0) {
		// Try primary address.
		if (connect(fd, (struct sockaddr*)&primary->addr, size) == 0 || errno == EINPROGRESS) {
			return index;
		}
		
		// Start from current index + 1 to end.
		rv = as_ev_try_connections(fd, addresses, size, index + 1, end);
		
		if (rv < 0) {
			// Start from begin to index.
			rv = as_ev_try_connections(fd, addresses, size, begin, index);
		}
	}
	else {
		rv = as_ev_try_connections(fd, addresses, size, begin, end);
	}
	
	if (rv < 0) {
		// Couldn't start a connection on any socket address - close the socket.
		as_socket_close(sock);
		return -1002;
	}
	return rv;
}

static void
as_ev_connect_error(as_event_command* cmd, as_address* primary, int rv)
{
	// Socket has already been closed. Release connection.
	cf_free(cmd->conn);
	as_event_decr_conn(cmd);
	cmd->event_loop->errors++;

	if (as_event_command_retry(cmd, true)) {
		return;
	}

	const char* msg;
	rv = -rv;

	if (rv < 1000) {
		// rv is errno.
		msg = strerror(rv);
	}
	else {
		switch (rv) {
			case 1000:
				msg = "Failed to modify fd for pipeline";
				break;
			case 1001:
				msg = "Failed to wrap socket for TLS";
				break;
			default:
				msg = "Failed to connect";
				break;
		}
	}

	as_error err;
	as_error_update(&err, AEROSPIKE_ERR_ASYNC_CONNECTION, "%s: %s %s", msg, cmd->node->name, primary->name);

	// Only timer needs to be released on socket connection failure.
	// Watcher has not been registered yet.
	if (cmd->flags & AS_ASYNC_FLAGS_HAS_TIMER) {
		as_event_stop_timer(cmd);
	}
	as_event_error_callback(cmd, &err);
}

void
as_event_connect(as_event_command* cmd)
{
	// Try addresses.
	as_socket sock;
	as_node* node = cmd->node;
	uint32_t index = node->address_index;
	as_address* primary = &node->addresses[index];
	int rv;
	int first_rv;

	if (primary->addr.ss_family == AF_INET) {
		// Try IPv4 addresses first.
		rv = as_ev_try_family_connections(cmd, AF_INET, 0, node->address4_size, index, primary, &sock);
		
		if (rv < 0) {
			// Try IPv6 addresses.
			first_rv = rv;
			rv = as_ev_try_family_connections(cmd, AF_INET6, AS_ADDRESS4_MAX, AS_ADDRESS4_MAX + node->address6_size, -1, NULL, &sock);
		}
	}
	else {
		// Try IPv6 addresses first.
		rv = as_ev_try_family_connections(cmd, AF_INET6, AS_ADDRESS4_MAX, AS_ADDRESS4_MAX + node->address6_size, index, primary, &sock);
		
		if (rv < 0) {
			// Try IPv4 addresses.
			first_rv = rv;
			rv = as_ev_try_family_connections(cmd, AF_INET, 0, node->address4_size, -1, NULL, &sock);
		}
	}
	
	if (rv < 0) {
		as_ev_connect_error(cmd, primary, first_rv);
		return;
	}
	
	if (rv != index) {
		// Replace invalid primary address with valid alias.
		// Other threads may not see this change immediately.
		// It's just a hint, not a requirement to try this new address first.
		ck_pr_store_32(&node->address_index, rv);
		as_log_debug("Change node address %s %s", node->name, as_node_get_address_string(node));
	}

	as_ev_watcher_init(cmd, &sock);
	cmd->event_loop->errors = 0; // Reset errors on valid connection.
}

void
as_ev_total_timeout(struct ev_loop* loop, ev_timer* timer, int revents)
{
	// One-off timers are automatically stopped by libev.
	as_event_total_timeout(timer->data);
}

void
as_ev_socket_timeout(struct ev_loop* loop, ev_timer* timer, int revents)
{
	as_event_socket_timeout(timer->data);
}

void
as_event_close_connection(as_event_connection* conn)
{
	as_socket_close(&conn->socket);
	cf_free(conn);
}

static void
as_ev_close_connections(as_node* node, as_conn_pool* pool)
{
	as_event_connection* conn;
	
	// Queue connection commands to event loops.
	while (as_conn_pool_get(pool, &conn)) {
		as_socket_close(&conn->socket);
		cf_free(conn);
		as_conn_pool_dec(pool);
	}
	as_conn_pool_destroy(pool);
}

void
as_event_node_destroy(as_node* node)
{
	// Close connections.
	for (uint32_t i = 0; i < as_event_loop_size; i++) {
		as_ev_close_connections(node, &node->async_conn_pools[i]);
		as_ev_close_connections(node, &node->pipe_conn_pools[i]);
	}
	cf_free(node->async_conn_pools);
	cf_free(node->pipe_conn_pools);
}

#endif
