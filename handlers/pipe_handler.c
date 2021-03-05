/*
 * (C) Copyright 2021
 * Dominique Martinet, Atmark Techno, dominique.martinet@atmark-techno.com
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "swupdate.h"
#include "handler.h"
#include "util.h"

struct pipe_priv {
	pid_t pid;
	/* pipes from the point of view of the spawned process */
	int stdin;
	int stdout;
	int stderr;
	int status;

	char stdout_buf[SWUPDATE_GENERAL_STRING_SIZE];
	int stdout_index;
	char stderr_buf[SWUPDATE_GENERAL_STRING_SIZE];
	int stderr_index;
};

/* This helper polls a process stdout/stderr and forwards these to
 * TRACE/ERROR appropriately.
 * It stops when there is nothing left on stdout/stderr if the
 * process terminated or it was requested to write and stdin is writable
 */
static int pipe_poll_process(struct pipe_priv *priv, int write)
{
	int ret = 0;
	int wstatus;

	while (1) {
		fd_set readfds;
		fd_set writefds;
		struct timeval tv = { .tv_sec = 1, };
		int max_fd;
		int n = 0;

		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		FD_SET(priv->stdout, &readfds);
		FD_SET(priv->stderr, &readfds);
		max_fd = max(priv->stdout, priv->stderr);
		if (write) {
			FD_SET(priv->stdin, &writefds);
			max_fd = max(max_fd, priv->stdin);
		}

		ret = select(max_fd + 1, &readfds, &writefds, NULL, &tv);
		if (ret < 0) {
			ret = -errno;
			ERROR("select failed: %d\n", -ret);
			return ret;
		}
		if (FD_ISSET(priv->stdout, &readfds)) {
			ret = read_lines_notify(priv->stdout, priv->stdout_buf,
						SWUPDATE_GENERAL_STRING_SIZE,
						&priv->stdout_index, TRACELEVEL);
			if (ret < 0) {
				ERROR("Could not read stdout: %d\n", ret);
				return ret;
			}
			n += ret;
		}
		if (FD_ISSET(priv->stderr, &readfds)) {
			ret = read_lines_notify(priv->stderr, priv->stderr_buf,
						SWUPDATE_GENERAL_STRING_SIZE,
						&priv->stderr_index, ERRORLEVEL);
			if (ret < 0) {
				ERROR("Could not read stderr: %d\n", ret);
				return ret;
			}
			n += ret;
		}
		/* keep reading from stdout/stderr if there was anything */
		if (n > 0)
			continue;

		/* return if process exited */
		pid_t w = waitpid(priv->pid, &wstatus, WNOHANG);
		if (w < 0) {
			ret = -errno;
			ERROR("Could not waitpid: %d", -ret);
			return ret;
		}
		if (w == priv->pid) {
			if (WIFEXITED(wstatus)) {
				priv->status = WEXITSTATUS(wstatus);
				ret = -priv->status;
				TRACE("Command returned %d", -ret);
			} else if (WIFSIGNALED(wstatus)) {
				priv->status = 1;
				ret = -1;
				TRACE("Command killed by signal %d",
				      WTERMSIG(wstatus));
			} else {
				priv->status = 1;
				ERROR("wait returned but no exit code nor signal?");
				ret = -1;
			}
			return ret;
		}

		/* or if we can write */
		if (write && FD_ISSET(priv->stdin, &writefds))
			return 0;
	}
}

static int pipe_copy_callback(void *out, const void *buf, unsigned int len)
{
	struct pipe_priv *priv = out;
	int ret;

	/* check data from subprocess */
	ret = pipe_poll_process(priv, 1);
	if (ret < 0)
		return ret;

	/* let copy_write do the actual copying */
	return copy_write(&priv->stdin, buf, len);
}

static int pipe_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct pipe_priv priv = { .status = -1, };
	int ret, pollret;
	int stdinpipe[2], stdoutpipe[2], stderrpipe[2];
	char *cmd = dict_get_value(&img->properties, "cmd");
	if (!cmd) {
		ERROR("Pipe handler needs a command to pipe data into: please set the 'cmd' property");
		return -EINVAL;
	}

	if (pipe(stdinpipe) < 0) {
		ERROR("Could not create process pipes");
		return -EFAULT;
	}
	if (pipe(stdoutpipe) < 0) {
		ERROR("Could not create process pipes");
		close(stdinpipe[0]);
		close(stdinpipe[1]);
		return -EFAULT;
	}
	if (pipe(stderrpipe) < 0) {
		ERROR("Could not create process pipes");
		close(stdinpipe[0]);
		close(stdinpipe[1]);
		close(stdoutpipe[0]);
		close(stdoutpipe[1]);
		return -EFAULT;
	}

	priv.pid = fork();
	if (priv.pid == 0) {
		/* child process: cleanup fds and exec */
		if (dup2(stdinpipe[0], STDIN_FILENO) < 0)
			exit(1);
		if (dup2(stdoutpipe[1], STDOUT_FILENO) < 0)
			exit(1);
		if (dup2(stderrpipe[1], STDERR_FILENO) < 0)
			exit(1);
		close(stdinpipe[0]);
		close(stdinpipe[1]);
		close(stdoutpipe[0]);
		close(stdoutpipe[1]);
		close(stderrpipe[0]);
		close(stderrpipe[1]);

		ret = execl("/bin/sh", "sh", "-c", cmd, NULL);
		ERROR("Cannot execute pipe handler: %s", strerror(errno));
		exit(1);
	}

	/* parent process */
	close(stdinpipe[0]);
	close(stdoutpipe[1]);
	close(stderrpipe[1]);
	priv.stdin = stdinpipe[1];
	priv.stdout = stdoutpipe[0];
	priv.stderr = stderrpipe[0];

	/* pipe data to process. Ignoring sigpipe lets the handler error
	 * properly when writing to a broken pipe instead of exiting */
	signal(SIGPIPE, SIG_IGN);
	ret = copyimage(&priv, img, pipe_copy_callback);
	if (ret < 0) {
		ERROR("Error copying data to pipe");
	}

	/* close stdin and keep reading process stdout/stderr until it exits
	 * (skip if already exited) */
	close(priv.stdin);
	if (priv.status == -1) {
		pollret = pipe_poll_process(&priv, 0);
		/* keep original error if we had any */
		if (!ret)
			ret = pollret;
	}
	close(priv.stdout);
	close(priv.stderr);

	/* empty trailing buffers */
	if (priv.stdout_index)
		TRACE("%s", priv.stdout_buf);
	if (priv.stderr_index)
		ERROR("%s", priv.stderr_buf);

	TRACE("finished piping image");
	return ret;
}

__attribute__((constructor))
static void pipe_handler(void)
{
	register_handler("pipe", pipe_image,
			 IMAGE_HANDLER | FILE_HANDLER, NULL);
}
