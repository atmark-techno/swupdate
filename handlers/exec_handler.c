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
#include "pctl.h"
#include "util.h"

static int exec_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	int ret;
	char *path;

	char *cmd = dict_get_value(&img->properties, "cmd");
	if (!cmd) {
		ERROR("Exec handler needs a command to run: please set the 'cmd' property");
		return -EINVAL;
	}

	if (img->install_directly) {
		struct installer_handler *hnd;

		// we need to extract the file ourselves, abuse rawfile handler
		strlcpy(img->type, "rawfile", sizeof(img->type));
		snprintf(img->path, sizeof(img->path), "%s/%s", get_tmpdir(), img->fname);

		hnd = find_handler(img);
		if (!hnd) {
			ERROR("Could not get rawfile handler?");
			return -EFAULT;
		}
		ret = hnd->installer(img, hnd->data);
		if (ret)
			return ret;
		path = img->path;
	} else {
		path = img->extract_file;
	}
	if (asprintf(&cmd, "%s %s", cmd, path) < 0) {
		ERROR("Could not allocate command string");
		return -1;
	}

	TRACE("Running %s", cmd);
	ret = run_system_cmd(cmd);
	if (ret)
		ERROR("Command failed: %s", cmd);
	free(cmd);

	if (img->install_directly)
		unlink(path);

	TRACE("Finished running command");
	return ret;
}

__attribute__((constructor))
static void exec_handler(void)
{
	register_handler("exec", exec_image,
			 FILE_HANDLER, NULL);
}
