// SPDX-License-Identifier:     GPL-2.0-only

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "installer.h"
#include "semver.h"
#include "pctl.h"

static bool running_vendored = false;
#define VENDORED_SCRIPTS "/usr/libexec/mkswu/"
#define SKIP_SCRIPTS_MARKER "# DEBUG_SKIP_SCRIPTS\n"
static char *EMBEDDED_CLEANUP_SCRIPT;

static const char *LOCK_FILE = "/var/lock/swupdate.lock";
static const char *REBOOT_FILE = "/run/swupdate_rebooting";
static int lock_fd = -2;


static ssize_t get_vendored_scripts_version(char *version)
{
	int fd;
	ssize_t rc;

	fd = open(VENDORED_SCRIPTS "version", O_RDONLY);
	if (fd < 0)
		return -1;
	rc = read(fd, version, SWUPDATE_GENERAL_STRING_SIZE - 1);
	close(fd);


	if (rc >= 0) {
		while (rc > 0 && version[rc-1] == '\n') {
			rc--;
		}
		version[rc] = 0;
	}
	return rc;
}

static ssize_t mark_embedded_scripts_no_run(const char *swdescription)
{
	int fd;
	ssize_t rc;

	fd = open(swdescription, O_WRONLY|O_APPEND);
	if (fd < 0)
		return -1;

	rc = write(fd, SKIP_SCRIPTS_MARKER, sizeof(SKIP_SCRIPTS_MARKER) - 1);

	close(fd);
	return rc == sizeof(SKIP_SCRIPTS_MARKER) - 1 ? 0 : -1;
}

int mkswu_hook_pre(struct swupdate_cfg *software, const char *swdescription)
{
	char vendored_scripts_version[SWUPDATE_GENERAL_STRING_SIZE];

	/* reset flag (if installing multiple swu in a row) */
	running_vendored = false;

	/* skip everything if scripts aren't installed */
	if (get_vendored_scripts_version(vendored_scripts_version) <= 0) {
		TRACE("Using scripts from swu (vendored scripts not installed)");
		return 0;
	}

	/* Prefer scripts embedded in swu if version is sufficient */
	if (compare_versions(software->version, vendored_scripts_version) >= 0) {
		TRACE("Using scripts from swu (version %s >= %s)",
		      software->version, vendored_scripts_version);
		return 0;
	}
	TRACE("Using scripts from vendored directory (version %s > %s)",
	      vendored_scripts_version, software->version);

	/* flag sw-description so embedded scripts do not run */
	if (mark_embedded_scripts_no_run(swdescription)) {
		WARN("Could not update sw-description, falling back to older vendored scripts");
		return 0;
	}

	running_vendored = true;
	TRACE("Running mkswu pre script");
	if (software->parms.dry_run)
		return 0;
	return run_system_cmd(VENDORED_SCRIPTS "pre.sh");
}

int mkswu_hook_post(bool dry_run)
{
	if (!running_vendored) {
		return 0;
	}

	TRACE("Running mkswu post script");
	if (dry_run)
		return 0;
	return run_system_cmd(VENDORED_SCRIPTS "post.sh");
}

void mkswu_hook_cleanup(bool dry_run)
{
	const char *cleanup_script;

	if (running_vendored) {
		cleanup_script = VENDORED_SCRIPTS "cleanup.sh";
	} else {
		if (!EMBEDDED_CLEANUP_SCRIPT) {
			if (asprintf(&EMBEDDED_CLEANUP_SCRIPT, "%s%s",
				     get_tmpdirscripts(), "cleanup.sh") == -1) {
				EMBEDDED_CLEANUP_SCRIPT = (char *)
					"/var/tmp/" SCRIPTS_DIR_SUFFIX "cleanup.sh";
			}
		}
		cleanup_script = EMBEDDED_CLEANUP_SCRIPT;
	}

	if (access(cleanup_script, X_OK)) {
		TRACE("Skipping non-executable cleanup_script %s", cleanup_script);
		return;
	}

	TRACE("Running mkswu cleanup script");
	if (dry_run)
		return;
	run_system_cmd(cleanup_script);
}

int mkswu_lock(void)
{
	struct stat statbuf_fd, statbuf_path;

	// use a different path for regular user (this should only ever be used for tests)
	if (lock_fd == -2 && geteuid() != 0) {
		lock_fd = -1;
		asprintf((char**)&LOCK_FILE, "/tmp/.mkswu_lock_%d", geteuid());
	}

	// this should never happen in practice
	if (lock_fd >= 0)
		goto sanity_checks;

again:
	lock_fd = open(LOCK_FILE, O_WRONLY|O_CREAT, 0644);
	if (lock_fd < 0) {
		ERROR("Could not open mkswu lock file %s: %m", LOCK_FILE);
		return 1;
	}
	if (flock(lock_fd, LOCK_EX|LOCK_NB) < 0) {
		if (errno != EAGAIN || errno != EWOULDBLOCK) {
			ERROR("Could not take mkswu lock: %m");
			goto out_close;
		}
		INFO("Waiting for mkswu lock...");
		while (1) {
			if (flock(lock_fd, LOCK_EX) >= 0)
				break;
			if (errno == EAGAIN)
				continue;
			ERROR("Could not take mkswu lock: %m");
			goto out_close;
		}
	}
sanity_checks:
	if (fstat(lock_fd, &statbuf_fd) < 0) {
		// should never happen...
		ERROR("Could not stat mkswu lock (fd): %m");
		goto out_close;
	}
	if (lstat(LOCK_FILE, &statbuf_path) < 0 || statbuf_fd.st_ino != statbuf_path.st_ino) {
		DEBUG("lock file changed, grabbing again");
		close(lock_fd);
		goto again;
	}
	if (access(REBOOT_FILE, F_OK) == 0) {
		INFO("Previous updated marked us for reboot, waiting forever...");
		while (1) {
			sleep(1000);
		}
	}
	// write PID for debugging. This is only informational so we do
	// not care about errors.
	lseek(lock_fd, 0, SEEK_SET);
	ftruncate(lock_fd, 0);
	dprintf(lock_fd, "%d\n", getpid());
	return 0;

out_close:
	close(lock_fd);
	lock_fd = -1;
	return 1;
}

void mkswu_unlock(void)
{
	if (lock_fd < 0)
		return;
	unlink(LOCK_FILE);
	close(lock_fd);
	lock_fd = -1;
}
