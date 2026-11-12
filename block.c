/*
 * block.c - update of a single status line block
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

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bar.h"
#include "block.h"
#include "json.h"
#include "line.h"
#include "log.h"
#include "sys.h"

const char *block_get(const struct block *block, const char *key)
{
	return map_get(block->env, key);
}

int block_set(struct block *block, const char *key, const char *value)
{
	return map_set(block->env, key, value);
}

int block_reset(struct block *block)
{
	map_clear(block->env);

	return map_copy(block->env, block->config);
}

int block_for_each(const struct block *block,
		   int (*func)(const char *key, const char *value, void *data),
		   void *data)
{
	return map_for_each(block->env, func, data);
}

static bool block_is_spawned(struct block *block)
{
	return block->pid > 0;
}

static int block_setenv(const char *name, const char *value, void *data)
{
	int err;

	(void) data;

	/* setenv(3) rejects empty names and names containing '='. */
	if (!name || !*name || strchr(name, '='))
		return 0;

	if (!value)
		value = "";

	err = sys_setenv(name, value);
	if (err)
		return err;

	/* Legacy env variables */
	if (strcmp(name, "name") == 0)
		return sys_setenv("BLOCK_NAME", value);
	if (strcmp(name, "instance") == 0)
		return sys_setenv("BLOCK_INSTANCE", value);
	if (strcmp(name, "interval") == 0)
		return sys_setenv("BLOCK_INTERVAL", value);
	if (strcmp(name, "button") == 0)
		return sys_setenv("BLOCK_BUTTON", value);
	if (strcmp(name, "x") == 0)
		return sys_setenv("BLOCK_X", value);
	if (strcmp(name, "y") == 0)
		return sys_setenv("BLOCK_Y", value);

	return 0;
}

static int block_child_env(struct block *block)
{
	return block_for_each(block, block_setenv, NULL);
}

static int block_stdout(struct block *block)
{
	const char *label, *full_text;
	int out = block->out[0];
	size_t count;
	int err;

	if (block->interval == INTERVAL_PERSIST)
		count = 1;
	else
		count = SIZE_MAX;

	if (block->format == FORMAT_JSON)
		err = json_read(out, count, block->env);
	else
		err = i3bar_read(out, count, block->env);

	if (err && err != -EAGAIN)
		return err;

	/* Deprecated label */
	label = block_get(block, "label");
	full_text = block_get(block, "full_text");
	if (label && full_text) {
		size_t llen = strlen(label);
		size_t flen = strlen(full_text);
		char *text = malloc(llen + flen + 1);

		if (!text)
			return -ENOMEM;

		memcpy(text, label, llen);
		memcpy(text + llen, full_text, flen + 1);

		err = block_set(block, "full_text", text);
		free(text);
		if (err)
			return err;
	}

	return 0;
}

int block_update(struct block *block)
{
	int err;

	/* Reset properties to default before updating from output */
	err = block_reset(block);
	if (err)
		return err;

	err = block_stdout(block);
	if (err)
		return err;

	/* Exit code takes precedence over the output */
	if (block->code == EXIT_URGENT) {
		err = block_set(block, "urgent", "true");
		if (err)
			return err;
	}

	block_debug(block, "updated successfully");

	return 0;
}

static int block_send_key(const char *key, const char *value, void *data)
{
	struct block *block = data;
	char keybuf[BUFSIZ];
	char valbuf[BUFSIZ];
	int err;

	err = json_escape(key, keybuf, sizeof(keybuf));
	if (err)
		return err;

	if (!json_is_valid(value)) {
		err = json_escape(value, valbuf, sizeof(valbuf));
		if (err)
			return err;

		value = valbuf;
	}

	if (dprintf(block->in[1], ",%s:%s", keybuf, value) < 0)
		return -errno;

	return 0;
}

static int block_send_json(struct block *block)
{
	int err;

	if (dprintf(block->in[1], "{\"\":\"\"") < 0)
		return -errno;

	err = block_for_each(block, block_send_key, block);
	if (err)
		return err;

	if (dprintf(block->in[1], "}\n") < 0)
		return -errno;

	return 0;
}

/* Push data to forked process through the open stdin pipe */
static int block_send(struct block *block)
{
	const char *button = block_get(block, "button");

	if (!button) {
		block_error(block, "no click data to send");
		return -EINVAL;
	}

	if (!block_is_spawned(block)) {
		block_error(block, "persistent block not spawned");
		return 0;
	}

	if (block->format == FORMAT_JSON)
		return block_send_json(block);

	if (dprintf(block->in[1], "%s\n", button) < 0)
		return -errno;

	return 0;
}

int block_click(struct block *block)
{
	block_debug(block, "clicked");

	if (block->interval == INTERVAL_PERSIST)
		return block_send(block);

	return block_spawn(block);
}

