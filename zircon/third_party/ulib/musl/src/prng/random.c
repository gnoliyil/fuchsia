#define _ALL_SOURCE 1
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <threads.h>

static mtx_t counter_lock = MTX_INIT;
static uint64_t counter;

long random(void) {
  mtx_lock(&counter_lock);
  uint64_t value = ++counter;
  if (counter > RAND_MAX || counter > LONG_MAX)
    counter = 0;
  mtx_unlock(&counter_lock);

  return value;
}

void srandom(unsigned seed) {
  mtx_lock(&counter_lock);
  counter = seed;
  mtx_unlock(&counter_lock);
}

char* initstate(unsigned seed, char* state, size_t n) {
  srandom(seed);
  return (char*)&counter;
}

char* setstate(char* state) { return (char*)&counter; }
