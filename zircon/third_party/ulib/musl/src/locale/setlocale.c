#define _ALL_SOURCE 1
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "atomic.h"
#include "libc.h"
#include "locale_impl.h"

static char buf[LC_ALL * (LOCALE_NAME_MAX + 1)];

static char* setlocale_one_unlocked(int cat, const char* name) {
  const struct __locale_map* lm;

  if (name)
    libc.global_locale.cat[cat] = lm = __get_locale(cat, name);
  else
    lm = libc.global_locale.cat[cat];

  return (char*)(lm ? lm->name : "C");
}

char* setlocale(int cat, const char* name) {
  static mtx_t lock = MTX_INIT;

  if ((unsigned)cat > LC_ALL)
    return 0;

  mtx_lock(&lock);

  /* For LC_ALL, setlocale is required to return a string which
   * encodes the current setting for all categories. The format of
   * this string is unspecified, and only the following code, which
   * performs both the serialization and deserialization, depends
   * on the format, so it can easily be changed if needed. */
  if (cat == LC_ALL) {
    int i;
    if (name) {
      const char* p = name;
      for (i = 0; i < LC_ALL; i++) {
        const char* part = "C";
        const char* z = strchr(p, ';');
        if (!z) {
          part = p;
        } else if (!strncmp("C.UTF8", p, z - p)) {
          part = "C.UTF8";
        }
        setlocale_one_unlocked(i, part);
      }
    }
    char* s = buf;
    for (i = 0; i < LC_ALL; i++) {
      const struct __locale_map* lm = libc.global_locale.cat[i];
      const char* part = lm ? lm->name : "C";
      size_t l = strlen(part);
      memcpy(s, part, l);
      s[l] = ';';
      s += l + 1;
    }
    *--s = 0;
    mtx_unlock(&lock);
    return buf;
  }

  char* ret = setlocale_one_unlocked(cat, name);

  mtx_unlock(&lock);

  return ret;
}
