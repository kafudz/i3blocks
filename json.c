/*
 * json.c - flat JSON parsing
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
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "line.h"
#include "log.h"
#include "map.h"
#include "sys.h"

static int json_hex(const unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

/* Parse exactly four hexadecimal digits */
static bool json_parse_hex4(const char *str, uint32_t *codepoint)
{
	uint32_t value = 0;
	int hex;
	int i;

	for (i = 0; i < 4; i++) {
		hex = json_hex((unsigned char) str[i]);
		if (hex < 0)
			return false;

		value = (value << 4) | (uint32_t) hex;
	}

	*codepoint = value;

	return true;
}

/* Return the number of UTF-8 bytes on success, 0 if it is invalid */
static size_t json_encode_utf8(uint32_t codepoint, char *buf, size_t size)
{
	char utf8[4];
	size_t len;

	if (codepoint == 0 || codepoint > 0x10ffff ||
	    (codepoint >= 0xd800 && codepoint <= 0xdfff))
		return 0;

	if (codepoint <= 0x7f) {
		len = 1;
		utf8[0] = (char) codepoint;
	} else if (codepoint <= 0x7ff) {
		len = 2;
		utf8[0] = (char) (0xc0 | (codepoint >> 6));
		utf8[1] = (char) (0x80 | (codepoint & 0x3f));
	} else if (codepoint <= 0xffff) {
		len = 3;
		utf8[0] = (char) (0xe0 | (codepoint >> 12));
		utf8[1] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
		utf8[2] = (char) (0x80 | (codepoint & 0x3f));
	} else {
		len = 4;
		utf8[0] = (char) (0xf0 | (codepoint >> 18));
		utf8[1] = (char) (0x80 | ((codepoint >> 12) & 0x3f));
		utf8[2] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
		utf8[3] = (char) (0x80 | (codepoint & 0x3f));
	}

	if (buf) {
		if (size < len)
			return 0;

		memcpy(buf, utf8, len);
	}

	return len;
}

/* Return the length of the parsed string, 0 if it is invalid */
static size_t json_parse_string(const char *str, char *buf, size_t size)
{
	const char *p = str;
	size_t out = 0;

	if (*p != '"')
		return 0;

	p++;

	while (*p != '"') {
		char value[4];
		size_t len;

		if (*p == '\0')
			return 0;

		if (*p == '\\') {
			p++;

			switch (*p) {
			case '"':
			case '\\':
			case '/':
				value[0] = *p;
				len = 1;
				break;
			case 'b':
				value[0] = '\b';
				len = 1;
				break;
			case 'f':
				value[0] = '\f';
				len = 1;
				break;
			case 'n':
				value[0] = '\n';
				len = 1;
				break;
			case 'r':
				value[0] = '\r';
				len = 1;
				break;
			case 't':
				value[0] = '\t';
				len = 1;
				break;
			case 'u': {
				uint32_t codepoint;

				if (!json_parse_hex4(p + 1, &codepoint))
					return 0;

				p += 4; /* last hexadecimal digit */

				/* Combine UTF-16 surrogate pairs. */
				if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
					uint32_t low;

					if (p[1] != '\\' || p[2] != 'u' ||
					    !json_parse_hex4(p + 3, &low) ||
					    low < 0xdc00 || low > 0xdfff)
						return 0;

					p += 6;
					codepoint = 0x10000 +
						((codepoint - 0xd800) << 10) +
						(low - 0xdc00);
				} else if (codepoint >= 0xdc00 &&
					   codepoint <= 0xdfff) {
					return 0;
				}

				len = json_encode_utf8(codepoint, value,
						       sizeof(value));
				if (!len)
					return 0;
				break;
			}
			default:
				return 0;
			}
		} else {
			/* Unescaped control characters are invalid. */
			if ((unsigned char) *p < 0x20)
				return 0;

			value[0] = *p;
			len = 1;
		}

		if (buf) {
			/* Keep room for the terminating null byte. */
			if (out + len + 1 > size)
				return 0;

			memcpy(buf + out, value, len);
		}

		out += len;
		p++;
	}

	if (buf) {
		if (out + 1 > size)
			return 0;

		buf[out] = '\0';
	}

	return (size_t)(p - str) + 1;
}

