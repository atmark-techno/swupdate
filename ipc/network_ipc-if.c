/*
 * (C) Copyright 2008-2020
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <pthread.h>
#include <inttypes.h>
#include "network_ipc.h"

static pthread_t async_thread_id;

struct async_lib {
	int connfd;
	int status;
	writedata	wr;
	getstatus	get;
	terminated	end;
};

static enum async_thread_state {
	ASYNC_THREAD_INIT,
	ASYNC_THREAD_RUNNING,
	ASYNC_THREAD_DONE
} running = ASYNC_THREAD_INIT;

static struct async_lib request;

#define get_request()	(&request)

static void *swupdate_async_thread(void *data)
{
	char *pbuf;
	int size;
	sigset_t sigpipe_mask;
	sigset_t saved_mask;
	struct timespec zerotime = {0, 0};
	struct async_lib *rq = (struct async_lib *)data;
	int notify_fd, ret, maxfd;
	fd_set rfds, wfds;
	ipc_message msg;
	msg.data.notify.status = RUN;

	sigemptyset(&sigpipe_mask);
	sigaddset(&sigpipe_mask, SIGPIPE);

	if (pthread_sigmask(SIG_BLOCK, &sigpipe_mask, &saved_mask) == -1) {
		perror("pthread_sigmask");
		msg.data.notify.status = FAILURE;
		goto out;
	}

	notify_fd = ipc_notify_connect();
	if (notify_fd < 0) {
		perror("could not setup notify fd");
		msg.data.status.last_result = FAILURE;
		goto out;
	}

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	maxfd = (notify_fd > rq->connfd ? notify_fd : rq->connfd) + 1;

	/* Start writing the image until end */
	do {
		if (!rq->wr)
			break;

		FD_SET(notify_fd, &rfds);
		FD_SET(rq->connfd, &wfds);
		ret = select(maxfd, &rfds, &wfds, NULL, NULL);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("select");
			msg.data.status.last_result = FAILURE;
			goto out;
		}

		if (FD_ISSET(rq->connfd, &wfds)) {
			rq->wr(&pbuf, &size);
			if (size) {
				if (swupdate_image_write(pbuf, size) != size) {
					perror("swupdate_image_write failed");
					msg.data.status.last_result = FAILURE;
					goto out;
				}
			}
		}

		/* handle any notification coming */
		while ((ret = ipc_notify_receive(&notify_fd, &msg, 0))
				!= -ETIMEDOUT) {
			if (ret < 0) {
				perror("ipc_notify receive failed");
				msg.data.status.last_result = FAILURE;
				goto out;
			}
			if (rq->get)
				rq->get(&msg);
		}
	} while(size > 0);

	ipc_end(rq->connfd);

	/* Everything sent, wait until we are IDLE again */
	while (msg.data.notify.status != IDLE) {
		ret = ipc_notify_receive(&notify_fd, &msg, -1);
		if (ret < 0) {
			perror("ipc_notify receive failed");
			msg.data.status.last_result = FAILURE;
			goto out;
		}
		if (rq->get)
			rq->get(&msg);
	}
	ipc_end(notify_fd);

	if (sigtimedwait(&sigpipe_mask, 0, &zerotime) == -1) {
		// currently ignored
	}

	if (pthread_sigmask(SIG_SETMASK, &saved_mask, 0) == -1) {
		perror("pthread_sigmask");
		msg.data.notify.status = FAILURE;
		goto out;
	}

out:
	running = ASYNC_THREAD_DONE;
	if (rq->end) {
		/* Get status to get update return code */
		ret = ipc_get_status(&msg);
		if (ret < 0) {
			perror("ipc_get_status failed");
			msg.data.status.last_result = FAILURE;
			goto out;
		}

		rq->end(msg.data.status.last_result);
	}

	pthread_exit((void*)(intptr_t)(msg.data.notify.status == SUCCESS));
}

/*
 * This is duplicated from pctl
 * to let build the ipc library without
 * linking pctl code
 */
static void start_ipc_thread(void *(* start_routine) (void *), void *arg)
{
	int ret;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	ret = pthread_create(&async_thread_id, &attr, start_routine, arg);
	if (ret) {
		perror("ipc thread creation failed");
		return;
	}

	running = ASYNC_THREAD_RUNNING;
}

/*
 * This is part of the library for an external client.
 * Only one running request is accepted
 */
int swupdate_async_start(writedata wr_func, getstatus status_func,
				terminated end_func, void *priv, ssize_t size)
{
	struct async_lib *rq;
	int connfd;

	switch (running) {
	case ASYNC_THREAD_INIT:
		break;
	case ASYNC_THREAD_DONE:
		pthread_join(async_thread_id, NULL);
		running = ASYNC_THREAD_INIT;
		break;
	default:
		return -EBUSY;
	}

	rq = get_request();

	rq->wr = wr_func;
	rq->get = status_func;
	rq->end = end_func;

	connfd = ipc_inst_start_ext(priv, size);

	if (connfd < 0)
		return connfd;

	rq->connfd = connfd;

	start_ipc_thread(swupdate_async_thread, rq);

	return running != ASYNC_THREAD_INIT;
}

int swupdate_image_write(char *buf, int size)
{
	struct async_lib *rq;

	rq = get_request();

	return ipc_send_data(rq->connfd, buf, size);
}

/*
 * Set via IPC the AES key for decryption
 * key is passed as ASCII string
 */
int swupdate_set_aes(char *key, char *ivt)
{
	ipc_message msg;

	if (!key || !ivt)
		return -EINVAL;
	if (strlen(key) != 64 && strlen(ivt) != 32)
		return -EINVAL;

	memset(&msg, 0, sizeof(msg));

	msg.magic = IPC_MAGIC;
	msg.type = SET_AES_KEY;

	/*
	 * Lenght for key and IVT are fixed
	 */
	strncpy(msg.data.aeskeymsg.key_ascii, key, sizeof(msg.data.aeskeymsg.key_ascii) - 1);
	strncpy(msg.data.aeskeymsg.ivt_ascii, ivt, sizeof(msg.data.aeskeymsg.ivt_ascii) - 1);

	return ipc_send_cmd(&msg);
}

/*
 * Set via IPC the range of accepted versions
 * Versions are string and they can use semver
 */
int swupdate_set_version_range(const char *minversion,
				const char *maxversion,
				const char *currentversion)
{
	ipc_message msg;

	memset(&msg, 0, sizeof(msg));
	msg.magic = IPC_MAGIC;
	msg.type = SET_VERSIONS_RANGE;

	if (minversion) {
		strncpy(msg.data.versions.minimum_version,
			minversion,
			sizeof(msg.data.versions.minimum_version) - 1);
	}

	if (maxversion) {
		strncpy(msg.data.versions.maximum_version,
			maxversion,
			sizeof(msg.data.versions.maximum_version) - 1);
	}

	if (currentversion) {
		strncpy(msg.data.versions.current_version,
			currentversion,
			sizeof(msg.data.versions.maximum_version) - 1);
	}

	return ipc_send_cmd(&msg);
}

void swupdate_prepare_req(struct swupdate_request *req) {
	if (!req)
		return;
	memset(req, 0, sizeof(struct swupdate_request));
	req->apiversion = SWUPDATE_API_VERSION;
	req->dry_run = RUN_DEFAULT;
	return;
}
