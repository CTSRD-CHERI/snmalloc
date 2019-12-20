#pragma once

using namespace std;

#include "../ds/address.h"
#include "chunkmap_consts.h"
#include "largealloc.h"
#include "mediumslab.h"
#include "pagemap.h"
#include "slab.h"

namespace snmalloc
{
  /*
   * Ensure that ChunkMapSuperslabKind values are actually disjoint, i.e.,
   * that large allocations don't land on CMMediumslab.
   */
  static_assert(
    SUPERSLAB_BITS > CMMediumslab, "Large allocations may be too small");

#ifndef SNMALLOC_MAX_FLATPAGEMAP_SIZE
// Use flat map is under a single node.
#  define SNMALLOC_MAX_FLATPAGEMAP_SIZE PAGEMAP_NODE_SIZE
#endif

  template<typename T>
  static constexpr bool use_flatpagemap = pal_supports<LazyCommit> ||
    (SNMALLOC_MAX_FLATPAGEMAP_SIZE >= sizeof(FlatPagemap<SUPERSLAB_BITS, T>));

  template<auto V>
  using DefaultChunkmapPagemapTemplate = std::conditional_t<
    use_flatpagemap<decltype(V)>,
    FlatPagemap<SUPERSLAB_BITS, decltype(V)>,
    Pagemap<SUPERSLAB_BITS, decltype(V), V>>;

  /**
   * Mixin used by `ChunkMap` to directly access the pagemap via a global
   * variable.  This should be used from within the library or program that
   * owns the pagemap.
   *
   * This class makes the global pagemap a static field so that its name
   * includes the type mangling.  If two compilation units try to instantiate
   * two different types of pagemap then they will see two distinct pagemaps.
   * This will prevent allocating with one and freeing with the other (because
   * the memory will show up as not owned by any allocator in the other
   * compilation unit) but will prevent the same memory being interpreted as
   * having two different types.
   */
  template<typename T>
  class GlobalPagemapTemplate
  {
    /**
     * The global pagemap variable.  The name of this symbol will include the
     * type of `T`.
     */
    inline static T global_pagemap;

  public:
    /**
     * Returns the pagemap.
     */
    static T& pagemap()
    {
      return global_pagemap;
    }
  };

  /**
   * Optionally exported function that accesses the global pagemap provided by
   * a shared library.
   */
  extern "C" void* snmalloc_pagemap_global_get(snmalloc::PagemapConfig const**);

  /**
   * Mixin used by `ChunkMap` to access the global pagemap via a
   * type-checked C interface.  This should be used when another library (e.g.
   * your C standard library) uses snmalloc and you wish to use a different
   * configuration in your program or library, but wish to share a pagemap so
   * that either version can deallocate memory.
   */
  template<typename T>
  class ExternalGlobalPagemap
  {
    /**
     * A pointer to the pagemap.
     */
    inline static T* external_pagemap;

  public:
    /**
     * Constructor.  Accesses the pagemap via the C ABI accessor and casts it to
     * the expected type, failing in cases of ABI mismatch.
     */
    ExternalGlobalPagemap()
    {
      const snmalloc::PagemapConfig* c;
      external_pagemap = T::cast_to_pagemap(snmalloc_pagemap_global_get(&c), c);
      if (!external_pagemap)
      {
        Pal::error("Incorrect ABI of global pagemap.");
      }
    }

    /**
     * Returns the exported pagemap.
     */
    static T& pagemap()
    {
      return *external_pagemap;
    }
  };

  /**
   * Class that defines an interface to the pagemap.  This is provided to
   * `Allocator` as a template argument and so can be replaced by a compatible
   * implementation (for example, to move pagemap updates to a different
   * protection domain).
   *
   * Because we heavily rely on static dispatch, this is declared final to
   * prevent accidental confusion.  Replacement `ChunkMap`s should provide the
   * complete implentation of this interface.
   */
  template<
    template<typename>
    typename PagemapProviderTemplate,
    template<auto>
    typename ChunkmapPagemapTemplate>
  struct DefaultChunkMap final
  {
    using ChunkmapPagemap = ChunkmapPagemapTemplate<static_cast<uint8_t>(0)>;
    using PagemapProvider = PagemapProviderTemplate<ChunkmapPagemap>;

#ifdef SNMALLOC_EXPOSE_PAGEMAP
    static ChunkmapPagemap& expose_pagemap()
    {
      return PagemapProvider::pagemap();
    }
#endif

