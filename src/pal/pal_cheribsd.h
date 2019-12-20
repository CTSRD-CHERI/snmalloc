#pragma once

#if defined(__FreeBSD__) && !defined(_KERNEL)
#  include "../ds/address.h"
#  include "../ds/bits.h"
#  include "../mem/allocconfig.h"
#  include "../mem/chunkmap_consts.h"

#  include <errno.h>
#  include <pthread.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <strings.h>
#  include <sys/mman.h>

namespace snmalloc
{
  class Superslab;
  class Mediumslab;

  class PALCHERIBSD
  {
    /*
     * In CHERI, we have to be able to rederive pointers to headers and
     * metadata given the address of the allocation, since the capabilities we
     * give out have bounds narrowed to the allocation itself.  Since snmalloc
     * already holds a map of the address space, here's a great place to do
     * that.  Rather than store sizes per each SUPERSLAB_SIZE sized piece of
     * memory, we store a capability.
     *
     * We have a lot of "metadata" bits at the least-significant end of the
     * address of a capability in this map, since its bounds must, at least,
     * cover a SUPERSLAB_SIZE sized object (or a large allocation).  To
     * minimize churn, we stash the existing enum PageMapSuperslabKind values
     * in the bottom 8 bits of the address.
     *
     * We could cut that down to 6 bits by reclaiming all values above 64; we
     * can test that the capability given to us to free is has address equal
     * to the base of the capability stored here in the page map.
     */
    static constexpr int PAGEMAP_PTR_ALIGN = 0x100;

    /*
     * The exact representation of NULL doesn't concern us, but this ChunkMap
     * encoding relies on the bottom 8 bits of its address being CMNotOurs.
     */
    static_assert(
      static_cast<uint8_t>(static_cast<uintptr_t>(NULL)) == CMNotOurs);

  public:
    static constexpr size_t ADDRESS_BITS = 39; /* XXX CheriBSD MIPS specific */

    template<
      template<typename>
      typename PagemapProviderTemplate,
      template<auto>
      typename ChunkmapPagemapTemplate>
    struct PalChunkMap final
    {
      using ChunkmapPagemap = ChunkmapPagemapTemplate<static_cast<void*>(NULL)>;
      using PagemapProvider = PagemapProviderTemplate<ChunkmapPagemap>;

#  ifdef SNMALLOC_EXPOSE_PAGEMAP
      static ChunkmapPagemap& expose_pagemap()
      {
        return PagemapProvider::pagemap();
      }
#  endif

      static uint8_t get(address_t p)
      {
        return static_cast<uint8_t>(
          address_cast(PagemapProvider::pagemap().get(p)));
      }

      template<bool offset = true>
      SNMALLOC_FAST_PATH static void* getp(void* p)
      {
        void* pmp = pointer_align_down<PAGEMAP_PTR_ALIGN, void>(
          PagemapProvider::pagemap().get(address_cast(p)));
        if constexpr (offset)
        {
          return pointer_offset(pmp, pointer_diff(pmp, p));
        }
        else
        {
          return pmp;
        }
      }

      static void set_slab(Superslab* slab)
      {
        assert(pointer_align_down<SUPERSLAB_SIZE>(slab) == slab);
        set(slab, pointer_offset(slab, CMSuperslab));
      }

      static void clear_slab(Superslab* slab)
      {
        assert(pointer_align_down<SUPERSLAB_SIZE>(slab) == slab);
        set(slab, NULL);
      }

      static void set_slab(Mediumslab* slab)
      {
        assert(pointer_align_down<SUPERSLAB_SIZE>(slab) == slab);
        set(slab, pointer_offset(slab, CMMediumslab));
      }

      static void clear_slab(Mediumslab* slab)
      {
        assert(pointer_align_down<SUPERSLAB_SIZE>(slab) == slab);
        set(slab, NULL);
      }

      static void set_large_size(void* vp, size_t size)
      {
        size_t size_bits = bits::next_pow2_bits(size);
        set(vp, pointer_offset(vp, size_bits));
        // Set redirect slide
        char* ss = reinterpret_cast<char*>(pointer_offset(vp, SUPERSLAB_SIZE));
        for (size_t i = 0; i < size_bits - SUPERSLAB_BITS; i++)
        {
          size_t run = 1ULL << i;
          PagemapProvider::pagemap().set_range(
            address_cast(ss), pointer_offset(vp, 64 + i + SUPERSLAB_BITS), run);
          ss += SUPERSLAB_SIZE * run;
        }
      }

      static void clear_large_size(void* vp, size_t size)
      {
        auto p = address_cast(vp);
        size_t rounded_size = bits::next_pow2(size);
        assert(get(p) == bits::next_pow2_bits(size));
        auto count = rounded_size >> SUPERSLAB_BITS;
        PagemapProvider::pagemap().set_range(p, NULL, count);
      }

    private:
      static void set(void* p, void* x)
      {
        PagemapProvider::pagemap().set(address_cast(p), x);
      }
    };

    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = LazyCommit | AlignedAllocation;
    static void error(const char* const str)
    {
      puts(str);
      abort();
    }

    /// Notify platform that we will not be using these pages
    void notify_not_using(void* p, size_t size) noexcept
    {
      assert(is_aligned_block<OS_PAGE_SIZE>(p, size));
      madvise(p, size, MADV_FREE);
    }

    /// Notify platform that we will be using these pages
    template<ZeroMem zero_mem>
    void notify_using(void* p, size_t size) noexcept
    {
      assert(is_aligned_block<OS_PAGE_SIZE>(p, size) || (zero_mem == NoZero));
      if constexpr (zero_mem == YesZero)
        zero(p, size);
    }

    /// OS specific function for zeroing memory
    template<bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      if (page_aligned || is_aligned_block<OS_PAGE_SIZE>(p, size))
      {
        assert(is_aligned_block<OS_PAGE_SIZE>(p, size));
        void* r = mmap(
          p,
          size,
          PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
          -1,
          0);

        if (r != MAP_FAILED)
          return;

        /*
         * We're going to fall back to zeroing the memory ourselves, which
         * is not great.  But we also need to zero errno, lest it propagate
         * out to our caller!
         */
        errno = 0;
      }

      bzero(p, size);
    }

    template<bool committed>
    void* reserve(size_t* size, size_t align) noexcept
    {
      size_t request = *size;
      // Alignment must be a power of 2.
      assert(align == bits::next_pow2(align));

      if (align == 0)
      {
        align = 1;
      }

      size_t log2align = bits::next_pow2_bits(align);

      void* p = mmap(
        NULL,
        request,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_ALIGNED(log2align),
        -1,
        0);

      if (p == MAP_FAILED)
        error("Out of memory");

      return p;
    }
  };
}
#endif