void block_touch(struct block *block)
{
	unsigned long now;
	int err;

	err = sys_gettime(&now);
	if (err) {
		block_error(block, "failed to touch block");
		return;
	}

	if (block->timestamp == now) {
		block_debug(block, "looping too fast");
		return;
	}

	block->timestamp = now;
}

static int block_child_sig(struct block *block)
{
	sigset_t set;
	int err;

	(void) block;

	/* The parent ignores SIGPIPE; block commands expect the default. */
	if (signal(SIGPIPE, SIG_DFL) == SIG_ERR)
		return -errno;

	/* It'd be safe to assume that all signals are unblocked by default */
	err = sys_sigfillset(&set);
	if (err)
		return err;

	return sys_sigunblock(&set);
}

static int block_child_stdin(struct block *block)
{
	int err;

	if (block->interval == INTERVAL_PERSIST) {
		err = sys_close(block->in[1]);
		if (err)
			return err;
	} else {
		err = sys_open("/dev/null", &block->in[0]);
		if (err)
			return err;
	}

	err = sys_dup(block->in[0], STDIN_FILENO);
	if (err)
		return err;

	return sys_close(block->in[0]);
}

static int block_child_stdout(struct block *block)
{
	int err;

	err = sys_close(block->out[0]);
	if (err)
		return err;

	err = sys_dup(block->out[1], STDOUT_FILENO);
	if (err)
		return err;

	return sys_close(block->out[1]);
}

static int block_child_exec(struct block *block)
{
	return sys_execsh(block->command);
}

static int block_child(struct block *block)
{
	int err;

	err = block_child_env(block);
	if (err)
		return err;

	err = block_child_sig(block);
	if (err)
		return err;

	err = block_child_stdin(block);
	if (err)
		return err;

	err = block_child_stdout(block);
	if (err)
		return err;

	return block_child_exec(block);
}

static int block_parent_stdin(struct block *block)
{
	/* Close read end of stdin pipe */
	if (block->interval == INTERVAL_PERSIST)
		return sys_close(block->in[0]);

	return 0;
}

static int block_parent_stdout(struct block *block)
{
	int err;

	/* Close write end of stdout pipe */
	err = sys_close(block->out[1]);
	if (err)
		return err;

	if (block->interval == INTERVAL_PERSIST)
		return sys_async(block->out[0], SIGRTMIN);

	return 0;
}

static int block_parent(struct block *block)
{
	int err;

	err = block_parent_stdin(block);
	if (err)
		return err;

	err = block_parent_stdout(block);
	if (err)
		return err;

	block_debug(block, "forked child %d", block->pid);

	return 0;
}

static int block_fork(struct block *block)
{
	int err;

	err = sys_fork(&block->pid);
	if (err)
		return err;

	if (block->pid == 0) {
		err = block_child(block);
		if (err)
			sys_exit(EXIT_ERR_INTERNAL);
	}

	return block_parent(block);
}

static void block_cleanup_pipes(struct block *block)
{
	if (block->in[0] >= 0)
		sys_close(block->in[0]);
	if (block->in[1] >= 0)
		sys_close(block->in[1]);
	if (block->out[0] >= 0) {
		line_forget(block->out[0]);
		sys_close(block->out[0]);
	}
	if (block->out[1] >= 0)
		sys_close(block->out[1]);

	block->in[0] = block->in[1] = -1;
	block->out[0] = block->out[1] = -1;
}

static int block_open(struct block *block)
{
	int err;

	err = sys_pipe(block->out);
	if (err)
		return err;

	if (block->interval == INTERVAL_PERSIST) {
		err = sys_pipe(block->in);
		if (err) {
			block_cleanup_pipes(block);
			return err;
		}
	}

	return 0;
}

int block_spawn(struct block *block)
{
	int err;

	if (!block->command) {
		block_debug(block, "no command, skipping");
		return 0;
	}

	if (block_is_spawned(block)) {
		block_debug(block, "process already spawned");
		return 0;
	}

	err = block_open(block);
	if (err)
		return err;

	err = block_fork(block);
	if (err) {
		block_cleanup_pipes(block);

		if (block->pid > 0) {
			sys_kill(block->pid, SIGTERM);
			sys_waitpid(block->pid, NULL);
			block->pid = 0;
		}
	}

	return err;
}

static int block_wait(struct block *block)
{
	int err;

	if (block->pid <= 0) {
		block_debug(block, "not spawned yet");
		return -EAGAIN;
	}

	err = sys_waitpid(block->pid, &block->code);
	if (err)
		return err;

	block_debug(block, "process %d exited with %d", block->pid, block->code);

	/* Process successfully reaped, reset the block PID */
	block->pid = 0;

	if (block->code == EXIT_ERR_INTERNAL)
		return -ECHILD;

	return 0;
}

