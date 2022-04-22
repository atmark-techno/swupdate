/*
 * (C) Copyright 2015
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This is a simple example how to control swupdate
 * triggering for a new update, streaming ore or more images
 * and asking for the result.
 * This program is not thought to be used as it is, but as example
 * how to use the swupdateipc library.
 * A common use case is when swupdate is in double-copy option
 * and not in rescue, and the communication with the external world
 * is realized by a custom application. The new software image can be
 * also loaded by the main application and then streamed to swupdate
 * via the IPC protocol.
 *
 * The library performs an async update: the user initializes the
 * library with callbacks that are called for each phase of the update process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>
#include "network_ipc.h"

static void usage(void) {
	fprintf(stdout, "client [OPTIONS] <image .swu to be installed>...\n");
	fprintf(stdout, " With - or no swu file given, read from STDIN.\n");
	fprintf(stdout,
		" Available OPTIONS\n"
		" -h : print help and exit\n"
		" -d : ask the server to only perform a dry run\n"
		" -e, --select <software>,<mode> : Select software images set and source\n"
		"                                  Ex.: stable,main\n"
		" -q : go quiet, resets verbosity\n"
		" -v : go verbose, essentially print upgrade status messages from server\n"
		" -p : ask the server to run post-update commands if upgrade succeeds\n"
		);
}

char buf[256];
int fd = STDIN_FILENO;
int verbose = 1;
bool dry_run = false;
bool run_postupdate = false;
int end_status = EXIT_SUCCESS;
char *software_set = NULL, *running_mode = NULL;

static pthread_mutex_t mymutex;
static pthread_cond_t cv_end = PTHREAD_COND_INITIALIZER;

/*
 * this is the callback to get a new chunk of the
 * image.
 * It is called by a thread generated by the library and
 * can block.
 */
static int readimage(char **p, int *size) {
	int ret;

	ret = read(fd, buf, sizeof(buf));

	*p = buf;

	*size = ret;

	return ret;
}

/*
 * This is called by the library to inform
 * about the current status of the upgrade
 */
static int printstatus(ipc_message *msg)
{
	if (verbose)
		fprintf(stdout, "Status: %d message: %s\n",
			msg->data.notify.status,
			msg->data.notify.msg);

	return 0;
}

/*
 * this is called at the end reporting the status
 * of the upgrade and running any post-update actions
 * if successful
 */
static int end(RECOVERY_STATUS status)
{
	end_status = (status == SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;

	fprintf(stdout, "SWUpdate %s\n",
		status == FAILURE ? "*failed* !" :
			"was successful !");

	if (status == SUCCESS && run_postupdate) {
		fprintf(stdout, "Executing post-update actions.\n");
		ipc_message msg;
		msg.data.procmsg.len = 0;
		if (ipc_postupdate(&msg) != 0 || msg.type != ACK) {
			fprintf(stderr, "Running post-update failed!\n");
			end_status = EXIT_FAILURE;
		}
	}

	pthread_mutex_lock(&mymutex);
	pthread_cond_signal(&cv_end);
	pthread_mutex_unlock(&mymutex);

	return 0;
}

/*
 * Send file to main swupdate process
 */
static int send_file(const char* filename) {
	int rc;
	if (filename && (fd = open(filename, O_RDONLY)) < 0) {
		fprintf(stderr, "Unable to open %s\n", filename);
		return EXIT_FAILURE;
	}

	/* May be set non-zero by end() function on failure */
	end_status = EXIT_SUCCESS;

	struct swupdate_request req;
	swupdate_prepare_req(&req);
	if (dry_run)
		req.dry_run = RUN_DRYRUN;
	if (software_set && strlen(software_set)) {
		strncpy(req.software_set, software_set, sizeof(req.software_set) - 1);
		strncpy(req.running_mode, running_mode, sizeof(req.running_mode) - 1);
	}
	rc = swupdate_async_start(readimage, printstatus,
				end, &req, sizeof(req));

	/* return if we've hit an error scenario */
	if (rc < 0) {
		fprintf(stderr, "swupdate_async_start returns %d\n", rc);
		pthread_mutex_unlock(&mymutex);
		close(fd);
		return EXIT_FAILURE;
	}

	/* End called, unlock and exit */
	pthread_mutex_lock(&mymutex);
	pthread_cond_wait(&cv_end, &mymutex);
	pthread_mutex_unlock(&mymutex);

	if (filename)
		close(fd);

	return end_status;
}


/*
 * Simple example, it does nothing but calling the library
 */
int main(int argc, char *argv[]) {
	int c;
	char *pos;

	pthread_mutex_init(&mymutex, NULL);

	/* parse command line options */
	while ((c = getopt(argc, argv, "dhqvpe:")) != EOF) {
		switch (c) {
		case 'd':
			dry_run = true;
			break;
		case 'h':
			usage();
			return 0;
		case 'q':
			verbose = 0;
			break;
		case 'v':
			verbose++;
			break;
		case 'e':
			pos = strchr(optarg, ',');
			if (pos == NULL) {
				fprintf(stderr, "Wrong selection %s\n", optarg);
				exit (EXIT_FAILURE);
			}
			*pos++ = '\0';
			software_set = optarg;
			running_mode = pos;
			break;
		case 'p':
			run_postupdate = true;
			break;
		default:
			usage();
			return -1;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 0 || (argc == 1 && strcmp(argv[0], "-") == 0)) {
		fprintf(stdout, "no input given, reading from STDIN...\n");
		if (send_file(NULL)) exit(1);
	} else {
		for (int i = 0; i < argc; i++) {
			if (send_file(argv[i])) exit(1);
		}
	}

	exit(0);
}

