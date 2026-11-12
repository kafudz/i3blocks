/*
 * test_unit.c - unit tests for map, JSON, and line helpers
 *
 * These tests are built by `make check`.  They exercise the invariants that
 * are easy to break during refactoring: map aliasing, JSON escaping and
 * Unicode decoding, and retention of partial non-blocking lines.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../json.h"
#include "../line.h"
#include "../log.h"
#include "../map.h"

enum log_level log_level;

static int failures;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

static void test_map(void)
{
	struct map *map = map_create();
	struct map *copy = map_create();
	const char *value;

	CHECK(map != NULL);
	CHECK(copy != NULL);

	CHECK(map_get(map, "missing") == NULL);
	CHECK(map_set(map, "key", "value") == 0);
	CHECK(strcmp(map_get(map, "key"), "value") == 0);
	CHECK(map_set(map, "key", "new") == 0);
	CHECK(strcmp(map_get(map, "key"), "new") == 0);

	/* Setting a key to its own value must not free it before copying. */
	value = map_get(map, "key");
	CHECK(map_set(map, "key", value) == 0);
	CHECK(strcmp(map_get(map, "key"), "new") == 0);

	CHECK(map_copy(copy, map) == 0);
	CHECK(strcmp(map_get(copy, "key"), "new") == 0);

	map_clear(map);
	CHECK(map_get(map, "key") == NULL);
	CHECK(map_set(map, "again", "ok") == 0);
	CHECK(strcmp(map_get(map, "again"), "ok") == 0);

	map_destroy(copy);
	map_destroy(map);
}

static void test_json(void)
{
	char buf[64];
	struct map *map;
	int fds[2];
	const char *input;

	CHECK(json_is_string("\"hello\""));
	CHECK(json_is_string("\"\""));
	CHECK(!json_is_string("hello"));
	CHECK(!json_is_string("\"unterminated"));

	CHECK(json_is_valid("null"));
	CHECK(json_is_valid("true"));
	CHECK(json_is_valid("-12.5e+2"));
	CHECK(json_is_valid("{\"a\":[1,2,{\"b\":\"}\"}]}"));
	CHECK(!json_is_valid("+1"));
	CHECK(!json_is_valid("01"));
	CHECK(!json_is_valid("{\"a\":1}garbage"));

	CHECK(json_escape("plain", buf, sizeof(buf)) == 0);
	CHECK(strcmp(buf, "\"plain\"") == 0);
	CHECK(json_escape("a\"b\n", buf, sizeof(buf)) == 0);
	CHECK(strcmp(buf, "\"a\\\"b\\n\"") == 0);
	CHECK(json_escape("", buf, sizeof(buf)) == 0);
	CHECK(strcmp(buf, "\"\"") == 0);
	CHECK(json_escape("x", buf, 2) == -ENOSPC);

	CHECK(json_is_valid("\"\\u00e9\""));
	CHECK(json_is_valid("\"\\ud83d\\ude00\""));
	CHECK(!json_is_valid("\"\\ud800\""));
	CHECK(!json_is_valid("\"\\u0000\""));

	/* Parse escaped Unicode through the line-oriented reader. */
	CHECK(pipe(fds) == 0);
	CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
	input = "{\"full_text\":\"caf\\u00e9 \\ud83d\\ude00\"}\n";
	CHECK(write(fds[1], input, strlen(input)) == (ssize_t) strlen(input));

	map = map_create();
	CHECK(map != NULL);
	CHECK(json_read(fds[0], 1, map) == 0);
	CHECK(strcmp(map_get(map, "full_text"),
		     "caf\xc3\xa9 \xf0\x9f\x98\x80") == 0);

	map_destroy(map);
	line_forget(fds[0]);
	close(fds[0]);
	close(fds[1]);
}

struct line_result {
	char text[64];
	size_t count;
};

static int line_cb(char *line, size_t num, void *data)
{
	struct line_result *result = data;

	(void) num;

	snprintf(result->text, sizeof(result->text), "%s", line);
	result->count++;

	return 0;
}

static void test_line_buffering(void)
{
	struct line_result result = { .count = 0 };
	int fds[2];
	int err;

	CHECK(pipe(fds) == 0);
	CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);

	CHECK(write(fds[1], "par", 3) == 3);
	err = line_read(fds[0], 1, line_cb, &result);
	CHECK(err == -EAGAIN);
	CHECK(result.count == 0);

	CHECK(write(fds[1], "tial\n", 5) == 5);
	CHECK(line_read(fds[0], 1, line_cb, &result) == 0);
	CHECK(result.count == 1);
	CHECK(strcmp(result.text, "partial") == 0);

	line_forget(fds[0]);
	close(fds[0]);
	close(fds[1]);
}

int main(void)
{
	test_map();
	test_json();
	test_line_buffering();

	if (failures) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}

	printf("all unit tests passed\n");

	return 0;
}
