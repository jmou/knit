#pragma once

#include "job.h"
#include "production.h"

struct production* read_cache(const struct job* job);
int write_cache(const struct job* job, const struct production* prd);
// Open cache file for read. Return its file descriptor, or -1 if nonexistent or
// on error.
int open_cache_file(const struct job* job);
