#include "../mem/slowalloc.h"
#include "../snmalloc.h"

#include <errno.h>
#include <string.h>

using namespace snmalloc;

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

#ifndef SNMALLOC_NAME_MANGLE
#  define SNMALLOC_NAME_MANGLE(a) a
#endif

extern "C"
{
  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(__malloc_end_pointer)(void* ptr)
  {
    return ThreadAlloc::get()->external_pointer<OnePastEnd>(ptr);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(malloc)(size_t size)
  {
    return ThreadAlloc::get_noncachable()->alloc(size);
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(free)(void* ptr)
  {
    ThreadAlloc::get_noncachable()->dealloc(ptr);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(calloc)(size_t nmemb, size_t size)
  {
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (overflow)
    {
      errno = ENOMEM;
      return nullptr;
    }
    return ThreadAlloc::get_noncachable()->alloc<ZeroMem::YesZero>(sz);
  }

  SNMALLOC_EXPORT size_t SNMALLOC_NAME_MANGLE(malloc_allocation_size)(void* ptr)
  {
    return ThreadAlloc::get()->alloc_size(ptr);
  }

  SNMALLOC_EXPORT size_t SNMALLOC_NAME_MANGLE(malloc_usable_size)(void* ptr)
  {
#ifndef __CHERI_PURE_CAPABILITY__
    return SNMALLOC_NAME_MANGLE(malloc_allocation_size)(ptr);
#else
    size_t allocation_size = SNMALLOC_NAME_MANGLE(malloc_allocation_size)(ptr);
    size_t cap_length = cheri_getlen(ptr);
    return cap_length < allocation_size ? cap_length : allocation_size;
#endif
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(realloc)(void* ptr, size_t size)
  {
    if (size == (size_t)-1)
    {
      errno = ENOMEM;
      return nullptr;
    }
    if (ptr == nullptr)
    {
      return SNMALLOC_NAME_MANGLE(malloc)(size);
    }
    if (size == 0)
    {
      SNMALLOC_NAME_MANGLE(free)(ptr);
      return nullptr;
    }

    auto a = ThreadAlloc::get();

#ifndef NDEBUG
    // This check is redundant, because the check in memcpy will fail if this
    // is skipped, but it's useful for debugging.
    if (a->external_pointer<Start>(ptr) != ptr)
    {
      error(
        "Calling realloc on pointer that is not to the start of an allocation");
    }
#endif

    size_t sz;
#if (SNMALLOC_CHERI_SETBOUNDS == 1)
    /*
     * On CHERI, we can just use the length of the capability we've been
     * given.  While the user might have truncated it, that's their problem.
     */
    sz = cheri_getlen(ptr);
#else
    sz = a->alloc_size(ptr);
#endif

#if SNMALLOC_CHERI_ALIGN == 1
    size = bits::align_up(size, 1 << CHERI_ALIGN_SHIFT(size));
#endif

    bool hold_still = (sz == sizeclass_to_size(size_to_sizeclass(size)));
    if constexpr (SNMALLOC_QUARANTINE_DEALLOC == 1)
    {
      /*
       * When quarantining (and, in particular, revoking), we don't permit
       * holding still, so that we can keep the invariant that only the most
       * recent allocation has access to the current version of the data.
       */
      hold_still = false;
    }

    // Keep the current allocation if the given size is in the same sizeclass.
    if (hold_still)
    {
#if SNMALLOC_CHERI_SETBOUNDS == 1
      /*
       * We've bounded the original allocation to its actual size; so, even
       * though we're not moving anything, we should adjust the bound.
       * While we could adjust downwards without acquiring a privileged
       * pointer, it's easier just to always grab the internal one and fall
       * down again.
       *
       * (Recall that SNMALLOC_CHERI_SETBOUNDS implies
       * SNMALLOC_PAGEMAP_REDERIVE, so our use of getp() here is justified.)
       */
      void *privp = a->pagemap().getp(ptr);
      return cheri_andperm(cheri_csetboundsexact(privp, size),
              CHERI_PERMS_USERSPACE_DATA & ~CHERI_PERM_CHERIABI_VMMAP);
#else
      return ptr;
#endif
    }

    void* p = SNMALLOC_NAME_MANGLE(malloc)(size);
    if (p != nullptr)
    {
      assert(p == a->external_pointer<Start>(p));
      sz = bits::min(size, sz);
      memcpy(p, ptr, sz);
      SNMALLOC_NAME_MANGLE(free)(ptr);
    }
    return p;
  }

#if !defined(__FreeBSD__) && !defined(__OpenBSD__)
  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(reallocarray)(void* ptr, size_t nmemb, size_t size)
  {
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (overflow)
    {
      errno = ENOMEM;
      return nullptr;
    }
    return SNMALLOC_NAME_MANGLE(realloc)(ptr, sz);
  }
#endif

  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(aligned_alloc)(size_t alignment, size_t size)
  {
    assert((size % alignment) == 0);
    (void)alignment;
    return SNMALLOC_NAME_MANGLE(malloc)(size);
  }

  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(memalign)(size_t alignment, size_t size)
  {
    if (
      (alignment == 0) || (alignment == size_t(-1)) ||
      (alignment > SUPERSLAB_SIZE))
    {
      errno = EINVAL;
      return nullptr;
    }
    if ((size + alignment) < size)
    {
      errno = ENOMEM;
      return nullptr;
    }

    size = bits::max(size, alignment);
    snmalloc::sizeclass_t sc = size_to_sizeclass(size);
    if (sc >= NUM_SIZECLASSES)
    {
      // large allocs are 16M aligned.
      return SNMALLOC_NAME_MANGLE(malloc)(size);
    }
    for (; sc < NUM_SIZECLASSES; sc++)
    {
      size = sizeclass_to_size(sc);
      if ((size & (~size + 1)) >= alignment)
      {
        return SNMALLOC_NAME_MANGLE(aligned_alloc)(alignment, size);
      }
    }
    return SNMALLOC_NAME_MANGLE(malloc)(SUPERSLAB_SIZE);
  }

  SNMALLOC_EXPORT int SNMALLOC_NAME_MANGLE(posix_memalign)(
    void** memptr, size_t alignment, size_t size)
  {
    if (
      ((alignment % sizeof(uintptr_t)) != 0) ||
      ((alignment & (alignment - 1)) != 0) || (alignment == 0))
    {
      return EINVAL;
    }

    void* p = SNMALLOC_NAME_MANGLE(memalign)(alignment, size);
    if (p == nullptr)
    {
      return ENOMEM;
    }
    *memptr = p;
    return 0;
  }

#if !defined(__FreeBSD__) && !defined(__OpenBSD__)
  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(valloc)(size_t size)
  {
    return SNMALLOC_NAME_MANGLE(memalign)(OS_PAGE_SIZE, size);
  }
#endif

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(pvalloc)(size_t size)
  {
    if (size == size_t(-1))
    {
      errno = ENOMEM;
      return nullptr;
    }
    return SNMALLOC_NAME_MANGLE(memalign)(
      OS_PAGE_SIZE, (size + OS_PAGE_SIZE - 1) & ~(OS_PAGE_SIZE - 1));
  }

  // Stub implementations for jemalloc compatibility.
  // These are called by FreeBSD's libthr (pthreads) to notify malloc of
  // various events.  They are currently unused, though we may wish to reset
  // statistics on fork if built with statistics.

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(_malloc_prefork)(void) {}
  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(_malloc_postfork)(void) {}
  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(_malloc_first_thread)(void) {}

  SNMALLOC_EXPORT int
    SNMALLOC_NAME_MANGLE(mallctl)(const char*, void*, size_t*, void*, size_t)
  {
    return ENOENT;
  }

#ifdef SNMALLOC_EXPOSE_PAGEMAP
  /**
   * Export the pagemap.  The return value is a pointer to the pagemap
   * structure.  The argument is used to return a pointer to a `PagemapConfig`
   * structure describing the type of the pagemap.  Static methods on the
   * concrete pagemap templates can then be used to safely cast the return from
   * this function to the correct type.  This allows us to preserve some
   * semblance of ABI safety via a pure C API.
   */
  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(snmalloc_pagemap_global_get)(
    PagemapConfig const** config)
  {
    auto& pm = GlobalPagemap::pagemap();
    if (config)
    {
      *config = &SuperslabPagemap::config;
      assert(SuperslabPagemap::cast_to_pagemap(&pm, *config) == &pm);
    }
    return &pm;
  }
#endif

#ifdef SNMALLOC_EXPOSE_RESERVE
  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(snmalloc_reserve_shared)(size_t* size, size_t align)
  {
    return snmalloc::default_memory_provider.reserve<true>(size, align);
  }
#endif

#if !defined(__PIC__) && !defined(NO_BOOTSTRAP_ALLOCATOR)
  // The following functions are required to work before TLS is set up, in
  // statically-linked programs.  These temporarily grab an allocator from the
  // pool and return it.

  void* __je_bootstrap_malloc(size_t size)
  {
    return get_slow_allocator()->alloc(size);
  }

  void* __je_bootstrap_calloc(size_t nmemb, size_t size)
  {
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (overflow)
    {
      errno = ENOMEM;
      return nullptr;
    }
    // Include size 0 in the first sizeclass.
    sz = ((sz - 1) >> (bits::BITS - 1)) + sz;
    return get_slow_allocator()->alloc<ZeroMem::YesZero>(sz);
  }

  void __je_bootstrap_free(void* ptr)
  {
    get_slow_allocator()->dealloc(ptr);
  }
#endif
}
