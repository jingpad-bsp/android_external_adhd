/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdlib.h>
#include <syslog.h>

#include "cras_config.h"
#include "cras_iodev_list.h"
#include "cras_messages.h"
#include "cras_rclient.h"
#include "cras_rstream.h"
#include "cras_types.h"
#include "cras_util.h"
#include "utlist.h"

/* An attached client.  This has a list of audio connections and a file
 * descriptor for communication with the client that isn't time critical. */
struct cras_rclient {
	size_t id;
	int fd; /* Connection for client communication. */
	struct cras_rstream *streams;
};

/* Handles a message from the client to connect a new stream */
static int handle_client_stream_connect(struct cras_rclient *client,
					const struct cras_connect_message *msg)
{
	struct cras_rstream *stream = NULL;
	struct cras_iodev *iodev;
	struct cras_client_stream_connected reply;
	struct cras_audio_format fmt;
	int rc;
	int aud_fd = 0;
	size_t buffer_frames, cb_threshold, min_cb_level;

	/* Find the iodev for this new connection and connect to it. */
	iodev = cras_get_iodev_for_stream_type(msg->stream_type,
					       msg->direction);
	if (!iodev) {
		rc = -ENODEV;
		goto reply_err;
	}
	fmt = msg->format;
	/* Tell the iodev about the format we want.  fmt will contain the actual
	 * format used after return. */
	cras_iodev_set_format(iodev, &fmt);

	/* Scale parameters to the frame rate of the device. */
	buffer_frames = cras_frames_at_rate(msg->format.frame_rate,
					    msg->buffer_frames,
					    fmt.frame_rate);
	cb_threshold = cras_frames_at_rate(msg->format.frame_rate,
					   msg->cb_threshold,
					   fmt.frame_rate);
	min_cb_level = cras_frames_at_rate(msg->format.frame_rate,
					   msg->min_cb_level,
					   fmt.frame_rate);

	/* Create the stream with the modified parameters. */
	rc = cras_rstream_create(msg->stream_id,
				 msg->stream_type,
				 msg->direction,
				 &fmt,
				 buffer_frames,
				 cb_threshold,
				 min_cb_level,
				 msg->flags,
				 client,
				 &stream);
	if (rc < 0)
		goto reply_err;

	/* Connect to client's audio socket. */
	aud_fd = cras_server_connect_to_client_socket(msg->stream_id);
	if (aud_fd < 0) {
		rc = aud_fd;
		goto reply_err;
	}
	cras_rstream_set_audio_fd(stream, aud_fd);

	/* Now can pass the stream to the device. */
	DL_APPEND(client->streams, stream);
	rc = cras_iodev_attach_stream(iodev, stream);
	if (rc < 0) {
		DL_DELETE(client->streams, stream);
		goto reply_err;
	}

	/* Tell client about the stream setup. */
	syslog(LOG_DEBUG, "Send connected for stream %x\n", msg->stream_id);
	cras_fill_client_stream_connected(
			&reply,
			0, /* No error. */
			msg->stream_id,
			fmt,
			cras_rstream_get_shm_key(stream),
			cras_rstream_get_total_shm_size(stream));
	rc = cras_rclient_send_message(client, &reply.header);
	if (rc < 0) {
		syslog(LOG_ERR, "Failed to send connected messaged\n");
		return rc;
	}

	return 0;

reply_err:
	/* Send the error code to the client. */
	cras_fill_client_stream_connected(&reply, rc, msg->stream_id,
					  msg->format, 0, 0);
	cras_rclient_send_message(client, &reply.header);

	if (rc && stream) {
		if (aud_fd >= 0)
			close(aud_fd);
		cras_rstream_destroy(stream);
	}
	return rc;
}

/* Removes the stream from the current device it is being played/captured on and
 * from the list of streams for the client. */
static int disconnect_client_stream(struct cras_rclient *client,
				    struct cras_rstream *stream)
{
	if (stream->iodev != NULL)
		cras_iodev_detach_stream(stream->iodev, stream);
	close(cras_rstream_get_audio_fd(stream));
	DL_DELETE(client->streams, stream);
	cras_rstream_destroy(stream);
	return 0;
}

/* Handles messages from the client requesting that a stream be removed from the
 * server. */
static int handle_client_stream_disconnect(
		struct cras_rclient *client,
		const struct cras_disconnect_stream_message *msg)
{
	struct cras_rstream *to_disconnect;

	DL_SEARCH_SCALAR(client->streams, to_disconnect, stream_id,
			 msg->stream_id);
	if (!to_disconnect)
		return -EINVAL;

	return disconnect_client_stream(client, to_disconnect);
}

/* Handles a request to move all streams of a type to an iodev at index */
static int handle_switch_stream_type_iodev(
		struct cras_rclient *client,
		const struct cras_switch_stream_type_iodev *msg)
{
	syslog(LOG_DEBUG, "move stream type %d to iodev %zu\n",
	       msg->stream_type, msg->iodev_idx);
	return cras_iodev_move_stream_type(msg->stream_type, msg->iodev_idx);
}

/*
 * Exported Functions.
 */

/* Creates a client structure and sends a message back informing the client that
 * the conneciton has succeeded. */
struct cras_rclient *cras_rclient_create(int fd, size_t id)
{
	struct cras_rclient *client;
	struct cras_client_connected msg;

	client = calloc(1, sizeof(struct cras_rclient));
	if (!client)
		return NULL;

	client->fd = fd;
	client->id = id;

	cras_fill_client_connected(&msg, client->id);
	cras_rclient_send_message(client, &msg.header);

	return client;
}

/* Removes all streams that the client owns and destroys it. */
void cras_rclient_destroy(struct cras_rclient *client)
{
	struct cras_rstream *stream, *tmp;
	DL_FOREACH_SAFE(client->streams, stream, tmp) {
		disconnect_client_stream(client, stream);
	}
	free(client);
}

/* Entry point for handling a message from the client.  Called from the main
 * server context. */
int cras_rclient_message(struct cras_rclient *client,
			 const struct cras_message *msg) {
	assert(client && msg);

	switch (msg->id) {
	case AUD_SERV_CLIENT_STREAM_CONNECT:
		handle_client_stream_connect(client,
			(const struct cras_connect_message *)msg);
		break;
	case AUD_SERV_CLIENT_STREAM_DISCONNECT:
		handle_client_stream_disconnect(client,
			(const struct cras_disconnect_stream_message *)msg);
		break;
	case AUD_SERV_SWITCH_STREAM_TYPE_IODEV:
		handle_switch_stream_type_iodev(client,
			(const struct cras_switch_stream_type_iodev *)msg);
		break;
	default:
		break;
	}

	return 0;
}

/* Sends a message to the client. */
int cras_rclient_send_message(const struct cras_rclient *client,
			      const struct cras_message *msg)
{
	return write(client->fd, msg, msg->length);
}