    /**
     * Get the pagemap entry corresponding to a specific address.
     *
     * Despite the type, the return value is an enum ChunkMapSuperslabKind
     * or one of the reserved values described therewith.
     */
    static uint8_t get(address_t p)
    {
      return PagemapProvider::pagemap().get(p);
    }

    /**
     * Get the pagemap entry corresponding to a specific address.
     */
    static uint8_t get(void* p)
    {
      return get(address_cast(p));
    }

    /**
     * Some architectures (notably, CHERI) need the ability to rederive an
     * internal, privileged pointer from a pointer given out as a result of
     * allocation.  This provides such a hook.  On architectures where
     * pointers are just integers, this is a no-op that should always be
     * inlined into nothing.
     *
     * The template paramter controls whether the returned pointer points
     * to the same place as the argument or to the beginning of the chunkmap
     * region containing the argument.
     */
    template<bool offset = true>
    SNMALLOC_FAST_PATH static void* getp(void* p)
    {
      static_assert(offset);
      return p;
    }

    /**
     * Set a pagemap entry indicating that there is a superslab at the
     * specified index.
     */
    static void set_slab(Superslab* slab)
    {
      set(slab, static_cast<size_t>(CMSuperslab));
    }
    /**
     * Add a pagemap entry indicating that a medium slab has been allocated.
     */
    static void set_slab(Mediumslab* slab)
    {
      set(slab, static_cast<size_t>(CMMediumslab));
    }
    /**
     * Remove an entry from the pagemap corresponding to a superslab.
     */
    static void clear_slab(Superslab* slab)
    {
      assert(get(slab) == CMSuperslab);
      set(slab, static_cast<size_t>(CMNotOurs));
    }
    /**
     * Remove an entry corresponding to a medium slab.
     */
    static void clear_slab(Mediumslab* slab)
    {
      assert(get(slab) == CMMediumslab);
      set(slab, static_cast<size_t>(CMNotOurs));
    }
    /**
     * Update the pagemap to reflect a large allocation, of `size` bytes from
     * address `p`.
     */
    static void set_large_size(void* p, size_t size)
    {
      size_t size_bits = bits::next_pow2_bits(size);
      set(p, static_cast<uint8_t>(size_bits));
      // Set redirect slide
      auto ss = address_cast(p) + SUPERSLAB_SIZE;
      for (size_t i = 0; i < size_bits - SUPERSLAB_BITS; i++)
      {
        size_t run = 1ULL << i;
        PagemapProvider::pagemap().set_range(
          ss, static_cast<uint8_t>(64 + i + SUPERSLAB_BITS), run);
        ss = ss + SUPERSLAB_SIZE * run;
      }
    }
    /**
     * Update the pagemap to remove a large allocation, of `size` bytes from
     * address `p`.
     */
    static void clear_large_size(void* vp, size_t size)
    {
      auto p = address_cast(vp);
      size_t rounded_size = bits::next_pow2(size);
      assert(get(p) == bits::next_pow2_bits(size));
      auto count = rounded_size >> SUPERSLAB_BITS;
      PagemapProvider::pagemap().set_range(p, CMNotOurs, count);
    }

  private:
    /**
     * Helper function to set a pagemap entry.  This is not part of the public
     * interface and exists to make it easy to reuse the code in the public
     * methods in other pagemap adaptors.
     */
    static void set(void* p, uint8_t x)
    {
      PagemapProvider::pagemap().set(address_cast(p), x);
    }
  };

#ifndef SNMALLOC_DEFAULT_CHUNKMAP
#  define SNMALLOC_DEFAULT_CHUNKMAP \
    snmalloc::Pal:: \
      PalChunkMap<GlobalPagemapTemplate, DefaultChunkmapPagemapTemplate>
#endif

} // namespace snmalloc
