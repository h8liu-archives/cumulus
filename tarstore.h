/* LBS: An LFS-inspired filesystem backup system
 * Copyright (C) 2006  Michael Vrable
 *
 * Backup data is stored in a collection of objects, which are grouped together
 * into segments for storage purposes.  This implementation of the object store
 * is built on top of libtar, and represents segments as TAR files and objects
 * as files within them. */

#ifndef _LBS_TARSTORE_H
#define _LBS_TARSTORE_H

#include <stdint.h>
#include <libtar.h>

#include <string>
#include <iostream>
#include <sstream>

#include "store.h"

/* A simple wrapper around a single TAR file to represent a segment.  Objects
 * may only be written out all at once, since the tar header must be written
 * first; incremental writing is not supported. */
class Tarfile {
public:
    Tarfile(const std::string &path, const std::string &segment);
    ~Tarfile();

    void write_object(int id, const char *data, size_t len);

private:
    void internal_write_object(const std::string &path,
                               const char *data, size_t len);

    std::string segment_name;
    std::ostringstream checksums;
    TAR *t;
};

class TarSegmentStore {
public:
    // New segments will be stored in the given directory.
    TarSegmentStore(const std::string &path) { this->path = path; }
    ~TarSegmentStore() { sync(); }

    // Writes an object to segment in the store, and returns the name
    // (segment/object) to refer to it.  The optional parameter group can be
    // used to control object placement; objects with different group
    // parameters are kept in separate segments.
    std::string write_object(const char *data, size_t len,
                             const std::string &group = "");

    // Ensure all segments have been fully written.
    void sync();

private:
    struct segment_info {
        Tarfile *file;
        std::string name;       // UUID
        int count;              // Objects written to this segment
    };

    std::string path;
    std::map<std::string, struct segment_info *> segments;
};

#endif // _LBS_TARSTORE_H