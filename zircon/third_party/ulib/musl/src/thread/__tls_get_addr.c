#include <stddef.h>

#include "libc.h"
#include "threads_impl.h"

void* __tls_get_addr(size_t* v) {
  thrd_t self = __thrd_current();
  if (v[0] <= (size_t)self->head.dtv[0])
    return (char*)self->head.dtv[v[0]] + v[1] + DTP_OFFSET;
  return __tls_get_new(v[1], v[0]);
}
