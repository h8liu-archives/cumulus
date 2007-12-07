/* LBS: An LFS-inspired filesystem backup system
 * Copyright (C) 2007  Michael Vrable
 *
 * When creating backup snapshots, maintain a local database of data blocks and
 * checksums, in addition to the data contents (which may be stored remotely).
 * This database is consulted when attempting to build incremental snapshots,
 * as it says which objects can be reused.
 *
 * The database is implemented as an SQLite3 database, but this implementation
 * detail is kept internal to this file, so that the storage format may be
 * changed later. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

#include <string>

#include "localdb.h"
#include "store.h"

using std::string;

/* Helper function to prepare a statement for execution in the current
 * database. */
sqlite3_stmt *LocalDb::Prepare(const char *sql)
{
    sqlite3_stmt *stmt;
    int rc;
    const char *tail;

    rc = sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, &tail);
    if (rc != SQLITE_OK) {
        ReportError(rc);
        throw IOException(string("Error preparing statement: ") + sql);
    }

    return stmt;
}

void LocalDb::ReportError(int rc)
{
    fprintf(stderr, "Result code: %d\n", rc);
    fprintf(stderr, "Error message: %s\n", sqlite3_errmsg(db));
}

void LocalDb::Open(const char *path, const char *snapshot_name,
                   const char *snapshot_scheme)
{
    int rc;

    rc = sqlite3_open(path, &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        throw IOException("Error opening local database");
    }

    rc = sqlite3_exec(db, "begin", NULL, NULL, NULL);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        throw IOException("Error starting transaction");
    }

    sqlite3_extended_result_codes(db, 1);

    /* Insert this snapshot into the database, and determine the integer key
     * which will be used to identify it. */
    sqlite3_stmt *stmt = Prepare("insert into "
                                 "snapshots(name, scheme, timestamp) "
                                 "values (?, ?, julianday('now'))");
    sqlite3_bind_text(stmt, 1, snapshot_name, strlen(snapshot_name),
                      SQLITE_TRANSIENT);
    if (snapshot_scheme == NULL)
        sqlite3_bind_null(stmt, 2);
    else
        sqlite3_bind_text(stmt, 2, snapshot_scheme, strlen(snapshot_scheme),
                          SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        ReportError(rc);
        sqlite3_close(db);
        throw IOException("Database execution error!");
    }

    snapshotid = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    if (snapshotid == 0) {
        ReportError(rc);
        sqlite3_close(db);
        throw IOException("Find snapshot id");
    }

    /* Create a temporary table which will be used to keep track of the objects
     * used by this snapshot.  When the database is closed, we will summarize
     * the results of this table into segments_used. */
    rc = sqlite3_exec(db,
                      "create temporary table snapshot_refs ("
                      "    segmentid integer not null,"
                      "    object text not null,"
                      "    size integer not null"
                      ")", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        ReportError(rc);
        sqlite3_close(db);
        throw IOException("Database initialization");
    }
    rc = sqlite3_exec(db,
                      "create unique index snapshot_refs_index "
                      "on snapshot_refs(segmentid, object)",
                      NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        ReportError(rc);
        sqlite3_close(db);
        throw IOException("Database initialization");
    }
}

void LocalDb::Close()
{
    int rc;

    /* Summarize the snapshot_refs table into segments_used. */
    sqlite3_stmt *stmt = Prepare("insert into segments_used "
                                 "select ? as snapshotid, segmentid, "
                                 "cast(used as real) / size as utilization "
                                 "from "
                                 "(select segmentid, sum(size) as used "
                                 "from snapshot_refs group by segmentid) "
                                 "join segments using (segmentid)");
    sqlite3_bind_int64(stmt, 1, snapshotid);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_OK && rc != SQLITE_DONE) {
        ReportError(rc);
        sqlite3_close(db);
        fprintf(stderr, "DATABASE ERROR: Unable to create segment summary!\n");
    }
    sqlite3_finalize(stmt);

    /* Commit changes to the database and close. */
    rc = sqlite3_exec(db, "commit", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "DATABASE ERROR: Can't commit database!\n");
        ReportError(rc);
    }
    sqlite3_close(db);
}

int64_t LocalDb::SegmentToId(const string &segment)
{
    int rc;
    sqlite3_stmt *stmt;
    int64_t result;

    stmt = Prepare("insert or ignore into segments(segment) values (?)");
    sqlite3_bind_text(stmt, 1, segment.c_str(), segment.size(),
                      SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        throw IOException("Could not execute INSERT statement!");
    }
    sqlite3_finalize(stmt);

    stmt = Prepare("select segmentid from segments where segment = ?");
    sqlite3_bind_text(stmt, 1, segment.c_str(), segment.size(),
                      SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        throw IOException("No segment found by id");
    } else if (rc == SQLITE_ROW) {
        result = sqlite3_column_int64(stmt, 0);
    } else {
        throw IOException("Error executing find segment by id query");
    }

    sqlite3_finalize(stmt);

    return result;
}

string LocalDb::IdToSegment(int64_t segmentid)
{
    int rc;
    sqlite3_stmt *stmt;
    string result;

    stmt = Prepare("select segment from segments where segmentid = ?");
    sqlite3_bind_int64(stmt, 1, segmentid);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        throw IOException("No segment found by id");
    } else if (rc == SQLITE_ROW) {
        result = (const char *)sqlite3_column_text(stmt, 0);
    } else {
        throw IOException("Error executing find segment by id query");
    }

    sqlite3_finalize(stmt);

    return result;
}

