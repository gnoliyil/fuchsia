#include <stdlib.h>
#include <string.h>

#include "rand48_impl.h"

unsigned short* seed48(unsigned short s[3]) {
  static unsigned short p[3];
  memcpy(p, __seed48, sizeof p);
  memcpy(__seed48, s, sizeof p);
  return p;
}
