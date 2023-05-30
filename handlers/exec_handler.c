/*
 * (C) Copyright 2021
 * Dominique Martinet, Atmark Techno, dominique.martinet@atmark-techno.com
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "swupdate.h"
#include "handler.h"
#include "pctl.h"
#include "util.h"

static int move_to_original_name(struct img_type *img, char *fname) {
	char *last_slash;
	size_t str_size;

	memcpy(img->path, img->extract_file, sizeof(img->path));
	last_slash = strrchr(img->path, '/');
	if (!last_slash)
		return -EINVAL;
	last_slash++;
	str_size = sizeof(img->path) - (last_slash - img->extract_file) - 1;
	if (snprintf(last_slash, str_size, "%s", fname) > str_size)
		return -ERANGE;

	/* Note this rename has the potential of overwriting a file
	 * that would be used later; we don't actively support the
	 * not-installed-directly pattern so let such users deal with that
	 * (also, yes, fname can be ../../../etc/shadow, but we trust the
	 * image anyway)
	 */
	if (rename(img->extract_file, img->path) != 0)
		return -errno;

	// update extract_file/fname for cleanup/other users
	memcpy(img->extract_file, img->path, sizeof(img->extract_file));
	strncpy(img->fname, fname, sizeof(img->fname));
	return 0;
}

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
	// original filename to use if possible
	char *fname = dict_get_value(&img->properties, "filename");

	if (img->install_directly) {
		struct installer_handler *hnd;

		// we need to extract the file ourselves, abuse rawfile handler
		strlcpy(img->type, "rawfile", sizeof(img->type));
		snprintf(img->path, sizeof(img->path), "%s%s", get_tmpdir(),
			 fname ?:img->fname);

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
		if (fname && strcmp(img->fname, fname)) {
			if (move_to_original_name(img, fname))
				WARN("Could not preserve original file name, keeping current one");
		}
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
