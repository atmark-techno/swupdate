// SPDX-License-Identifier:     GPL-2.0-only

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

#include "installer.h"
#include "semver.h"
#include "pctl.h"

static bool running_vendored = false;
#define VENDORED_SCRIPTS "/usr/libexec/mkswu/"
#define SKIP_SCRIPTS_MARKER "# DEBUG_SKIP_SCRIPTS\n"
static char *EMBEDDED_CLEANUP_SCRIPT;

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
