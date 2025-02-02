/*!
 * table_builder.h - sorted string table builder for lcdb
 * Copyright (c) 2022, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/lcdb
 *
 * Parts of this software are based on google/leveldb:
 *   Copyright (c) 2011, The LevelDB Authors. All rights reserved.
 *   https://github.com/google/leveldb
 *
 * See LICENSE for more information.
 */

#ifndef LDB_TABLE_BUILDER_H
#define LDB_TABLE_BUILDER_H

#include <stdint.h>

#include "../util/types.h"

/*
 * Types
 */

struct ldb_dbopt_s;
struct ldb_wfile_s;

typedef struct ldb_tablebuilder_s ldb_tablebuilder_t;

/*
 * Table Builder
 */

/* Create a builder that will store the contents of the table it is
 * building in *file. Does not close the file. It is up to the
 * caller to close the file after calling finish().
 */
ldb_tablebuilder_t *
ldb_tablebuilder_create(const struct ldb_dbopt_s *options,
                        struct ldb_wfile_s *file);

/* REQUIRES: Either finish() or abandon() has been called. */
void
ldb_tablebuilder_destroy(ldb_tablebuilder_t *tb);

int
ldb_tablebuilder_ok(const ldb_tablebuilder_t *tb);

/* Change the options used by this builder. Note: only some of the
 * option fields can be changed after construction. If a field is
 * not allowed to change dynamically and its value in the structure
 * passed to the constructor is different from its value in the
 * structure passed to this method, this method will return an error
 * without changing any fields.
 */
int
ldb_tablebuilder_change_options(ldb_tablebuilder_t *tb,
                                const struct ldb_dbopt_s *options);

/* Add key,value to the table being constructed. */
/* REQUIRES: key is after any previously added key according to comparator. */
/* REQUIRES: finish(), abandon() have not been called */
void
ldb_tablebuilder_add(ldb_tablebuilder_t *tb,
                     const ldb_slice_t *key,
                     const ldb_slice_t *value);

/* Advanced operation: flush any buffered key/value pairs to file.
 * Can be used to ensure that two adjacent entries never live in
 * the same data block. Most clients should not need to use this method.
 * REQUIRES: finish(), abandon() have not been called
 */
void
ldb_tablebuilder_flush(ldb_tablebuilder_t *tb);

/* Return non-ok iff some error has been detected. */
int
ldb_tablebuilder_status(const ldb_tablebuilder_t *tb);

/* Finish building the table. Stops using the file passed to the
 * constructor after this function returns.
 * REQUIRES: finish(), abandon() have not been called
 */
int
ldb_tablebuilder_finish(ldb_tablebuilder_t *tb);

/* Indicate that the contents of this builder should be abandoned. Stops
 * using the file passed to the constructor after this function returns.
 * If the caller is not going to call finish(), it must call abandon()
 * before destroying this builder.
 * REQUIRES: finish(), abandon() have not been called
 */
void
ldb_tablebuilder_abandon(ldb_tablebuilder_t *tb);

/* Number of calls to add() so far. */
uint64_t
ldb_tablebuilder_num_entries(const ldb_tablebuilder_t *tb);

/* Size of the file generated so far. If invoked after a successful
   finish() call, returns the size of the final generated file. */
uint64_t
ldb_tablebuilder_file_size(const ldb_tablebuilder_t *tb);

#endif /* LDB_TABLE_BUILDER_H */
