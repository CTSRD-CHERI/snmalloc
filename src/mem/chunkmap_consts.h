#pragma once

/*
 * This needs to be sufficiently early in the include order that it can be
 * included by PALs, which may provide alternate implementations of the
 * ChunkMap
 */

namespace snmalloc
{
  enum ChunkMapSuperslabKind
  {
    CMNotOurs = 0,
    CMSuperslab = 1,
    CMMediumslab = 2

    /*
     * Values 3 (inclusive) through SUPERSLAB_BITS (exclusive) are as yet
     * unused.
     *
     * Values SUPERSLAB_BITS (inclusive) through 64 (exclusive, as it would
     * represent the entire address space) are used for log2(size) at the
     * heads of large allocations.  See SuperslabMap::set_large_size.
     *
     * Values 64 (inclusive) through 128 (exclusive) are used for entries
     * within a large allocation.  A value of x at pagemap entry p indicates
     * that there are at least 2^(x-64) (inclusive) and at most 2^(x+1-64)
     * (exclusive) page map entries between p and the start of the
     * allocation.  See SuperslabMap::set_large_size and external_address's
     * handling of large reallocation redirections.
     *
     * Values 128 (inclusive) through 255 (inclusive) are as yet unused.
     */

  };
}
