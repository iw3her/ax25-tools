#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include "config.h"
#include "user_io.h"

#define BUFLEN	8192
#ifndef PENDING_CAP
#define PENDING_CAP	(BUFLEN * 4)
#endif

int compression;
int paclen_in		= 256;
int paclen_out		= 256;

/* This is for select_loop() */
static unsigned char buf[BUFLEN];

struct pending_output {
	int fd;
	int active;
	int seqpacket;
	unsigned char *data;
	size_t capacity;
	size_t len;
	size_t off;
};

static struct pending_output pending_outputs[2] = {
	{ .fd = -1 },
	{ .fd = -1 }
};

#ifdef HAVE_ZLIB_H
#include <zlib.h>

/* Error in (de)compression happened? */
static int compression_error;

/* These are for the (de)compressor */
static unsigned char input_buffer[BUFLEN];
static unsigned char compression_input[BUFLEN];

static z_stream incoming_stream;
static z_stream outgoing_stream;
static int outgoing_work_active;
#endif

#define min(a,b)	((a) < (b) ? (a) : (b))

void err(char *message)
{
	user_write(STDOUT_FILENO, message, strlen(message));
	exit(1);
}

void init_compress(void)
{
#ifdef HAVE_ZLIB_H
	compression_error = 0;
	outgoing_work_active = 0;
	inflateInit(&incoming_stream);
	deflateInit(&outgoing_stream, 9);

	incoming_stream.next_in = input_buffer;
#else
	err("*** Compression support not available!!!\r\n");
#endif
}

void end_compress(void)
{
#ifdef HAVE_ZLIB_H
	inflateEnd(&incoming_stream);
	deflateEnd(&outgoing_stream);
#endif
}

static struct pending_output *pending_for_fd(int fd)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (pending_outputs[i].fd == fd)
			return &pending_outputs[i];
	}

	for (i = 0; i < 2; i++) {
		if (pending_outputs[i].fd == -1) {
			pending_outputs[i].fd = fd;
			return &pending_outputs[i];
		}
	}

	errno = EMFILE;
	return NULL;
}

static void pending_release(struct pending_output *output)
{
	free(output->data);
	output->data = NULL;
	output->capacity = 0;
	output->off = 0;
	output->len = 0;
	output->active = 0;
	output->seqpacket = 0;
	output->fd = -1;
}

static void pending_release_all(void)
{
	int i;

	for (i = 0; i < 2; i++)
		pending_release(&pending_outputs[i]);
}

static int pending_reserve(struct pending_output *output, size_t extra)
{
	size_t needed;
	size_t capacity;
	unsigned char *data;

	if (extra > PENDING_CAP - output->len) {
		errno = ENOBUFS;
		return -1;
	}
	needed = output->len + extra;
	if (needed <= output->capacity)
		return 0;

	capacity = output->capacity ? output->capacity : extra;
	while (capacity < needed) {
		if (capacity > PENDING_CAP / 2) {
			capacity = PENDING_CAP;
			break;
		}
		capacity *= 2;
	}
	if (capacity > PENDING_CAP)
		capacity = PENDING_CAP;
	data = realloc(output->data, capacity);
	if (data == NULL)
		return -1;
	output->data = data;
	output->capacity = capacity;
	return 0;
}

/* Return 1 for SOCK_SEQPACKET, 0 for non-sockets/other socket types, -1 when
 * the descriptor is a socket but its type cannot be determined. */
static int socket_is_seqpacket(int fd)
{
	int type;
	socklen_t type_len = sizeof(type);

	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_len) < 0) {
		if (errno == ENOTSOCK)
			return 0;
		return -1;
	}

	return type == SOCK_SEQPACKET;
}

static int pending_append(struct pending_output *output,
		const void *data, size_t count)
{
	if (output->off && count > output->capacity - output->len) {
		memmove(output->data, output->data + output->off,
			output->len - output->off);
		output->len -= output->off;
		output->off = 0;
	}
	if (pending_reserve(output, count) < 0)
		return -1;

	memcpy(output->data + output->len, data, count);
	output->len += count;
	output->active = 1;
	return 0;
}