void block_close(struct block *block)
{
	int err;

	/* Invalidate descriptors to avoid misdetection after reassignment */
	if (block->interval == INTERVAL_PERSIST && block->in[1] >= 0) {
		err = sys_close(block->in[1]);
		if (err)
			block_error(block, "failed to close stdin");

		block->in[1] = -1;
	}

	if (block->out[0] >= 0) {
		line_forget(block->out[0]);

		err = sys_close(block->out[0]);
		if (err)
			block_error(block, "failed to close stdout");

		block->out[0] = -1;
	}
}

void block_stop(struct block *block)
{
	if (block->pid > 0)
		sys_kill(block->pid, SIGTERM);

	block_close(block);
}

int block_reap(struct block *block)
{
	int err;

	err = block_wait(block);
	if (err) {
		if (err == -EAGAIN)
			return 0;

		block_error(block, "Internal error");
		return err;
	}

	switch (block->code) {
	case 0:
	case EXIT_URGENT:
		break;
	case 126:
		block_error(block, "Command '%s' not executable",
			    block->command);
		break;
	case 127:
		block_error(block, "Command '%s' not found or missing dependency",
			    block->command);
		break;
	default:
		block_error(block, "Command '%s' exited unexpectedly with code %d",
			    block->command, block->code);
		break;
	}

	return 0;
}

static int block_parse_int(const char *value, int min, int max, int *result)
{
	char *end;
	long number;

	errno = 0;
	number = strtol(value, &end, 10);
	if (errno || end == value || *end != '\0' ||
	    number < min || number > max)
		return -EINVAL;

	*result = (int) number;

	return 0;
}

static int i3blocks_setup(struct block *block)
{
	const char *value;
	int err;

	value = map_get(block->config, "command");
	if (value && *value != '\0')
		block->command = value;

	value = map_get(block->config, "interval");
	if (!value) {
		block->interval = 0;
	} else if (strcmp(value, "once") == 0) {
		block->interval = INTERVAL_ONCE;
	} else if (strcmp(value, "repeat") == 0) {
		block->interval = INTERVAL_REPEAT;
	} else if (strcmp(value, "persist") == 0) {
		block->interval = INTERVAL_PERSIST;
	} else {
		err = block_parse_int(value, INTERVAL_PERSIST, INT_MAX,
				      &block->interval);
		if (err) {
			block_error(block, "invalid interval '%s', using 0",
				    value);
			block->interval = 0;
		}
	}

	value = map_get(block->config, "format");
	if (value && strcmp(value, "json") == 0)
		block->format = FORMAT_JSON;
	else
		block->format = FORMAT_RAW;

	value = map_get(block->config, "signal");
	if (!value) {
		block->signal = 0;
	} else {
		err = block_parse_int(value, 0, SIGRTMAX - SIGRTMIN,
				      &block->signal);
		if (err) {
			block_error(block, "invalid signal '%s', ignoring",
				    value);
			block->signal = 0;
		}
	}

	return 0;
}

int block_setup(struct block *block)
{
	int err;

	err = i3bar_setup(block);
	if (err)
		return err;

	err = i3blocks_setup(block);
	if (err)
		return err;

	err = block_reset(block);
	if (err)
		return err;

	block_debug(block, "new block");

	return 0;
}

void block_destroy(struct block *block)
{
	if (!block)
		return;

	block_close(block);

	if (block->config)
		map_destroy(block->config);
	if (block->env)
		map_destroy(block->env);
	if (block->name)
		free(block->name);
	free(block);
}

struct block *block_create(struct bar *bar, const struct map *config)
{
	struct block *block;
	int err;

	block = calloc(1, sizeof(struct block));
	if (!block)
		return NULL;

	block->bar = bar;
	block->in[0] = block->in[1] = -1;
	block->out[0] = block->out[1] = -1;

	block->config = map_create();
	if (!block->config) {
		block_destroy(block);
		return NULL;
	}

	if (config) {
		err = map_copy(block->config, config);
		if (err) {
			block_destroy(block);
			return NULL;
		}
	}

	block->env = map_create();
	if (!block->env) {
		block_destroy(block);
		return NULL;
	}

	return block;
}

void block_printf(struct block *block, int lvl, const char *fmt, ...)
{
	char buf[BUFSIZ];
	va_list ap;
	int err;

	if (lvl > (int) log_level)
		return;

	/* The bar may have no block yet, e.g. while reporting a config error. */
	if (!block)
		return;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	err = i3bar_printf(block, lvl, buf);
	if (err)
		fatal("failed to format message for block %s: %s",
		      block->name ? block->name : "(unnamed)", buf);
}
