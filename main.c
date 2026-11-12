/*
 * main.c - main entry point
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

#ifdef HAVE_CONFIG_H
#include "i3blocks-config.h"
#endif

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bar.h"
#include "log.h"
#include "sys.h"

enum log_level log_level;

int main(int argc, char *argv[])
{
	char *output = NULL;
	char *path = NULL;
	bool term;
	int c;

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		error("failed to ignore SIGPIPE");
		return EXIT_FAILURE;
	}

	while ((c = getopt(argc, argv, "c:o:vhV")) != -1) {
		switch (c) {
		case 'c':
			path = optarg;
			break;
		case 'o':
			output = optarg;
			break;
		case 'v':
			log_level++;
			break;
		case 'h':
			printf("Usage: %s [-c <configfile>] [-o <output>] [-v] [-h] [-V]\n", argv[0]);
			return EXIT_SUCCESS;
		case 'V':
			printf(PACKAGE_STRING " © 2014-2019 Vivien Didelot and contributors\n");
			return EXIT_SUCCESS;
		default:
			error("Try '%s -h' for more information.", argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (output && strcmp(output, "term") != 0) {
		error("unknown output '%s', expected 'term'", output);
		return EXIT_FAILURE;
	}

	term = sys_isatty(STDOUT_FILENO);
	if (output)
		term = true;

	if (bar_init(term, path))
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}
