/*
 * config.c - parsing of the configuration file
 * Copyright (C) 2014-2019  Vivien Didelot
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "ini.h"
#include "line.h"
#include "log.h"
#include "map.h"
#include "sys.h"

#ifndef SYSCONFDIR
#define SYSCONFDIR "/etc"
#endif

struct config {
	struct map *section;
	struct map *global;
	config_cb_t *cb;
	void *data;
};

static int config_finalize(struct config *conf)
{
	int err = 0;

	if (!conf->section)
		return 0;

	if (conf->cb)
		err = conf->cb(conf->section, conf->data);

	/* The callback is expected to copy what it needs. */
	map_destroy(conf->section);
	conf->section = NULL;

	return err;
}

static void config_free(struct config *conf)
{
	if (conf->section)
		map_destroy(conf->section);
	if (conf->global)
		map_destroy(conf->global);

	conf->section = NULL;
	conf->global = NULL;
}

static int config_reset(struct config *conf)
{
	int err;

	conf->section = map_create();
	if (!conf->section)
		return -ENOMEM;

	if (conf->global) {
		err = map_copy(conf->section, conf->global);
		if (err) {
			map_destroy(conf->section);
			conf->section = NULL;
			return err;
		}
	}

	return 0;
}

static int config_set(struct config *conf, const char *key, const char *value)
{
	struct map *map = conf->section;

	if (!map) {
		if (!conf->global) {
			conf->global = map_create();
			if (!conf->global)
				return -ENOMEM;
		}

		map = conf->global;
	}

	return map_set(map, key, value);
}

static int config_ini_section_cb(char *section, void *data)
{
	int err;

	err = config_finalize(data);
	if (err)
		return err;

	err = config_reset(data);
	if (err)
		return err;

	return config_set(data, "name", section);
}

static int config_ini_property_cb(char *key, char *value, void *data)
{
	return config_set(data, key, value);
}

static int config_read(struct config *conf, int fd)
{
	int err;

	err = ini_read(fd, -1, config_ini_section_cb, config_ini_property_cb,
		       conf);
	if (err && err != -EAGAIN)
		return err;

	return config_finalize(conf);
}

/* Format "dir/name" and reject silent truncation. */
static int config_path(char *buf, size_t size, const char *dir,
		       const char *name)
{
	int len;

	len = snprintf(buf, size, "%s/%s", dir, name);
	if (len < 0 || (size_t) len >= size)
		return -ENAMETOOLONG;

	return 0;
}

static int config_open(struct config *conf, const char *path)
{
	char pname[PATH_MAX];
	char *dname;
	int err;
	int fd;

	debug("try file %s", path);

	if (strlen(path) >= sizeof(pname)) {
		error("configuration path too long: %s", path);
		return -ENAMETOOLONG;
	}
	strcpy(pname, path);

	err = sys_open(path, &fd);
	if (err)
		return err;

	dname = dirname(pname);
	err = sys_chdir(dname);
	if (err) {
		error("failed to change directory to %s", dname);
		sys_close(fd);
		return err;
	}

	debug("changed directory to %s", dname);

	err = config_read(conf, fd);
	line_forget(fd);
	sys_close(fd);

	if (conf->global) {
		map_destroy(conf->global);
		conf->global = NULL;
	}

	if (err)
		config_free(conf);

	return err;
}

/* Try a default configuration path; ENOENT means "keep looking". */
static int config_try(struct config *conf, const char *dir, const char *name)
{
	char buf[PATH_MAX];
	int err;

	err = config_path(buf, sizeof(buf), dir, name);
	if (err)
		return err;

	return config_open(conf, buf);
}

int config_load(const char *path, config_cb_t *cb, void *data)
{
	const char * const home = sys_getenv("HOME");
	const char * const xdg_home = sys_getenv("XDG_CONFIG_HOME");
	const char * const xdg_dirs = sys_getenv("XDG_CONFIG_DIRS");
	struct config conf = {
		.data = data,
		.cb = cb,
	};
	char buf[PATH_MAX];
	int err;

	/* command line config file? */
	if (path) {
		err = config_open(&conf, path);
		if (err)
			config_free(&conf);
		return err;
	}

	/* user config file? */
	if (home && *home) {
		if (xdg_home && *xdg_home) {
			err = config_try(&conf, xdg_home, "i3blocks/config");
		} else {
			err = config_try(&conf, home,
					 ".config/i3blocks/config");
		}
		if (err != -ENOENT)
			return err;

		err = config_try(&conf, home, ".i3blocks.conf");
		if (err != -ENOENT)
			return err;
	}

	/* system config file? */
	if (xdg_dirs && *xdg_dirs) {
		const char *dir = xdg_dirs;

		while (*dir) {
			const char *end = strchr(dir, ':');
			size_t len = end ? (size_t)(end - dir) : strlen(dir);

			if (len) {
				if (len >= sizeof(buf)) {
					err = -ENAMETOOLONG;
				} else {
					memcpy(buf, dir, len);
					buf[len] = '\0';
					err = config_try(&conf, buf,
							 "i3blocks/config");
				}

				if (err != -ENOENT)
					return err;
			}

			if (!end)
				break;
			dir = end + 1;
		}
	} else {
		err = config_try(&conf, SYSCONFDIR "/xdg",
				 "i3blocks/config");
		if (err != -ENOENT)
			return err;
	}

	return config_try(&conf, SYSCONFDIR, "i3blocks.conf");
}
