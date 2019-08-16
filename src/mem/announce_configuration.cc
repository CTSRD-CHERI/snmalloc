#include "allocconfig.h"
extern "C" {
  #include <string.h>
  #include <unistd.h>
}

namespace snmalloc {
  static void __attribute__((constructor)) snmalloc_announce_configuration(void) {
    const char * verdesc = "snmalloc "
#define VERDESC_STR(x) #x
#define VERDESC_XSTR(x) VERDESC_STR(x)
#if (SNMALLOC_PAGEMAP_REDERIVE == 1)
        "pm+rederive " 
#elif (SNMALLOC_PAGEMAP_POINTERS == 1)
        "pm+pointers " 
#endif
#if (SNMALLOC_CHERI_SETBOUNDS == 1)
        "cheri+bounds "
#endif
#if defined(CHECK_CLIENT)
        "check-client"
#endif
		" "
		GIT_VERSION
    "\n";

    write(2, verdesc, strlen(verdesc));
  }
};