/* Return the length of the parsed non scalar (with open/close delimiter) */
static size_t json_parse_nested_struct(const char *str, char open, char close,
				       char *buf, size_t size)
{
	const char *p = str;
	size_t len;
	int nested;
	bool string;
	bool escape;

	if (*p != open)
		return 0;

	nested = 1;
	string = false;
	escape = false;

	while (nested) {
		p++;

		/* Control characters are not allowed outside strings. */
		if (*p == '\0')
			return 0;
		if ((unsigned char) *p < 0x20 && *p != '\t' &&
		    *p != '\n' && *p != '\r')
			return 0;

		if (string) {
			if (escape)
				escape = false;
			else if (*p == '\\')
				escape = true;
			else if (*p == '"')
				string = false;
			continue;
		}

		if (*p == '"') {
			string = true;
		} else if (*p == open) {
			nested++;
		} else if (*p == close) {
			nested--;
		}
	}

	len = (size_t)(p - str) + 1;

	if (buf) {
		if (size <= len)
			return 0;

		memcpy(buf, str, len);
		buf[len] = '\0';
	}

	return len;
}

/* Return the length of the parsed array, 0 if it is invalid */
static size_t json_parse_nested_array(const char *str, char *buf, size_t size)
{
	return json_parse_nested_struct(str, '[', ']', buf, size);
}

/* Return the length of the parsed object, 0 if it is invalid */
static size_t json_parse_nested_object(const char *str, char *buf, size_t size)
{
	return json_parse_nested_struct(str, '{', '}', buf, size);
}

/* Return the length of the parsed number, 0 if it is invalid */
static size_t json_parse_number(const char *str, char *buf, size_t size)
{
	const char *p = str;
	size_t len;

	if (*p == '-')
		p++;

	if (*p == '0') {
		p++;
	} else if (*p >= '1' && *p <= '9') {
		do {
			p++;
		} while (isdigit((unsigned char) *p));
	} else {
		return 0;
	}

	if (*p == '.') {
		p++;
		if (!isdigit((unsigned char) *p))
			return 0;
		do {
			p++;
		} while (isdigit((unsigned char) *p));
	}

	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '+' || *p == '-')
			p++;
		if (!isdigit((unsigned char) *p))
			return 0;
		do {
			p++;
		} while (isdigit((unsigned char) *p));
	}

	len = (size_t)(p - str);

	if (buf) {
		if (size <= len)
			return 0;

		memcpy(buf, str, len);
		buf[len] = '\0';
	}

	return len;
}

/* Return the length of the parsed literal, 0 if it is invalid */
static size_t json_parse_literal(const char *str, const char *literal,
				 char *buf, size_t size)
{
	const size_t len = strlen(literal);

	if (strncmp(str, literal, len) != 0)
		return 0;

	if (buf) {
		if (size <= len)
			return 0;

		memcpy(buf, literal, len + 1);
	}

	return len;
}

/* A value can be a string, number, object, array, true, false, or null */
static size_t json_parse_value(const char *str, char *buf, size_t size)
{
	size_t len;

	len = json_parse_string(str, buf, size);
	if (len)
		return len;

	len = json_parse_number(str, buf, size);
	if (len)
		return len;

	len = json_parse_nested_object(str, buf, size);
	if (len)
		return len;

	len = json_parse_nested_array(str, buf, size);
	if (len)
		return len;

	len = json_parse_literal(str, "true", buf, size);
	if (len)
		return len;

	len = json_parse_literal(str, "false", buf, size);
	if (len)
		return len;

	len = json_parse_literal(str, "null", buf, size);
	if (len)
		return len;

	return 0;
}

