/*
 * Copyright (C) 2015 Jan-Piet Mens <jpmens@gmail.com> and OwnTracks
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * JAN-PIET MENS OR OWNTRACKS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "gcache.h"
#include "util.h"

struct gcache *gcache_open(char *path, int rdonly)
{
	MDB_txn *txn = NULL;
	int rc;
	unsigned int flags = 0;
	struct gcache *gc;

	if (!is_directory(path)) {
		fprintf(stderr, "gcache_open: %s is not a directory\n", path);
		return (NULL);
	}

	if ((gc = malloc(sizeof (struct gcache))) == NULL)
		return (NULL);

	memset(gc, 0, sizeof(struct gcache));

	if (rdonly) {
		flags |= MDB_RDONLY;
	}

	rc = mdb_env_create(&gc->env);
	if (rc != 0) {
		fprintf(stderr, "%s\n", mdb_strerror(rc));
		free(gc);
		return (NULL);
	}

	mdb_env_set_mapsize(gc->env, LMDB_DB_SIZE);

	rc = mdb_env_open(gc->env, path, flags, 0664);
	if (rc != 0) {
		fprintf(stderr, "%s\n", mdb_strerror(rc));
		free(gc);
		return (NULL);
	}

	/* Open a pseudo TX so that we can open DBI */

	mdb_txn_begin(gc->env, NULL, flags, &txn);
	if (rc != 0) {
		fprintf(stderr, "%s\n", mdb_strerror(rc));
		mdb_env_close(gc->env);
		free(gc);
		return (NULL);
	}

	rc = mdb_dbi_open(txn, NULL, 0, &gc->dbi);
	if (rc != 0) {
	write(1, "HELLO\n", 6);
		fprintf(stderr, "%s\n", mdb_strerror(rc));
		mdb_txn_abort(txn);
		mdb_env_close(gc->env);
		free(gc);
		return (NULL);
	}

	rc = mdb_txn_commit(txn);
	if (rc != 0) {
		fprintf(stderr, "commit after open %s\n", mdb_strerror(rc));
		mdb_env_close(gc->env);
		free(gc);
		return (NULL);
	}

	return (gc);
}

void gcache_close(struct gcache *gc)
{
	if (gc == NULL)
		return;

	mdb_env_close(gc->env);
	free(gc);
}

int gcache_put(struct gcache *gc, char *ghash, char *payload)
{
	int rc;
	MDB_val key, data;
	MDB_txn *txn;

	if (gc == NULL)
		return (1);

	rc = mdb_txn_begin(gc->env, NULL, 0, &txn);
	if (rc != 0)
		fprintf(stderr, "%s\n", mdb_strerror(rc));

	key.mv_data	= ghash;
	key.mv_size	= strlen(ghash);
	data.mv_data	= payload;
	data.mv_size	= strlen(payload) + 1;	/* including nul-byte so we can
						 * later decode string directly
						 * from this buffer */

	rc = mdb_put(txn, gc->dbi, &key, &data, 0);
	if (rc != 0)
		fprintf(stderr, "%s\n", mdb_strerror(rc));

	rc = mdb_txn_commit(txn);
	if (rc) {
		fprintf(stderr, "mdb_txn_commit: (%d) %s\n", rc, mdb_strerror(rc));
		mdb_txn_abort(txn);
	}
	return (rc);
}

int gcache_json_put(struct gcache *gc, char *ghash, JsonNode *geo)
{
	int rc;
	char *js;

	if (gc == NULL)
		return (1);

	if ((js = json_stringify(geo, NULL)) == NULL) {
		fprintf(stderr, "%s\n", "CAN'T stringify JSON during gache_json_put()");
		return (1);
	}

	rc = gcache_put(gc, ghash, js);
	free(js);
	return (rc);
}

int gcache_get(struct gcache *gc, char *k)
{
	MDB_val key, data;
	MDB_txn *txn;
	int rc;

	if (gc == NULL)
		return (1);

	rc = mdb_txn_begin(gc->env, NULL, MDB_RDONLY, &txn);
	if (rc) {
		fprintf(stderr, "gcache_get: cannot txn_begin: (%d) %s\n", rc, mdb_strerror(rc));
		return (1);
	}

	key.mv_data = k;
	key.mv_size = strlen(k);
	
	rc = mdb_get(txn, gc->dbi, &key, &data);
	if (rc != 0) {
		if (rc != MDB_NOTFOUND) {
			printf("get: %s\n", mdb_strerror(rc));
		} else {
			printf(" [%s] not found\n", k);
		}
	} else {
		printf("%s\n", (char *)data.mv_data);
	}
	mdb_txn_commit(txn);
	return (0);
}

/*
 * Attempt to get key `k` (a geohash string) from LMDB. If
 * found, decode the JSON string in it and return a JSON
 * object, else NULL.
 */

JsonNode *gcache_json_get(struct gcache *gc, char *k)
{
	MDB_val key, data;
	MDB_txn *txn;
	int rc;
	JsonNode *geo = NULL;

	if (gc == NULL)
		return (NULL);

	rc = mdb_txn_begin(gc->env, NULL, MDB_RDONLY, &txn);
	if (rc) {
		fprintf(stderr, "gcache_get: cannot txn_begin: (%d) %s\n", rc, mdb_strerror(rc));
		return (NULL);
	}

	key.mv_data = k;
	key.mv_size = strlen(k);
	
	rc = mdb_get(txn, gc->dbi, &key, &data);
	if (rc != 0) {
		if (rc != MDB_NOTFOUND) {
			fprintf(stderr, "gcache_json_get(%s): %s\n", k, mdb_strerror(rc));
		} else {
			// printf(" [%s] not found\n", k);
			geo = NULL;
		}
	} else {
		// printf("%s\n", (char *)data.mv_data);
		if ((geo = json_decode((char *)data.mv_data)) == NULL) {
			fprintf(stderr, "Cannot decode JSON from lmdb\n");
		}
	}

	mdb_txn_commit(txn);

	return (geo);
}
