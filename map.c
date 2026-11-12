/*
 * map.c - implementation of an associative array
 * Copyright (C) 2017-2019  Vivien Didelot
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
#include <stdlib.h>
#include <string.h>

#include "map.h"

#define MAP_BUCKETS	16
#define MAP_LOAD_NUM	3
#define MAP_LOAD_DEN	4

struct pair {
	char *key;
	char *value;

	/* Insertion order, used to keep map_for_each() deterministic. */
	struct pair *next;

	/* Hash bucket chain, used for lookups. */
	struct pair *hash_next;
};

struct map {
	struct pair *head;	/* sentinel node */
	struct pair *tail;
	struct pair **buckets;
	size_t nbuckets;
	size_t count;
};

static size_t map_hash(const char *key)
{
	const unsigned char *p = (const unsigned char *)key;
	size_t hash = 5381;

	while (*p)
		hash = ((hash << 5) + hash) ^ *p++;

	return hash;
}

static struct pair *map_lookup(const struct map *map, const char *key)
{
	struct pair *pair;
	size_t bucket;

	if (!key || !map->nbuckets)
		return NULL;

	bucket = map_hash(key) % map->nbuckets;
	for (pair = map->buckets[bucket]; pair; pair = pair->hash_next)
		if (strcmp(pair->key, key) == 0)
			return pair;

	return NULL;
}

/* Update the value of a pair.  Duplicate first so that value may alias
 * the current pair value without being freed before it is copied. */
static int map_reassign(struct pair *pair, const char *value)
{
	char *copy = NULL;

	if (value) {
		copy = strdup(value);
		if (!copy)
			return -ENOMEM;
	}

	free(pair->value);
	pair->value = copy;

	return 0;
}

/* Create a new key-value pair */
static struct pair *map_pair(const char *key, const char *value)
{
	struct pair *pair;

	if (!key)
		return NULL;

	pair = calloc(1, sizeof(struct pair));
	if (!pair)
		return NULL;

	pair->key = strdup(key);
	if (!pair->key) {
		free(pair);
		return NULL;
	}

	if (map_reassign(pair, value)) {
		free(pair->key);
		free(pair);
		return NULL;
	}

	return pair;
}

/* Destroy a key-value pair */
static void map_unpair(struct pair *pair)
{
	map_reassign(pair, NULL);
	free(pair->key);
	free(pair);
}

static void map_hash_insert(struct map *map, struct pair *pair)
{
	size_t bucket = map_hash(pair->key) % map->nbuckets;

	pair->hash_next = map->buckets[bucket];
	map->buckets[bucket] = pair;
}

static int map_resize(struct map *map, size_t nbuckets)
{
	struct pair **buckets;
	struct pair **old;
	struct pair *pair;
	size_t i;

	buckets = calloc(nbuckets, sizeof(*buckets));
	if (!buckets)
		return -ENOMEM;

	old = map->buckets;
	map->buckets = buckets;
	map->nbuckets = nbuckets;

	for (pair = map->head->next; pair; pair = pair->next) {
		i = map_hash(pair->key) % nbuckets;
		pair->hash_next = buckets[i];
		buckets[i] = pair;
	}

	free(old);

	return 0;
}

/* Append a key-value pair to the end of the insertion order list */
static int map_insert(struct map *map, const char *key, const char *value)
{
	struct pair *pair;

	pair = map_pair(key, value);
	if (!pair)
		return -ENOMEM;

	/* Grow lazily; a failed resize is harmless, only slower. */
	if (map->count + 1 > map->nbuckets * MAP_LOAD_NUM / MAP_LOAD_DEN)
		map_resize(map, map->nbuckets * 2);

	pair->next = NULL;
	map->tail->next = pair;
	map->tail = pair;
	map_hash_insert(map, pair);
	map->count++;

	return 0;
}

const char *map_get(const struct map *map, const char *key)
{
	struct pair *pair = map_lookup(map, key);

	return pair ? pair->value : NULL;
}

int map_set(struct map *map, const char *key, const char *value)
{
	struct pair *pair = map_lookup(map, key);

	if (pair)
		return map_reassign(pair, value);

	return map_insert(map, key, value);
}

int map_for_each(const struct map *map, map_func_t *func, void *data)
{
	struct pair *pair;
	int err;

	for (pair = map->head->next; pair; pair = pair->next) {
		err = func(pair->key, pair->value, data);
		if (err)
			return err;
	}

	return 0;
}

void map_clear(struct map *map)
{
	struct pair *pair = map->head->next;

	while (pair) {
		struct pair *next = pair->next;

		map_unpair(pair);
		pair = next;
	}

	map->head->next = NULL;
	map->tail = map->head;
	map->count = 0;

	if (map->buckets)
		memset(map->buckets, 0,
		       map->nbuckets * sizeof(*map->buckets));
}

static int map_dup(const char *key, const char *value, void *data)
{
	struct map *map = data;

	return map_set(map, key, value);
}

int map_copy(struct map *map, const struct map *base)
{
	return map_for_each(base, map_dup, map);
}

void map_destroy(struct map *map)
{
	if (!map)
		return;

	map_clear(map);
	free(map->head);
	free(map->buckets);
	free(map);
}

struct map *map_create(void)
{
	struct map *map;

	map = calloc(1, sizeof(struct map));
	if (!map)
		return NULL;

	map->head = calloc(1, sizeof(struct pair));
	if (!map->head) {
		free(map);
		return NULL;
	}

	map->buckets = calloc(MAP_BUCKETS, sizeof(*map->buckets));
	if (!map->buckets) {
		free(map->head);
		free(map);
		return NULL;
	}

	map->tail = map->head;
	map->nbuckets = MAP_BUCKETS;

	return map;
}