/* Return 0 when the output remains pending, and -1 on a permanent error. */
static int flush_pending(struct pending_output *output)
{
	size_t count;
	ssize_t written;

	while (output->off < output->len) {
		count = output->len - output->off;
		if (output->seqpacket && count > (size_t)paclen_out)
			count = paclen_out;

		written = write(output->fd, output->data + output->off, count);
		if (written > 0) {
			if (output->seqpacket && (size_t)written != count) {
				errno = EPROTO;
				return -1;
			}
			output->off += written;
			continue;
		}
		if (written == 0) {
			errno = EPIPE;
			return -1;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}

	output->active = 0;
	output->len = 0;
	output->off = 0;
	return 0;
}

#ifdef HAVE_ZLIB_H
/* Continue one buffered Z_PARTIAL_FLUSH operation.  The input remains in
 * compression_input and zlib's avail_in/next_in fields remain authoritative
 * until the operation completes. */
static int resume_compression(struct pending_output *output)
{
	while (outgoing_work_active) {
		size_t space;
		size_t output_space;
		size_t produced;
		uInt old_avail_in;
		int status;

		if (output->active) {
			if (flush_pending(output) < 0)
				return -1;
			if (output->active) {
				errno = EAGAIN;
				return -1;
			}
		}

		space = PENDING_CAP - output->len;
		if (space == 0) {
			errno = EAGAIN;
			return -1;
		}
		output_space = min(BUFLEN, space);
		if (pending_reserve(output, output_space) < 0)
			return -1;

		outgoing_stream.next_out = output->data + output->len;
		outgoing_stream.avail_out = output_space;
		old_avail_in = outgoing_stream.avail_in;
		status = deflate(&outgoing_stream, Z_PARTIAL_FLUSH);
		produced = output_space - outgoing_stream.avail_out;
		output->len += produced;
		if (produced)
			output->active = 1;

		if (status != Z_OK) {
			compression_error = status;
			errno = 0;
			return -1;
		}
		if (outgoing_stream.avail_in == 0 &&
			outgoing_stream.avail_out != 0) {
			outgoing_work_active = 0;
			return flush_pending(output);
		}
		if (produced == 0 && old_avail_in == outgoing_stream.avail_in) {
			compression_error = Z_BUF_ERROR;
			errno = 0;
			return -1;
		}
		if (flush_pending(output) < 0)
			return -1;
		if (output->active) {
			errno = EAGAIN;
			return -1;
		}
	}

	return 0;
}
#endif

int user_write(int fd, const void *buf, size_t count)
{
	struct pending_output *output;
	int seqpacket;

	if (count == 0)
		return 0;

	output = pending_for_fd(fd);
	if (output == NULL)
		return -1;
	if (output->active) {
		errno = EAGAIN;
		return -1;
	}
	seqpacket = socket_is_seqpacket(fd);
	if (seqpacket < 0)
		return -1;
	output->seqpacket = seqpacket;

#ifndef HAVE_ZLIB_H
	if (pending_append(output, buf, count) < 0)
		return -1;
	if (flush_pending(output) < 0)
		return -1;
	return count;
#else
	/* Only output to stdout can be compressed */
	if (fd != STDOUT_FILENO || !compression) {
		if (pending_append(output, buf, count) < 0)
			return -1;
		else if (flush_pending(output) < 0)
			return -1;
		else
			return count;
	}

	if (compression_error) {
		errno = 0;
		return -1;
	}

	if (outgoing_work_active || output->active) {
		errno = EAGAIN;
		return -1;
	}
	if (count > sizeof(compression_input)) {
		errno = EMSGSIZE;
		return -1;
	}
	memcpy(compression_input, buf, count);
	outgoing_stream.next_in = compression_input;
	outgoing_stream.avail_in = count;
	outgoing_work_active = 1;
	if (resume_compression(output) < 0)
		return -1;
	return outgoing_work_active ? -1 : (int)count;
#endif
}

int user_read(int fd, void *buf, size_t count)
{
#ifdef HAVE_ZLIB_H
	int status, len;
#endif

	if (count == 0)
		return 0;

#ifndef HAVE_ZLIB_H
	return read(fd, buf, count);
#else
	/* Only input from stdin can be compressed */
	if (fd != STDIN_FILENO || !compression)
		return read(fd, buf, count);

	if (compression_error) {
		errno = 0;
		return -1;
	}

	incoming_stream.next_out = buf;
	incoming_stream.avail_out = count;

	for (;;) {
		status = inflate(&incoming_stream, Z_SYNC_FLUSH);

		if (count - incoming_stream.avail_out > 0)
			return count - incoming_stream.avail_out;

		if (status != Z_OK && status != Z_BUF_ERROR) {
			compression_error = status;
			errno = 0;
			return -1;
		}

		incoming_stream.next_in = input_buffer;
		incoming_stream.avail_in = 0;

		len = read(fd, input_buffer, BUFLEN);
		if (len <= 0)
			return len;

		incoming_stream.avail_in = len;
	}

	return 0;
#endif
}

int select_loop(int s)
{
	fd_set read_fd;
	fd_set write_fd;
	struct pending_output *to_socket;
	struct pending_output *to_stdout;
	int n;
	int nfds;
	size_t read_count;
	int flags;
	int socket_seqpacket;
	int socket_eof = 0;
	int stdin_eof = 0;
	int result = 0;

	if (fcntl(s, F_SETFL, O_NONBLOCK) < 0) {
		close(s);
		return -1;
	}
	if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) < 0) {
		close(s);
		return -1;
	}
	flags = fcntl(STDOUT_FILENO, F_GETFL);
	if (flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(s);
		return -1;
	}

	to_socket = pending_for_fd(s);
	to_stdout = pending_for_fd(STDOUT_FILENO);
	if (to_socket == NULL || to_stdout == NULL)
		goto fail;
	socket_seqpacket = socket_is_seqpacket(s);
	if (socket_seqpacket < 0)
		goto fail;
	to_socket->seqpacket = socket_seqpacket;
	to_stdout->seqpacket = 0;
	if (socket_seqpacket && paclen_in > BUFLEN) {
		errno = EMSGSIZE;
		goto fail;
	}

	/*
	 * Loop until one end of the connection goes away.
	 */
	for (;;) {
		if (socket_eof && !to_stdout->active && !to_socket->active
#ifdef HAVE_ZLIB_H
			&& !outgoing_work_active
#endif
		)
			break;

		FD_ZERO(&read_fd);
		FD_ZERO(&write_fd);
		if (!stdin_eof && !socket_eof && !to_socket->active)
			FD_SET(STDIN_FILENO, &read_fd);
		if (!socket_eof && !to_stdout->active
#ifdef HAVE_ZLIB_H
			&& !outgoing_work_active
#endif
		)
			FD_SET(s, &read_fd);
		if (to_socket->active)
			FD_SET(s, &write_fd);
		if (to_stdout->active)
			FD_SET(STDOUT_FILENO, &write_fd);

		nfds = s > STDOUT_FILENO ? s + 1 : STDOUT_FILENO + 1;
		if (select(nfds, &read_fd, &write_fd, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			goto fail;
		}

		if (FD_ISSET(s, &write_fd) && flush_pending(to_socket) < 0)
			goto fail;
		if (FD_ISSET(STDOUT_FILENO, &write_fd) &&
			flush_pending(to_stdout) < 0)
			goto fail;
#ifdef HAVE_ZLIB_H
		if (outgoing_work_active && !to_stdout->active &&
			resume_compression(to_stdout) < 0 &&
			errno != EAGAIN && errno != EWOULDBLOCK)
			goto fail;
#endif

		if (FD_ISSET(s, &read_fd)) {
			read_count = BUFLEN;
			if (socket_seqpacket && read_count > (size_t)paclen_in)
				read_count = paclen_in;
			n = user_read(s, buf, read_count);
			if (n > 0 && user_write(STDOUT_FILENO, buf, n) < 0 &&
				errno != EAGAIN && errno != EWOULDBLOCK)
				goto fail;
			if (n == 0) {
				socket_eof = 1;
			} else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				goto fail;
			}
		}

		if (!socket_eof && FD_ISSET(STDIN_FILENO, &read_fd)) {
			read_count = BUFLEN;
			if (to_socket->seqpacket && read_count > (size_t)paclen_out)
				read_count = paclen_out;
			n = user_read(STDIN_FILENO, buf, read_count);
			if (n > 0 && user_write(s, buf, n) < 0 &&
				errno != EAGAIN && errno != EWOULDBLOCK)
				goto fail;
			if (n == 0) {
				stdin_eof = 1;
			} else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				goto fail;
			}
		}
	}

	goto cleanup;

	fail:
	result = -1;

	cleanup:
	close(s);
	pending_release_all();
	return result;
}
