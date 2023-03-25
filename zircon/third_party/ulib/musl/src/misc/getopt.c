#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "libc.h"

char* optarg;
int optind = 1, opterr = 1, optopt, __optpos, __optreset = 0;

#define optpos __optpos
weak_alias(__optreset, optreset);

void __getopt_msg(const char* a, const char* b, const char* c, size_t l) {
  FILE* f = stderr;
  flockfile(f);
  (void)(fputs(a, f) >= 0 && fwrite(b, strlen(b), 1, f) && fwrite(c, 1, l, f) == l &&
         putc('\n', f));
  funlockfile(f);
}

int getopt(int argc, char* const argv[], const char* optstring) {
  int i;
  wchar_t c, d;
  int k, l;
  char* optchar;

  if (!optind || __optreset) {
    __optreset = 0;
    __optpos = 0;
    optind = 1;
  }

  if (optind >= argc || !argv[optind])
    return -1;

  if (argv[optind][0] != '-') {
    if (optstring[0] == '-') {
      optarg = argv[optind++];
      return 1;
    }
    return -1;
  }

  if (!argv[optind][1])
    return -1;

  if (argv[optind][1] == '-' && !argv[optind][2])
    return optind++, -1;

  if (!optpos)
    optpos++;
  if ((k = mbtowc(&c, argv[optind] + optpos, MB_LEN_MAX)) < 0) {
    k = 1;
    c = 0xfffd; /* replacement char */
  }
  optchar = argv[optind] + optpos;
  optopt = c;
  optpos += k;

  if (!argv[optind][optpos]) {
    optind++;
    optpos = 0;
  }

  if (optstring[0] == '-' || optstring[0] == '+')
    optstring++;

  i = 0;
  d = 0;
  do {
    l = mbtowc(&d, optstring + i, MB_LEN_MAX);
    if (l > 0)
      i += l;
    else
      i++;
  } while (l && d != c);

  if (d != c) {
    if (optstring[0] != ':' && opterr)
      __getopt_msg(argv[0], ": unrecognized option: ", optchar, k);
    return '?';
  }
  if (optstring[i] == ':') {
    if (optstring[i + 1] == ':')
      optarg = 0;
    else if (optind >= argc) {
      if (optstring[0] == ':')
        return ':';
      if (opterr)
        __getopt_msg(argv[0], ": option requires an argument: ", optchar, k);
      return '?';
    }
    if (optstring[i + 1] != ':' || optpos) {
      optarg = argv[optind++] + optpos;
      optpos = 0;
    }
  }
  return c;
}

weak_alias(getopt, __posix_getopt);