void LocalDb::StoreObject(const ObjectReference& ref,
                          const string &checksum, int64_t size,
                          double age)
{
    int rc;
    sqlite3_stmt *stmt;

    if (age == 0.0) {
        stmt = Prepare("insert into block_index("
                       "segmentid, object, checksum, size, timestamp) "
                       "values (?, ?, ?, ?, julianday('now'))");
    } else {
        stmt = Prepare("insert into block_index("
                       "segmentid, object, checksum, size, timestamp) "
                       "values (?, ?, ?, ?, ?)");
    }

    sqlite3_bind_int64(stmt, 1, SegmentToId(ref.get_segment()));
    string obj = ref.get_sequence();
    sqlite3_bind_text(stmt, 2, obj.c_str(), obj.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, checksum.c_str(), checksum.size(),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, size);
    if (age != 0.0)
        sqlite3_bind_double(stmt, 5, age);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Could not execute INSERT statement!\n");
        ReportError(rc);
    }

    sqlite3_finalize(stmt);
}

ObjectReference LocalDb::FindObject(const string &checksum, int64_t size)
{
    int rc;
    sqlite3_stmt *stmt;
    ObjectReference ref;

    stmt = Prepare("select segmentid, object from block_index "
                   "where checksum = ? and size = ? and expired is null");
    sqlite3_bind_text(stmt, 1, checksum.c_str(), checksum.size(),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, size);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
    } else if (rc == SQLITE_ROW) {
        ref = ObjectReference(IdToSegment(sqlite3_column_int64(stmt, 0)),
                              (const char *)sqlite3_column_text(stmt, 1));
    } else {
        fprintf(stderr, "Could not execute SELECT statement!\n");
        ReportError(rc);
    }

    sqlite3_finalize(stmt);

    return ref;
}

bool LocalDb::IsOldObject(const string &checksum, int64_t size, double *age,
                          int *group)
{
    int rc;
    sqlite3_stmt *stmt;
    bool found = false;

    stmt = Prepare("select segmentid, object, timestamp, expired "
                   "from block_index where checksum = ? and size = ?");
    sqlite3_bind_text(stmt, 1, checksum.c_str(), checksum.size(),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, size);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        found = false;
    } else if (rc == SQLITE_ROW) {
        found = true;
        *age = sqlite3_column_double(stmt, 2);
        *group = sqlite3_column_int(stmt, 3);
    } else {
        fprintf(stderr, "Could not execute SELECT statement!\n");
        ReportError(rc);
    }

    sqlite3_finalize(stmt);

    return found;
}

/* Does this object still exist in the database (and not expired)? */
bool LocalDb::IsAvailable(const ObjectReference &ref)
{
    int rc;
    sqlite3_stmt *stmt;
    bool found = false;

    // Special objects (such as the zero object) aren't stored in segments, and
    // so are always available.
    if (!ref.is_normal())
        return true;

    stmt = Prepare("select count(*) from block_index "
                   "where segmentid = ? and object = ? and expired is null");
    sqlite3_bind_int64(stmt, 1, SegmentToId(ref.get_segment()));
    sqlite3_bind_text(stmt, 2, ref.get_sequence().c_str(),
                      ref.get_sequence().size(), SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        found = false;
    } else if (rc == SQLITE_ROW) {
        if (sqlite3_column_int(stmt, 0) > 0)
            found = true;
    } else {
        fprintf(stderr, "Could not execute SELECT statement!\n");
        ReportError(rc);
    }

    sqlite3_finalize(stmt);

    return found;
}

void LocalDb::UseObject(const ObjectReference& ref)
{
    int rc;
    sqlite3_stmt *stmt;

    if (!ref.is_normal())
        return;

    stmt = Prepare("insert or ignore into snapshot_refs "
                   "select segmentid, object, size from block_index "
                   "where segmentid = ? and object = ?");
    sqlite3_bind_int64(stmt, 1, SegmentToId(ref.get_segment()));
    string obj = ref.get_sequence();
    sqlite3_bind_text(stmt, 2, obj.c_str(), obj.size(), SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Could not execute INSERT statement!\n");
        ReportError(rc);
    }

    sqlite3_finalize(stmt);
}

void LocalDb::SetSegmentChecksum(const std::string &segment,
                                 const std::string &path,
                                 const std::string &checksum)
{
    int rc;
    sqlite3_stmt *stmt;

    stmt = Prepare("update segments set path = ?, checksum = ?, "
                   "size = (select sum(size) from block_index "
                   "        where segmentid = ?) "
                   "where segmentid = ?");
    sqlite3_bind_text(stmt, 1, path.c_str(), path.size(),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, checksum.c_str(), checksum.size(),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, SegmentToId(segment));
    sqlite3_bind_int64(stmt, 4, SegmentToId(segment));

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Could not update segment checksum in database!\n");
        ReportError(rc);
    }

    sqlite3_finalize(stmt);
}

bool LocalDb::GetSegmentChecksum(const string &segment,
                                 string *seg_path,
                                 string *seg_checksum)
{
    int rc;
    sqlite3_stmt *stmt;
    ObjectReference ref;
    int found = false;

    stmt = Prepare("select path, checksum from segments where segment = ?");
    sqlite3_bind_text(stmt, 1, segment.c_str(), segment.size(),
                      SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
    } else if (rc == SQLITE_ROW) {
        found = true;
        const char *val;

        val = (const char *)sqlite3_column_text(stmt, 0);
        if (val == NULL)
            found = false;
        else
            *seg_path = val;

        val = (const char *)sqlite3_column_text(stmt, 1);
        if (val == NULL)
            found = false;
        else
            *seg_checksum = val;
    } else {
        fprintf(stderr, "Could not execute SELECT statement!\n");
        ReportError(rc);
    }

    sqlite3_finalize(stmt);

    return found;
}