/* Return the length of a separator optionally enclosed by whitespaces, 0 otherwise */
static size_t json_parse_sep(const char *str, char sep)
{
	size_t len = 0;

	while (isspace((unsigned char) *str))
		str++, len++;

	if (*str != sep)
		return 0;

	str++;
	len++;

	while (isspace((unsigned char) *str))
		str++, len++;

	return len;
}

/* Parse an inline ["name"][\s+:\s+][value] name-value pair */
static size_t json_parse_pair(const char *str, char *name, size_t namesiz,
			      char *val, size_t valsiz)
{
	size_t pair_len = 0;
	size_t len;

	len = json_parse_string(str, name, namesiz);
	if (!len)
		return 0;

	pair_len += len;
	str += len;

	len = json_parse_sep(str, ':');
	if (!len)
		return 0;

	pair_len += len;
	str += len;

	len = json_parse_value(str, val, valsiz);
	if (!len)
		return 0;

	pair_len += len;

	return pair_len;
}

static int json_line_cb(char *line, size_t num, void *data)
{
	struct map *map = data;
	char name[BUFSIZ];
	char val[BUFSIZ];
	size_t len;
	int err;

	(void) num;

	for (;;) {
		/* Only support inline flattened structures at the moment */
		while (*line == '[' || *line == ']' || *line == ',' ||
		       *line == '{' || *line == '}' ||
		       isspace((unsigned char) *line))
			line++;

		if (*line == '\0')
			break;

		memset(name, 0, sizeof(name));
		memset(val, 0, sizeof(val));

		len = json_parse_pair(line, name, sizeof(name), val,
				      sizeof(val));
		if (!len)
			return -EINVAL;

		line += len;

		/* valid delimiters after a pair */
		if (*line != ',' && *line != '}' && *line != '\0' &&
		    !isspace((unsigned char) *line))
			return -EINVAL;

		if (*line != '\0')
			line++;

		if (map) {
			err = map_set(map, name, val);
			if (err)
				return err;
		}
	}

	return 0;
}

int json_read(int fd, size_t count, struct map *map)
{
	return line_read(fd, count, json_line_cb, map);
}

bool json_is_string(const char *str)
{
	size_t len;

	len = strlen(str);
	if (!len)
		return false;

	return json_parse_string(str, NULL, 0) == len;
}

bool json_is_valid(const char *str)
{
	size_t len;

	len = strlen(str);
	if (!len)
		return false;

	return json_parse_value(str, NULL, 0) == len;
}

static int json_append(char *buf, size_t size, size_t *len,
		       const char *str, size_t count)
{
	if (*len + count >= size)
		return -ENOSPC;

	memcpy(buf + *len, str, count);
	*len += count;

	return 0;
}

int json_escape(const char *str, char *buf, size_t size)
{
	size_t len = 0;
	int err;

	if (!size)
		return -ENOSPC;

	err = json_append(buf, size, &len, "\"", 1);
	if (err)
		return err;

	for (;;) {
		unsigned char c = (unsigned char) *str;

		if (c == '\0')
			break;

		switch (c) {
		case '"':
			err = json_append(buf, size, &len, "\\\"", 2);
			break;
		case '\\':
			err = json_append(buf, size, &len, "\\\\", 2);
			break;
		case '\b':
			err = json_append(buf, size, &len, "\\b", 2);
			break;
		case '\f':
			err = json_append(buf, size, &len, "\\f", 2);
			break;
		case '\n':
			err = json_append(buf, size, &len, "\\n", 2);
			break;
		case '\r':
			err = json_append(buf, size, &len, "\\r", 2);
			break;
		case '\t':
			err = json_append(buf, size, &len, "\\t", 2);
			break;
		default:
			if (c < 0x20) {
				char escaped[7];

				snprintf(escaped, sizeof(escaped), "\\u%04x",
					 c);
				err = json_append(buf, size, &len, escaped, 6);
			} else {
				err = json_append(buf, size, &len,
						  (const char *) str, 1);
			}
			break;
		}

		if (err)
			return err;

		str++;
	}

	err = json_append(buf, size, &len, "\"", 1);
	if (err)
		return err;

	buf[len] = '\0';

	return 0;
}
