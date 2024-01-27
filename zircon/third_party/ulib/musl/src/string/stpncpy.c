#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "libc.h"

#define ALIGN (sizeof(size_t) - 1)
#define ONES ((size_t)-1 / UCHAR_MAX)
#define HIGHS (ONES * (UCHAR_MAX / 2 + 1))
#define HASZERO(x) (((x)-ONES) & ~(x)&HIGHS)

__attribute__((visibility("hidden"))) char* __stpncpy(char* restrict d, const char* restrict s,
                                                      size_t n) {
  // This reads past the end of the string, which is usually OK since
  // it won't cross a page boundary.  But under ASan, even one byte
  // past the actual end is diagnosed.
  if (STRICT_BYTE_ACCESS && ((uintptr_t)s & ALIGN) == ((uintptr_t)d & ALIGN)) {
    for (; ((uintptr_t)s & ALIGN) && n && (*d = *s); n--, s++, d++)
      ;
    if (!n || !*s)
      goto tail;
    size_t* wd = (void*)d;
    const size_t* ws = (const void*)s;
    for (; n >= sizeof(size_t) && !HASZERO(*ws); n -= sizeof(size_t), ws++, wd++)
      *wd = *ws;
    d = (void*)wd;
    s = (const void*)ws;
  }
  for (; n && (*d = *s); n--, s++, d++)
    ;
tail:
  memset(d, 0, n);
  return d;
}

strong_alias(__stpncpy, stpncpy);
