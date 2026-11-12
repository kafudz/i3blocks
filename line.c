/*
 * line.c - generic line parser
 * Copyright (C) 2015-2019  Vivien Didelot
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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "line.h"
#include "log.h"
#include "sys.h"

/*
 * Lines are buffered per file descriptor.  This avoids one read(2) syscall
 * per character and, more importantly, preserves partial lines across
 * non-blocking reads instead of silently dropping them on EAGAIN.
 */
struct line {
	int fd;
	struct line *next;

	size_t start;
	size_t end;
	char buf[BUFSIZ];
};

static struct line *line_list;

static struct line **line_slot(int fd)
{
	struct line **link = &line_list;

	while (*link && (*link)->fd != fd)
		link = &(*link)->next;

	return link;
}

static struct line *line_find(int fd, bool create)
{
	struct line **link = line_slot(fd);
	struct line *line = *link;

	if (line || !create)
		return line;

	line = calloc(1, sizeof(*line));
	if (!line)
		return NULL;

	line->fd = fd;
	*link = line;

	return line;
}

void line_forget(int fd)
{
	struct line **link = line_slot(fd);
	struct line *line = *link;

	if (!line)
		return;

	*link = line->next;
	free(line);
}

static void line_compact(struct line *line)
{
	if (line->start == 0)
		return;

	if (line->start == line->end) {
		line->start = 0;
		line->end = 0;
		return;
	}

	memmove(line->buf, line->buf + line->start, line->end - line->start);
	line->end -= line->start;
	line->start = 0;
}

static int line_refill(struct line *line)
{
	size_t count;
	int err;

	line_compact(line);

	if (line->end == sizeof(line->buf))
		return -ENOSPC;

	err = sys_read(line->fd, line->buf + line->end,
		       sizeof(line->buf) - line->end, &count);
	if (err)
		return err;

	line->end += count;

	return 0;
}

/* Read a line including the newline character and return its positive length */
static ssize_t line_gets(struct line *line, char *buf, size_t size)
{
	for (;;) {
		size_t avail = line->end - line->start;
		char *newline;

		newline = avail ? memchr(line->buf + line->start, '\n', avail)
				: NULL;
		if (newline) {
			size_t len = (size_t)(newline -
					      (line->buf + line->start)) + 1;

			if (len > size)
				return -ENOSPC;

			memcpy(buf, line->buf + line->start, len);
			line->start += len;
			if (line->start == line->end)
				line->start = line->end = 0;

			return (ssize_t)len;
		}

		{
			int err = line_refill(line);

			if (err) {
				/* Drop an overlong unterminated line so that
				 * the next read can resynchronize. */
				if (err == -ENOSPC)
					line->start = line->end = 0;

				return err;
			}
		}
	}
}

/* Read a line excluding the newline character */
static int line_parse(struct line *line, line_cb_t *cb, size_t num,
		      void *data)
{
	char buf[BUFSIZ];
	ssize_t len;
	int err;

	len = line_gets(line, buf, sizeof(buf));
	if (len < 0)
		return (int)len;

	/* replace newline with terminating null byte */
	buf[len - 1] = '\0';

	debug("&%d:%.3zu: %s", line->fd, num, buf);

	if (cb) {
		err = cb(buf, num, data);
		if (err)
			return err;
	}

	return 0;
}

/* Read up to count lines excluding their newline character */
int line_read(int fd, size_t count, line_cb_t *cb, void *data)
{
	struct line *line;
	size_t lines = 0;
	int err;

	line = line_find(fd, true);
	if (!line)
		return -ENOMEM;

	while (count--) {
		err = line_parse(line, cb, lines, data);
		if (err)
			return err;

		lines++;
	}

	return 0;
}

/* Return true if a complete line is buffered or can be read without blocking */
bool line_pending(int fd)
{
	struct line *line = line_find(fd, true);
	int err;

	if (!line)
		return false;

	if (memchr(line->buf + line->start, '\n', line->end - line->start))
		return true;

	err = line_refill(line);
	if (err) {
		/* Discard an overlong unterminated line as line_gets() does,
		 * otherwise the full buffer would never be drained. */
		if (err == -ENOSPC)
			line->start = line->end = 0;

		return false;
	}

	return memchr(line->buf + line->start, '\n',
		      line->end - line->start) != NULL;
}
