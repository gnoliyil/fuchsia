#define _GNU_SOURCE
#include "dynlink.h"

#include <ctype.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <lib/processargs/processargs.h>
#include <lib/zircon-internal/align.h>
#include <lib/zircon-internal/default_stack_size.h>
#include <lib/zircon-internal/unique-backtrace.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <zircon/dlfcn.h>
#include <zircon/fidl.h>
#include <zircon/process.h>
#include <zircon/sanitizer.h>
#include <zircon/status.h>
#include <zircon/syscalls/log.h>

#include <ldmsg/ldmsg.h>
#include <runtime/thread.h>

#include "asan_impl.h"
#include "libc.h"
#include "relr.h"
#include "stdio_impl.h"
#include "threads_impl.h"
#include "zircon_impl.h"

static void early_init(void);
static void error(const char*, ...);
static void debugmsg(const char*, ...);
static zx_status_t get_library_vmo(const char* name, zx_handle_t* vmo);
static void loader_svc_config(const char* config);

#define MAXP2(a, b) (-(-(a) & -(b)))

#define VMO_NAME_DL_ALLOC "ld.so.1-internal-heap"
#define VMO_NAME_UNKNOWN "<unknown ELF file>"
#define VMO_NAME_PREFIX_BSS "bssN:"
#define VMO_NAME_PREFIX_DATA "dataN:"
#define VMO_NAME_PREFIX_RELRO "relro:"

#define KEEP_DSO_VMAR __has_feature(xray_instrument)

struct dso {
  // Must be first.
  struct link_map l_map;

  const struct gnu_note* build_id_note;
  atomic_flag logged;

  // ID of this module for symbolizer markup.
  unsigned int module_id;

  const char* soname;
  Phdr* phdr;
  unsigned int phnum;
  size_t phentsize;
  int refcnt;
  zx_handle_t vmar;  // Closed after relocation.
  Sym* syms;
  uint32_t* hashtab;
  uint32_t* ghashtab;
  int16_t* versym;
  char* strings;
  unsigned char* map;
  size_t map_len;
  signed char global;
  char relocated;
  char constructed;
  struct dso **deps, *needed_by;
  struct tls_module tls;
  size_t tls_id;
  size_t code_start, code_end;
  size_t relro_start, relro_end;
  void** new_dtv;
  unsigned char* new_tls;
  atomic_int new_dtv_idx, new_tls_idx;
  struct dso* fini_next;
  struct funcdesc {
    void* addr;
    size_t* got;
  }* funcdescs;
  size_t* got;
  bool in_dlsym;
  struct dso* buf[];
};

struct symdef {
  Sym* sym;
  struct dso* dso;
};

union gnu_note_name {
  char name[sizeof("GNU")];
  uint32_t word;
};
#define GNU_NOTE_NAME ((union gnu_note_name){.name = "GNU"})
_Static_assert(sizeof(GNU_NOTE_NAME.name) == sizeof(GNU_NOTE_NAME.word), "");

struct gnu_note {
  Elf64_Nhdr nhdr;
  union gnu_note_name name;
  alignas(4) uint8_t desc[];
};

#define MIN_TLS_ALIGN alignof(struct pthread)

#define NO_INLINE __attribute__((noinline))

#define ADDEND_LIMIT 32
static size_t *saved_addends, *apply_addends_to;

#ifdef ABI_TCBHEAD_SIZE
#define INITIAL_TLS_OFFSET ABI_TCBHEAD_SIZE
#else
#define INITIAL_TLS_OFFSET 0
#endif

static struct dso ldso, vdso;
static struct dso *head, *tail, *fini_head;
static struct dso* detached_head;
static unsigned long long gencnt;
int runtime __asm__("_dynlink_runtime") __ALWAYS_EMIT __attribute__((__visibility__("hidden")));
static int ldso_fail;
static jmp_buf* rtld_fail;
static pthread_rwlock_t lock;
static struct r_debug debug;
static struct tls_module* tls_tail;
static size_t tls_cnt, tls_align = MIN_TLS_ALIGN;
static size_t tls_offset = INITIAL_TLS_OFFSET;
static size_t static_tls_cnt;
static pthread_mutex_t init_fini_lock = {
    ._m_attr = PTHREAD_MUTEX_MAKE_ATTR(PTHREAD_MUTEX_RECURSIVE, PTHREAD_PRIO_NONE)};

static bool log_libs = false;
static atomic_uintptr_t unlogged_tail;

static zx_handle_t loader_svc = ZX_HANDLE_INVALID;
static zx_handle_t logger = ZX_HANDLE_INVALID;

// Various tools use this value to bootstrap their knowledge of the process.
// E.g., the list of loaded shared libraries is obtained from here.
// The value is stored in the process's ZX_PROPERTY_PROCESS_DEBUG_ADDR so that
// tools can obtain the value when aslr is enabled.
struct r_debug* _dl_debug_addr = &debug;

// If true then dump load map data in a specific format for tracing.
// This is used by Intel PT (Processor Trace) support for example when
// post-processing the h/w trace.
static bool trace_maps = false;

void _dl_rdlock(void) { pthread_rwlock_rdlock(&lock); }
void _dl_unlock(void) { pthread_rwlock_unlock(&lock); }
static void _dl_wrlock(void) { pthread_rwlock_wrlock(&lock); }

NO_ASAN LIBC_NO_SAFESTACK static int dl_strcmp(const char* l, const char* r) {
  for (; *l == *r && *l; l++, r++)
    ;
  return *(unsigned char*)l - *(unsigned char*)r;
}
#define strcmp(l, r) dl_strcmp(l, r)

// Signals a debug breakpoint. It does't use __builtin_trap() because that's
// actually an "undefined instruction" rather than a debug breakpoint, and
// __builtin_trap() documented to never return. We don't want the compiler to
// optimize later code away because it assumes the trap will never be returned
// from.
//
// NOTE: The x64 reported address when reading the exception's instruction pointer
// will be offset by one byte. This is because x64 will report the address as being
// the one *after* executing the breakpoint, while ARM will report the address of
// the breakpoint instruction.  Thus the reporting address will be 1 byte higher
// in the case of x64 and the caller will need to offset it back in order to get
// the correct address of the debug trap.

// This is actually defined with internal linkage in the asm.
// It can't be defined in C since we want the exact address of
// the breakpoint instruction, not just a function containing it.
void debug_break(void);

__asm__(
    ".pushsection .text.debug_break, \"ax\", @progbits\n"
    "debug_break:\n"
    ".cfi_startproc\n"
#if defined(__x86_64__)
    "int3\n"
#elif defined(__aarch64__)
    "brk #0\n"
#elif defined(__riscv)
    "ebreak\n"
#else
#error "what machine?"
#endif
    "ret\n"
    ".cfi_endproc\n"
    ".popsection\n");

LIBC_NO_SAFESTACK static bool should_break_on_load(void) {
  intptr_t dyn_break_on_load = 0;
  zx_status_t status = _zx_object_get_property(__zircon_process_self, ZX_PROP_PROCESS_BREAK_ON_LOAD,
                                               &dyn_break_on_load, sizeof(dyn_break_on_load));
  if (status != ZX_OK || dyn_break_on_load == 0)
    return false;

  if (dyn_break_on_load != (intptr_t)&debug_break) {
    // Set ZX_PROP_PROCESS_BREAK_ON_LOAD to &debug_break. Debuggers use this to know whether
    // a breakpoint exception is to notify changes of dso list.
    dyn_break_on_load = (intptr_t)&debug_break;
    _zx_object_set_property(__zircon_process_self, ZX_PROP_PROCESS_BREAK_ON_LOAD,
                            &dyn_break_on_load, sizeof(dyn_break_on_load));
  };

  return true;
}

// Simple bump allocator for dynamic linker internal data structures.
// This allocator is single-threaded: it can be used only at startup or
// while holding the big lock.  These allocations can never be freed
// once in use.  But it does support a simple checkpoint and rollback
// mechanism to undo all allocations since the checkpoint, used for the
// abortive dlopen case.

union allocated_types {
  struct dso dso;
  size_t tlsdesc[2];
};
#define DL_ALLOC_ALIGN alignof(union allocated_types)

static uintptr_t alloc_base, alloc_limit, alloc_ptr;

LIBC_NO_SAFESTACK NO_ASAN __attribute__((malloc)) static void* dl_alloc(size_t size) {
  // Round the size up so the allocation pointer always stays aligned.
  size = (size + DL_ALLOC_ALIGN - 1) & -DL_ALLOC_ALIGN;

  // Get more pages if needed.  The remaining partial page, if any,
  // is wasted unless the system happens to give us the adjacent page.
  if (alloc_limit - alloc_ptr < size) {
    size_t chunk_size = (size + PAGE_SIZE - 1) & -PAGE_SIZE;
    zx_handle_t vmo;
    zx_status_t status = _zx_vmo_create(chunk_size, 0, &vmo);
    if (status != ZX_OK)
      return NULL;
    _zx_object_set_property(vmo, ZX_PROP_NAME, VMO_NAME_DL_ALLOC, sizeof(VMO_NAME_DL_ALLOC));
    uintptr_t chunk;
    status = _zx_vmar_map(_zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo, 0,
                          chunk_size, &chunk);
    _zx_handle_close(vmo);
    if (status != ZX_OK)
      return NULL;
    if (chunk != alloc_limit)
      alloc_ptr = alloc_base = chunk;
    alloc_limit = chunk + chunk_size;
  }

  void* block = (void*)alloc_ptr;
  alloc_ptr += size;

  return block;
}

struct dl_alloc_checkpoint {
  uintptr_t ptr, base;
};

LIBC_NO_SAFESTACK
static void dl_alloc_checkpoint(struct dl_alloc_checkpoint* state) {
  state->ptr = alloc_ptr;
  state->base = alloc_base;
}

LIBC_NO_SAFESTACK
static void dl_alloc_rollback(const struct dl_alloc_checkpoint* state) {
  uintptr_t frontier = alloc_ptr;
  // If we're still using the same contiguous chunk as the checkpoint
  // state, we can just restore the old state directly and waste nothing.
  // If we've allocated new chunks since then, the best we can do is
  // reset to the beginning of the current chunk, since we haven't kept
  // track of the past chunks.
  alloc_ptr = alloc_base == state->base ? state->ptr : alloc_base;
  memset((void*)alloc_ptr, 0, frontier - alloc_ptr);
}

/* Compute load address for a virtual address in a given dso. */
LIBC_NO_SAFESTACK NO_ASAN static inline size_t saddr(struct dso* p, size_t v) {
  return p->l_map.l_addr + v;
}

LIBC_NO_SAFESTACK NO_ASAN static inline void* laddr(struct dso* p, size_t v) {
  return (void*)saddr(p, v);
}

LIBC_NO_SAFESTACK NO_ASAN static inline void (*fpaddr(struct dso* p, size_t v))(void) {
  return (void (*)(void))saddr(p, v);
}

// Accessors for dso previous and next pointers.
LIBC_NO_SAFESTACK NO_ASAN static inline struct dso* dso_next(struct dso* p) {
  return (struct dso*)p->l_map.l_next;
}

LIBC_NO_SAFESTACK NO_ASAN static inline struct dso* dso_prev(struct dso* p) {
  return (struct dso*)p->l_map.l_prev;
}

LIBC_NO_SAFESTACK NO_ASAN static inline void dso_set_next(struct dso* p, struct dso* next) {
  p->l_map.l_next = next ? &next->l_map : NULL;
}

LIBC_NO_SAFESTACK NO_ASAN static inline void dso_set_prev(struct dso* p, struct dso* prev) {
  p->l_map.l_prev = prev ? &prev->l_map : NULL;
}

// TODO(mcgrathr): Working around arcane compiler issues; find a better way.
// The compiler can decide to turn the loop below into a memset call.  Since
// memset is an exported symbol, calls to that name are PLT calls.  But this
// code runs before PLT calls are available.  So use the .weakref trick to
// tell the assembler to rename references (the compiler generates) to memset
// to __libc_memset.  That's a hidden symbol that won't cause a PLT entry to
// be generated, so it's safe to use in calls here.
//
// Under ASan, the compiler generates calls to __asan_memset instead.
// That is normally a PLT call to the ASan runtime DSO, before PLT
// resolution it might not even have been mapped in yet.
//
// A further issue is that the __asan_memset implementation may use
// ShadowCallStack, but some calls here are before stack ABI setup
// necessary for that to work.  So redirecting to __libc_memset also
// ensures those calls reach libc's own memset implementation, which is
// always a leaf function that doesn't require the ShadowCallStack ABI.
//
// Note this also affects the explicit memset calls made in this source
// file.  That's necessary for some of the instances: those made before PLT
// resolution and/or stack ABI setup are complete.  It's superfluous for
// the instances that can only happen later (e.g. via dl* calls), but
// happens anyway since this symbol redirection is necessary to catch the
// compiler-generated calls.  However, relying on this implicit redirection
// rather than explicitly using __libc_memset in the early-startup calls
// here means that the compiler gets to decide whether to inline each case
// or generate the memset call.
//
// All the same applies to memcpy calls here as well, since __asan_memcpy
// is a PLT call that uses ShadowCallStack.
__asm__(".weakref memcpy,__libc_memcpy");
__asm__(".weakref memset,__libc_memset");
__asan_weak_ref("memcpy") __asan_weak_ref("memset")

    LIBC_NO_SAFESTACK NO_ASAN static void decode_vec(ElfW(Dyn) * v, size_t* a, size_t cnt) {
  size_t i;
  for (i = 0; i < cnt; i++)
    a[i] = 0;
  for (; v->d_tag; v++)
    if (v->d_tag - 1 < cnt - 1) {
      a[0] |= 1UL << v->d_tag;
      a[v->d_tag] = v->d_un.d_val;
    }
}

LIBC_NO_SAFESTACK NO_ASAN static int search_vec(ElfW(Dyn) * v, size_t* r, size_t key) {
  for (; v->d_tag != key; v++)
    if (!v->d_tag)
      return 0;
  *r = v->d_un.d_val;
  return 1;
}

LIBC_NO_SAFESTACK NO_ASAN static uint32_t sysv_hash(const char* s0) {
  const unsigned char* s = (void*)s0;
  uint_fast32_t h = 0;
  while (*s) {
    h = 16 * h + *s++;
    h ^= h >> 24 & 0xf0;
  }
  return h & 0xfffffff;
}

LIBC_NO_SAFESTACK NO_ASAN static uint32_t gnu_hash(const char* s0) {
  const unsigned char* s = (void*)s0;
  uint_fast32_t h = 5381;
  for (; *s; s++)
    h += h * 32 + *s;
  return h;
}

LIBC_NO_SAFESTACK NO_ASAN static Sym* sysv_lookup(const char* s, uint32_t h, struct dso* dso) {
  size_t i;
  Sym* syms = dso->syms;
  uint32_t* hashtab = dso->hashtab;
  char* strings = dso->strings;
  for (i = hashtab[2 + h % hashtab[0]]; i; i = hashtab[2 + hashtab[0] + i]) {
    if ((!dso->versym || dso->versym[i] >= 0) && (!strcmp(s, strings + syms[i].st_name)))
      return syms + i;
  }
  return 0;
}

LIBC_NO_SAFESTACK NO_ASAN static Sym* gnu_lookup(uint32_t h1, uint32_t* hashtab, struct dso* dso,
                                                 const char* s) {
  uint32_t nbuckets = hashtab[0];
  uint32_t* buckets = hashtab + 4 + hashtab[2] * (sizeof(size_t) / 4);
  uint32_t i = buckets[h1 % nbuckets];

  if (!i)
    return 0;

  uint32_t* hashval = buckets + nbuckets + (i - hashtab[1]);

  for (h1 |= 1;; i++) {
    uint32_t h2 = *hashval++;
    if ((h1 == (h2 | 1)) && (!dso->versym || dso->versym[i] >= 0) &&
        !strcmp(s, dso->strings + dso->syms[i].st_name))
      return dso->syms + i;
    if (h2 & 1)
      break;
  }

  return 0;
}

LIBC_NO_SAFESTACK NO_ASAN static Sym* gnu_lookup_filtered(uint32_t h1, uint32_t* hashtab,
                                                          struct dso* dso, const char* s,
                                                          uint32_t fofs, size_t fmask) {
  const size_t* bloomwords = (const void*)(hashtab + 4);
  size_t f = bloomwords[fofs & (hashtab[2] - 1)];
  if (!(f & fmask))
    return 0;

  f >>= (h1 >> hashtab[3]) % (8 * sizeof f);
  if (!(f & 1))
    return 0;

  return gnu_lookup(h1, hashtab, dso, s);
}

#define OK_TYPES \
  (1 << STT_NOTYPE | 1 << STT_OBJECT | 1 << STT_FUNC | 1 << STT_COMMON | 1 << STT_TLS)
#define OK_BINDS (1 << STB_GLOBAL | 1 << STB_WEAK | 1 << STB_GNU_UNIQUE)

LIBC_NO_SAFESTACK NO_ASAN static struct symdef find_sym(struct dso* dso, const char* s,
                                                        int need_def) {
  uint32_t h = 0, gh = 0, gho = 0, *ght;
  size_t ghm = 0;
  struct symdef def = {};
  for (; dso; dso = dso_next(dso)) {
    Sym* sym;
    if (!dso->global)
      continue;
    if ((ght = dso->ghashtab)) {
      if (!ghm) {
        gh = gnu_hash(s);
        int maskbits = 8 * sizeof ghm;
        gho = gh / maskbits;
        ghm = 1ul << gh % maskbits;
      }
      sym = gnu_lookup_filtered(gh, ght, dso, s, gho, ghm);
    } else {
      if (!h)
        h = sysv_hash(s);
      sym = sysv_lookup(s, h, dso);
    }
    if (!sym)
      continue;
    if (!sym->st_shndx)
      if (need_def || (sym->st_info & 0xf) == STT_TLS)
        continue;
    if (!sym->st_value)
      if ((sym->st_info & 0xf) != STT_TLS)
        continue;
    if (!(1 << (sym->st_info & 0xf) & OK_TYPES))
      continue;
    if (!(1 << (sym->st_info >> 4) & OK_BINDS))
      continue;

    if (def.sym && sym->st_info >> 4 == STB_WEAK)
      continue;
    def.sym = sym;
    def.dso = dso;
    if (sym->st_info >> 4 == STB_GLOBAL)
      break;
  }
  return def;
}

__attribute__((__visibility__("hidden"))) ptrdiff_t __tlsdesc_static(void), __tlsdesc_dynamic(void);

LIBC_NO_SAFESTACK NO_ASAN static void do_relocs(struct dso* dso, size_t* rel, size_t rel_size,
                                                size_t stride) {
  ElfW(Addr) base = dso->l_map.l_addr;
  Sym* syms = dso->syms;
  char* strings = dso->strings;
  Sym* sym;
  const char* name;
  void* ctx;
  int type;
  int sym_index;
  struct symdef def;
  size_t* reloc_addr;
  size_t sym_val;
  size_t tls_val;
  size_t addend;
  int skip_relative = 0, reuse_addends = 0, save_slot = 0;

  if (dso == &ldso) {
    /* Only ldso's REL table needs addend saving/reuse. */
    if (rel == apply_addends_to)
      reuse_addends = 1;
    skip_relative = 1;
  }

  for (; rel_size; rel += stride, rel_size -= stride * sizeof(size_t)) {
    if (skip_relative && R_TYPE(rel[1]) == REL_RELATIVE)
      continue;
    type = R_TYPE(rel[1]);
    if (type == REL_NONE)
      continue;
    sym_index = R_SYM(rel[1]);
    reloc_addr = laddr(dso, rel[0]);
    if (sym_index) {
      sym = syms + sym_index;
      name = strings + sym->st_name;
      ctx = type == REL_COPY ? dso_next(head) : head;
      def = (sym->st_info & 0xf) == STT_SECTION ? (struct symdef){.dso = dso, .sym = sym}
                                                : find_sym(ctx, name, type == REL_PLT);
      if (!def.sym && (sym->st_shndx != SHN_UNDEF || sym->st_info >> 4 != STB_WEAK)) {
        error("Error relocating %s: %s: symbol not found", dso->l_map.l_name, name);
        if (runtime)
          longjmp(*rtld_fail, 1);
        continue;
      }
    } else {
      name = "(local)";
      sym = 0;
      def.sym = 0;
      def.dso = dso;
    }

    if (stride > 2) {
      addend = rel[2];
    } else if (type == REL_GOT || type == REL_PLT || type == REL_COPY) {
      addend = 0;
    } else if (reuse_addends) {
      /* Save original addend in stage 2 where the dso
       * chain consists of just ldso; otherwise read back
       * saved addend since the inline one was clobbered. */
      if (head == &ldso)
        saved_addends[save_slot] = *reloc_addr;
      addend = saved_addends[save_slot++];
    } else {
      addend = *reloc_addr;
    }

    sym_val = def.sym ? saddr(def.dso, def.sym->st_value) : 0;
    tls_val = def.sym ? def.sym->st_value : 0;

    switch (type) {
      case REL_NONE:
        break;
      case REL_OFFSET:
        addend -= (size_t)reloc_addr;
      case REL_SYMBOLIC:
#if REL_GOT != REL_SYMBOLIC
      case REL_GOT:
#endif
      case REL_PLT:
        *reloc_addr = sym_val + addend;
        break;
      case REL_RELATIVE:
        *reloc_addr = base + addend;
        break;
      case REL_COPY:
        memcpy(reloc_addr, (void*)sym_val, sym->st_size);
        break;
      case REL_OFFSET32:
        *(uint32_t*)reloc_addr = sym_val + addend - (size_t)reloc_addr;
        break;
      case REL_FUNCDESC:
        *reloc_addr = def.sym ? (size_t)(def.dso->funcdescs + (def.sym - def.dso->syms)) : 0;
        break;
      case REL_FUNCDESC_VAL:
        if ((sym->st_info & 0xf) == STT_SECTION)
          *reloc_addr += sym_val;
        else
          *reloc_addr = sym_val;
        reloc_addr[1] = def.sym ? (size_t)def.dso->got : 0;
        break;
      case REL_DTPMOD:
        *reloc_addr = def.dso->tls_id;
        break;
      case REL_DTPOFF:
        *reloc_addr = tls_val + addend - DTP_OFFSET;
        break;
#ifdef TLS_ABOVE_TP
      case REL_TPOFF:
        *reloc_addr = tls_val + def.dso->tls.offset + addend;
        break;
#else
      case REL_TPOFF:
        *reloc_addr = tls_val - def.dso->tls.offset + addend;
        break;
      case REL_TPOFF_NEG:
        *reloc_addr = def.dso->tls.offset - tls_val + addend;
        break;
#endif
      case REL_TLSDESC:
        if (stride < 3)
          addend = reloc_addr[1];
        if (runtime && def.dso->tls_id >= static_tls_cnt) {
#if !TLSDESC_DIRECT
          size_t* new = dl_alloc(2 * sizeof(size_t));
          if (!new) {
            error("Error relocating %s: cannot allocate TLSDESC for %s", dso->l_map.l_name, name);
            longjmp(*rtld_fail, 1);
          }
          new[0] = def.dso->tls_id;
          new[1] = tls_val + addend;
          reloc_addr[1] = (size_t) new;
#else
          reloc_addr[1] = tls_val + addend;
          reloc_addr[2] = def.dso->tls_id;
#endif
          reloc_addr[0] = (size_t)__tlsdesc_dynamic;
        } else {
          reloc_addr[0] = (size_t)__tlsdesc_static;
#ifdef TLS_ABOVE_TP
          reloc_addr[1] = tls_val + def.dso->tls.offset + addend;
#else
          reloc_addr[1] = tls_val - def.dso->tls.offset + addend;
#endif
        }
        break;
      default:
        error("Error relocating %s: unsupported relocation type %d", dso->l_map.l_name, type);
        if (runtime)
          longjmp(*rtld_fail, 1);
        continue;
    }
  }
}

LIBC_NO_SAFESTACK static void unmap_library(struct dso* dso) {
  if (dso->map && dso->map_len) {
    munmap(dso->map, dso->map_len);
  }
  if (dso->vmar != ZX_HANDLE_INVALID) {
    _zx_vmar_destroy(dso->vmar);
    _zx_handle_close(dso->vmar);
    dso->vmar = ZX_HANDLE_INVALID;
  }
}

// app.module_id is always zero, so assignments start with 1.
LIBC_NO_SAFESTACK NO_ASAN static void assign_module_id(struct dso* dso) {
  static unsigned int last_module_id;
  dso->module_id = ++last_module_id;
}

// Locate the build ID note just after mapping the segments in.
// This is called from dls2, so it cannot use any non-static functions.
LIBC_NO_SAFESTACK NO_ASAN static bool find_buildid_note(struct dso* dso, const Phdr* seg) {
  const char* end = laddr(dso, seg->p_vaddr + seg->p_filesz);
  for (const struct gnu_note* n = laddr(dso, seg->p_vaddr); (const char*)n < end;
       n = (const void*)((const char*)&n->name + ((n->nhdr.n_namesz + 3) & -4) +
                         ((n->nhdr.n_descsz + 3) & -4))) {
    if (n->nhdr.n_type == NT_GNU_BUILD_ID && n->nhdr.n_namesz == sizeof(GNU_NOTE_NAME) &&
        n->name.word == GNU_NOTE_NAME.word) {
      dso->build_id_note = n;
      return true;
    }
  }
  return false;
}

// Format the markup elements by hand to avoid using large and complex code
// like the printf engine.

LIBC_NO_SAFESTACK static char* format_string(char* p, const char* string, size_t len) {
  return memcpy(p, string, len) + len;
}

#define FORMAT_HEX_VALUE_SIZE (2 + (sizeof(uint64_t) * 2))
#define HEXDIGITS "0123456789abcdef"

LIBC_NO_SAFESTACK static char* format_hex_value(char buffer[FORMAT_HEX_VALUE_SIZE],
                                                uint64_t value) {
  char* p = buffer;
  if (value == 0) {
    // No "0x" prefix on zero.
    *p++ = '0';
  } else {
    *p++ = '0';
    *p++ = 'x';
    // Skip the high nybbles that are zero.
    int shift = 60;
    while ((value >> shift) == 0) {
      shift -= 4;
    }
    do {
      *p++ = HEXDIGITS[(value >> shift) & 0xf];
      shift -= 4;
    } while (shift >= 0);
  }
  return p;
}

LIBC_NO_SAFESTACK static char* format_hex_string(char* p, const uint8_t* string, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    uint8_t byte = string[i];
    *p++ = HEXDIGITS[byte >> 4];
    *p++ = HEXDIGITS[byte & 0xf];
  }
  return p;
}

// The format theoretically does not constrain the size of build ID notes,
// but there is a reasonable upper bound.
#define MAX_BUILD_ID_SIZE 64

// Likewise, there's no real limit on the length of module names.
// But they're only included in the markup output to be informative,
// so truncating them is OK.
#define MODULE_NAME_SIZE 64

#define MODULE_ELEMENT_BEGIN "{{{module:"
#define MODULE_ELEMENT_BUILD_ID_BEGIN ":elf:"
#define MODULE_ELEMENT_END "}}}\n"
#define MODULE_ELEMENT_SIZE                                                          \
  (sizeof(MODULE_ELEMENT_BEGIN) - 1 + FORMAT_HEX_VALUE_SIZE + 1 + MODULE_NAME_SIZE + \
   sizeof(MODULE_ELEMENT_BUILD_ID_BEGIN) - 1 + (MAX_BUILD_ID_SIZE * 2) + 1 +         \
   sizeof(MODULE_ELEMENT_END))

LIBC_NO_SAFESTACK static void log_module_element(struct dso* dso) {
  char buffer[MODULE_ELEMENT_SIZE];
  char* p = format_string(buffer, MODULE_ELEMENT_BEGIN, sizeof(MODULE_ELEMENT_BEGIN) - 1);
  p = format_hex_value(p, dso->module_id);
  *p++ = ':';
  const char* name = dso->l_map.l_name;
  if (name[0] == '\0') {
    name = dso->soname == NULL ? "<application>" : dso->soname;
  }
  size_t namelen = strlen(name);
  if (namelen > MODULE_NAME_SIZE) {
    namelen = MODULE_NAME_SIZE;
  }
  p = format_string(p, name, namelen);
  p = format_string(p, MODULE_ELEMENT_BUILD_ID_BEGIN, sizeof(MODULE_ELEMENT_BUILD_ID_BEGIN) - 1);
  if (dso->build_id_note) {
    p = format_hex_string(p, dso->build_id_note->desc, dso->build_id_note->nhdr.n_descsz);
  }
  p = format_string(p, MODULE_ELEMENT_END, sizeof(MODULE_ELEMENT_END) - 1);
  _dl_log_write(buffer, p - buffer);
}

#define MMAP_ELEMENT_BEGIN "{{{mmap:"
#define MMAP_ELEMENT_LOAD_BEGIN ":load:"
#define MMAP_ELEMENT_END "}}}\n"
#define MMAP_ELEMENT_SIZE                                                                   \
  (sizeof(MMAP_ELEMENT_BEGIN) - 1 + FORMAT_HEX_VALUE_SIZE + 1 + FORMAT_HEX_VALUE_SIZE + 1 + \
   sizeof(MMAP_ELEMENT_LOAD_BEGIN) - 1 + FORMAT_HEX_VALUE_SIZE + 1 + 3 + 1 +                \
   FORMAT_HEX_VALUE_SIZE)

LIBC_NO_SAFESTACK static void log_mmap_element(struct dso* dso, const Phdr* ph) {
  size_t start = ph->p_vaddr & -PAGE_SIZE;
  size_t end = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & -PAGE_SIZE;
  char buffer[MMAP_ELEMENT_SIZE];
  char* p = format_string(buffer, MMAP_ELEMENT_BEGIN, sizeof(MMAP_ELEMENT_BEGIN) - 1);
  p = format_hex_value(p, saddr(dso, start));
  *p++ = ':';
  p = format_hex_value(p, end - start);
  p = format_string(p, MMAP_ELEMENT_LOAD_BEGIN, sizeof(MMAP_ELEMENT_LOAD_BEGIN) - 1);
  p = format_hex_value(p, dso->module_id);
  *p++ = ':';
  if (ph->p_flags & PF_R) {
    *p++ = 'r';
  }
  if (ph->p_flags & PF_W) {
    *p++ = 'w';
  }
  if (ph->p_flags & PF_X) {
    *p++ = 'x';
  }
  *p++ = ':';
  p = format_hex_value(p, start);
  p = format_string(p, MMAP_ELEMENT_END, sizeof(MMAP_ELEMENT_END) - 1);
  _dl_log_write(buffer, p - buffer);
}

// No newline because it's immediately followed by a {{{module:...}}}.
#define RESET_ELEMENT "{{{reset}}}"

LIBC_NO_SAFESTACK static void log_dso(struct dso* dso) {
  if (dso == head) {
    // Write the reset element before the first thing listed.
    _dl_log_write(RESET_ELEMENT, sizeof(RESET_ELEMENT) - 1);
  }
  log_module_element(dso);
  if (dso->phdr) {
    for (unsigned int i = 0; i < dso->phnum; ++i) {
      if (dso->phdr[i].p_type == PT_LOAD) {
        log_mmap_element(dso, &dso->phdr[i]);
      }
    }
  }
}

LIBC_NO_SAFESTACK void _dl_log_unlogged(void) {
  // The first thread to successfully swap in 0 and get an old value
  // for unlogged_tail is responsible for logging all the unlogged
  // DSOs up through that pointer.  If dlopen calls move the tail
  // and another thread then calls into here, we can race with that
  // thread.  So we use a separate atomic_flag on each 'struct dso'
  // to ensure only one thread prints each one.
  uintptr_t last_unlogged = atomic_load_explicit(&unlogged_tail, memory_order_acquire);
  do {
    if (last_unlogged == 0)
      return;
  } while (!atomic_compare_exchange_weak_explicit(&unlogged_tail, &last_unlogged, 0,
                                                  memory_order_acq_rel, memory_order_relaxed));
  for (struct dso* p = head; true; p = dso_next(p)) {
    if (!atomic_flag_test_and_set_explicit(&p->logged, memory_order_relaxed)) {
      log_dso(p);
    }
    if ((struct dso*)last_unlogged == p) {
      break;
    }
  }
}

LIBC_NO_SAFESTACK NO_ASAN static zx_status_t map_library(zx_handle_t vmo, struct dso* dso) {
  struct {
    Ehdr ehdr;
    // A typical ELF file has 7 or 8 phdrs, so in practice
    // this is always enough.  Life is simpler if there is no
    // need for dynamic allocation here.
    Phdr phdrs[16];
  } buf;
  size_t phsize;
  size_t addr_min = SIZE_MAX, addr_max = 0, map_len;
  size_t this_min, this_max;
  const Ehdr* const eh = &buf.ehdr;
  Phdr *ph, *ph0;
  unsigned char *map = MAP_FAILED, *base;
  size_t dyn = 0;
  size_t tls_image = 0;
  size_t i;

  size_t l;
  zx_status_t status = _zx_vmo_get_size(vmo, &l);
  if (status != ZX_OK)
    return status;
  status = _zx_vmo_read(vmo, &buf, 0, sizeof(buf) < l ? sizeof(buf) : l);
  if (status != ZX_OK)
    return status;
  // We cannot support ET_EXEC in the general case, because its fixed
  // addresses might conflict with where the dynamic linker has already
  // been loaded.  It's also policy in Fuchsia that all executables are
  // PIEs to maximize ASLR security benefits.  So don't even try to
  // handle loading ET_EXEC.
  if (l < sizeof *eh || eh->e_type != ET_DYN)
    goto noexec;
  phsize = eh->e_phentsize * eh->e_phnum;
  if (phsize > sizeof(buf.phdrs))
    goto noexec;
  if (eh->e_phoff + phsize > l) {
    status = _zx_vmo_read(vmo, buf.phdrs, eh->e_phoff, phsize);
    if (status != ZX_OK)
      goto error;
    ph = ph0 = buf.phdrs;
  } else {
    ph = ph0 = (void*)((char*)&buf + eh->e_phoff);
  }
  const Phdr* first_note = NULL;
  const Phdr* last_note = NULL;
  for (i = eh->e_phnum; i; i--, ph = (void*)((char*)ph + eh->e_phentsize)) {
    switch (ph->p_type) {
      case PT_LOAD:
        if (ph->p_vaddr < addr_min) {
          addr_min = ph->p_vaddr;
        }
        if (ph->p_vaddr + ph->p_memsz > addr_max) {
          addr_max = ph->p_vaddr + ph->p_memsz;
        }
        if (ph->p_flags & PF_X) {
          dso->code_start = addr_min;
          dso->code_end = addr_max;
        }
        break;
      case PT_DYNAMIC:
        dyn = ph->p_vaddr;
        break;
      case PT_TLS:
        tls_image = ph->p_vaddr;
        dso->tls.align = ph->p_align;
        dso->tls.len = ph->p_filesz;
        dso->tls.size = ph->p_memsz;
        break;
      case PT_GNU_RELRO:
        dso->relro_start = ph->p_vaddr;
        dso->relro_end = ph->p_vaddr + ph->p_memsz;
        break;
      case PT_NOTE:
        if (first_note == NULL)
          first_note = ph;
        last_note = ph;
        break;
      case PT_GNU_STACK:
        if (ph->p_flags & PF_X) {
          error(
              "%s requires executable stack"
              " (built with -z execstack?),"
              " which Fuchsia will never support",
              dso->soname == NULL ? dso->l_map.l_name : dso->soname);
          goto noexec;
        }
        break;
    }
  }
  if (!dyn)
    goto noexec;
  addr_max += PAGE_SIZE - 1;
  addr_max &= -PAGE_SIZE;
  addr_min &= -PAGE_SIZE;
  map_len = addr_max - addr_min;

  // Allocate a VMAR to reserve the whole address range.  Stash
  // the new VMAR's handle until relocation has finished, because
  // we need it to adjust page protections for RELRO.
  uintptr_t vmar_base;
  status = _zx_vmar_allocate(
      __zircon_vmar_root_self,
      ZX_VM_CAN_MAP_READ | ZX_VM_CAN_MAP_WRITE | ZX_VM_CAN_MAP_EXECUTE | ZX_VM_CAN_MAP_SPECIFIC, 0,
      map_len, &dso->vmar, &vmar_base);
  if (status != ZX_OK) {
    error("failed to reserve %zu bytes of address space: %d\n", map_len, status);
    goto error;
  }

  char vmo_name[ZX_MAX_NAME_LEN];
  if (_zx_object_get_property(vmo, ZX_PROP_NAME, vmo_name, sizeof(vmo_name)) != ZX_OK ||
      vmo_name[0] == '\0')
    memcpy(vmo_name, VMO_NAME_UNKNOWN, sizeof(VMO_NAME_UNKNOWN));

  dso->map = map = (void*)vmar_base;
  dso->map_len = map_len;
  base = map - addr_min;
  dso->phdr = 0;
  dso->phnum = 0;
  uint_fast16_t nbss = 0, ndata = 0;
  for (ph = ph0, i = eh->e_phnum; i; i--, ph = (void*)((char*)ph + eh->e_phentsize)) {
    if (ph->p_type != PT_LOAD)
      continue;
    /* Check if the programs headers are in this load segment, and
     * if so, record the address for use by dl_iterate_phdr. */
    if (!dso->phdr && eh->e_phoff >= ph->p_offset &&
        eh->e_phoff + phsize <= ph->p_offset + ph->p_filesz) {
      dso->phdr = (void*)(base + ph->p_vaddr + (eh->e_phoff - ph->p_offset));
      dso->phnum = eh->e_phnum;
      dso->phentsize = eh->e_phentsize;
    }
    this_min = ph->p_vaddr & -PAGE_SIZE;
    this_max = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & -PAGE_SIZE;
    size_t off_start = ph->p_offset & -PAGE_SIZE;
    zx_vm_option_t zx_options = ZX_VM_SPECIFIC | ZX_VM_ALLOW_FAULTS;
    zx_options |= (ph->p_flags & PF_R) ? ZX_VM_PERM_READ : 0;
    zx_options |= (ph->p_flags & PF_W) ? ZX_VM_PERM_WRITE : 0;
    const zx_vm_option_t exec_perm = ZX_VM_PERM_EXECUTE | ZX_VM_PERM_READ_IF_XOM_UNSUPPORTED;
    zx_options |= (ph->p_flags & PF_X) ? exec_perm : 0;
    uintptr_t mapaddr = (uintptr_t)base + this_min;
    zx_handle_t map_vmo = vmo;
    size_t map_size = this_max - this_min;
    if (map_size == 0)
      continue;

    if (ph->p_flags & PF_W) {
      size_t data_size = ((ph->p_vaddr + ph->p_filesz + PAGE_SIZE - 1) & -PAGE_SIZE) - this_min;
      if (data_size == 0) {
        // This segment is purely zero-fill.
        status = _zx_vmo_create(map_size, 0, &map_vmo);
        if (status == ZX_OK) {
          char name[ZX_MAX_NAME_LEN] = VMO_NAME_PREFIX_BSS;
          memcpy(&name[sizeof(VMO_NAME_PREFIX_BSS) - 1], vmo_name,
                 ZX_MAX_NAME_LEN - sizeof(VMO_NAME_PREFIX_BSS));
          // Replace the N with a digit for how many bssN's there have been.
          name[sizeof(VMO_NAME_PREFIX_BSS) - 3] = "0123456789abcdef"[nbss++];
          _zx_object_set_property(map_vmo, ZX_PROP_NAME, name, strlen(name));
        }
      } else {
        // Get a writable (lazy) copy of the portion of the file VMO.
        status = _zx_vmo_create_child(
            vmo, ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE | ZX_VMO_CHILD_RESIZABLE, off_start,
            data_size, &map_vmo);
        if (status == ZX_OK && map_size > data_size) {
          // Extend the writable VMO to cover the .bss pages too.
          // These pages will be zero-filled, not copied from the
          // file VMO.
          status = _zx_vmo_set_size(map_vmo, map_size);
          if (status != ZX_OK) {
            _zx_handle_close(map_vmo);
            goto error;
          }
        }
        if (status == ZX_OK) {
          char name[ZX_MAX_NAME_LEN] = VMO_NAME_PREFIX_DATA;
          memcpy(&name[sizeof(VMO_NAME_PREFIX_DATA) - 1], vmo_name,
                 ZX_MAX_NAME_LEN - sizeof(VMO_NAME_PREFIX_DATA));
          if (ph->p_vaddr >= dso->relro_start && ph->p_vaddr + ph->p_memsz <= dso->relro_end) {
            // Make "data1" be "relro" instead when the RELRO region covers
            // the entire segment.
            static_assert(sizeof(VMO_NAME_PREFIX_DATA) == sizeof(VMO_NAME_PREFIX_RELRO), "");
            memcpy(name, VMO_NAME_PREFIX_RELRO, sizeof(VMO_NAME_PREFIX_RELRO) - 1);
          } else {
            // Replace the N with a digit for how many dataN's.
            name[sizeof(VMO_NAME_PREFIX_DATA) - 3] = "0123456789abcdef"[ndata++];
          }
          _zx_object_set_property(map_vmo, ZX_PROP_NAME, name, strlen(name));
        }
      }
      if (status != ZX_OK)
        goto error;
      off_start = 0;
    } else if (ph->p_memsz > ph->p_filesz) {
      // Read-only .bss is not a thing.
      goto noexec;
    }

    status = _zx_vmar_map(dso->vmar, zx_options, mapaddr - vmar_base, map_vmo, off_start, map_size,
                          &mapaddr);
    if (map_vmo != vmo)
      _zx_handle_close(map_vmo);
    if (status != ZX_OK)
      goto error;

    if (ph->p_memsz > ph->p_filesz) {
      // The final partial page of data from the file is followed by
      // whatever the file's contents there are, but in the memory
      // image that partial page should be all zero.
      const uintptr_t file_end = (uintptr_t)base + ph->p_vaddr + ph->p_filesz;
      const uintptr_t partial_page_offset = file_end & (PAGE_SIZE - 1);
      if (partial_page_offset) {
        memset((void*)file_end, 0, PAGE_SIZE - partial_page_offset);
      }
    }
  }

  dso->l_map.l_addr = (uintptr_t)base;
  dso->l_map.l_ld = laddr(dso, dyn);
  if (dso->tls.size)
    dso->tls.image = laddr(dso, tls_image);

  if (first_note != NULL) {
    for (const Phdr* seg = first_note; seg <= last_note; ++seg) {
      if (seg->p_type == PT_NOTE && find_buildid_note(dso, seg))
        break;
    }
  }

  return ZX_OK;
noexec:
  // We overload this to translate into ENOEXEC later.
  status = ZX_ERR_WRONG_TYPE;
error:
  if (map != MAP_FAILED)
    unmap_library(dso);
  if (dso->vmar != ZX_HANDLE_INVALID && !KEEP_DSO_VMAR)
    _zx_handle_close(dso->vmar);
  dso->vmar = ZX_HANDLE_INVALID;
  return status;
}

LIBC_NO_SAFESTACK NO_ASAN static void decode_dyn(struct dso* p) {
  size_t dyn[DT_NUM];
  decode_vec(p->l_map.l_ld, dyn, DT_NUM);
  p->syms = laddr(p, dyn[DT_SYMTAB]);
  p->strings = laddr(p, dyn[DT_STRTAB]);
  if (dyn[0] & (1 << DT_SONAME))
    p->soname = p->strings + dyn[DT_SONAME];
  if (dyn[0] & (1 << DT_HASH))
    p->hashtab = laddr(p, dyn[DT_HASH]);
  if (dyn[0] & (1 << DT_PLTGOT))
    p->got = laddr(p, dyn[DT_PLTGOT]);
  if (search_vec(p->l_map.l_ld, dyn, DT_GNU_HASH))
    p->ghashtab = laddr(p, *dyn);
  if (search_vec(p->l_map.l_ld, dyn, DT_VERSYM))
    p->versym = laddr(p, *dyn);
}

LIBC_NO_SAFESTACK static size_t count_syms(struct dso* p) {
  if (p->hashtab)
    return p->hashtab[1];

  size_t nsym, i;
  uint32_t* buckets = p->ghashtab + 4 + (p->ghashtab[2] * sizeof(size_t) / 4);
  uint32_t* hashval;
  for (i = nsym = 0; i < p->ghashtab[0]; i++) {
    if (buckets[i] > nsym)
      nsym = buckets[i];
  }
  if (nsym) {
    hashval = buckets + p->ghashtab[0] + (nsym - p->ghashtab[1]);
    do
      nsym++;
    while (!(*hashval++ & 1));
  }
  return nsym;
}

LIBC_NO_SAFESTACK static struct dso* find_library_in(struct dso* p, const char* name) {
  while (p != NULL) {
    if (!strcmp(p->l_map.l_name, name) || (p->soname != NULL && !strcmp(p->soname, name))) {
      ++p->refcnt;
      break;
    }
    p = dso_next(p);
  }
  return p;
}

LIBC_NO_SAFESTACK static struct dso* find_library(const char* name) {
  // First see if it's in the general list.
  struct dso* p = find_library_in(head, name);
  if (p == NULL && detached_head != NULL) {
    // ldso is not in the list yet, so the first search didn't notice
    // anything that is only a dependency of ldso, i.e. the vDSO.
    // See if the lookup by name matches ldso or its dependencies.
    p = find_library_in(detached_head, name);
    if (p == &ldso) {
      // If something depends on libc (&ldso), we actually want
      // to pull in the entire detached list in its existing
      // order (&ldso is always last), so that libc stays after
      // its own dependencies.
      dso_set_prev(detached_head, tail);
      dso_set_next(tail, detached_head);
      tail = p;
      detached_head = NULL;
    } else if (p != NULL) {
      // Take it out of its place in the list rooted at detached_head.
      if (dso_prev(p) != NULL)
        dso_set_next(dso_prev(p), dso_next(p));
      else
        detached_head = dso_next(p);
      if (dso_next(p) != NULL) {
        dso_set_prev(dso_next(p), dso_prev(p));
        dso_set_next(p, NULL);
      }
      // Stick it on the main list.
      dso_set_next(tail, p);
      dso_set_prev(p, tail);
      tail = p;
    }
  }
  return p;
}

#define MAX_BUILDID_SIZE 64

LIBC_NO_SAFESTACK static void trace_load(struct dso* p) {
  static zx_koid_t pid = ZX_KOID_INVALID;
  if (pid == ZX_KOID_INVALID) {
    zx_info_handle_basic_t process_info;
    if (_zx_object_get_info(__zircon_process_self, ZX_INFO_HANDLE_BASIC, &process_info,
                            sizeof(process_info), NULL, NULL) == ZX_OK) {
      pid = process_info.koid;
    } else {
      // No point in continually calling zx_object_get_info.
      // The first 100 are reserved.
      pid = 1;
    }
  }

  // Compute extra values useful to tools.
  // This is done here so that it's only done when necessary.
  char buildid[MAX_BUILDID_SIZE * 2 + 1];
  if (p->build_id_note) {
    if (p->build_id_note->nhdr.n_descsz > MAX_BUILD_ID_SIZE) {
      snprintf(buildid, sizeof(buildid), "build_id_too_large_%u", p->build_id_note->nhdr.n_descsz);
    } else {
      char* end =
          format_hex_string(buildid, p->build_id_note->desc, p->build_id_note->nhdr.n_descsz);
      *end = '\0';
    }
  } else {
    strcpy(buildid, "<none>");
  }

  const char* name = p->soname == NULL ? "<application>" : p->l_map.l_name;
  const char* soname = p->soname == NULL ? "<application>" : p->soname;

  // The output is in multiple lines to cope with damn line wrapping.
  // N.B. Programs like the Intel Processor Trace decoder parse this output.
  // Do not change without coordination with consumers.
  // TODO(fxbug.dev/30479): Switch to official tracing mechanism when ready.
  static int seqno;
  debugmsg("@trace_load: %" PRIu64 ":%da %p %p %p", pid, seqno, p->l_map.l_addr, p->map,
           p->map + p->map_len);
  debugmsg("@trace_load: %" PRIu64 ":%db %s", pid, seqno, buildid);
  debugmsg("@trace_load: %" PRIu64 ":%dc %s %s", pid, seqno, soname, name);
  ++seqno;
}

LIBC_NO_SAFESTACK static void do_tls_layout(struct dso* p, char* tls_buffer, int n_th) {
  if (p->tls.size == 0)
    return;

  p->tls_id = ++tls_cnt;
  tls_align = MAXP2(tls_align, p->tls.align);
#ifdef TLS_ABOVE_TP
  p->tls.offset = (tls_offset + p->tls.align - 1) & -p->tls.align;
  tls_offset = p->tls.offset + p->tls.size;
#else
  tls_offset += p->tls.size + p->tls.align - 1;
  tls_offset -= (tls_offset + (uintptr_t)p->tls.image) & (p->tls.align - 1);
  p->tls.offset = tls_offset;
#endif

  if (tls_buffer != NULL) {
    p->new_dtv = (void*)(-sizeof(size_t) & (uintptr_t)(tls_buffer + sizeof(size_t)));
    p->new_tls = (void*)(p->new_dtv + n_th * (tls_cnt + 1));
  }

  if (tls_tail)
    tls_tail->next = &p->tls;
  else
    libc.tls_head = &p->tls;
  tls_tail = &p->tls;
}

LIBC_NO_SAFESTACK static zx_status_t load_library_vmo(zx_handle_t vmo, const char* name,
                                                      int rtld_mode, struct dso* needed_by,
                                                      struct dso** loaded) {
  struct dso *p, temp_dso = {};
  size_t alloc_size;
  int n_th = 0;

  if (rtld_mode & RTLD_NOLOAD) {
    *loaded = NULL;
    return ZX_OK;
  }

  zx_status_t status = map_library(vmo, &temp_dso);
  if (status != ZX_OK)
    return status;

  decode_dyn(&temp_dso);
  if (temp_dso.soname != NULL) {
    // Now check again if we opened the same file a second time.
    // That is, a file with the same DT_SONAME string.
    p = find_library(temp_dso.soname);
    if (p != NULL) {
      unmap_library(&temp_dso);
      *loaded = p;
      return ZX_OK;
    }
  }

  // If this was loaded by VMO rather than by name, we have to synthesize
  // one.  If the SONAME if present.  Otherwise synthesize something
  // informative from the VMO (that won't look like any sensible SONAME).
  char synthetic_name[ZX_MAX_NAME_LEN + 32];
  if (name == NULL)
    name = temp_dso.soname;
  if (name == NULL) {
    char vmo_name[ZX_MAX_NAME_LEN];
    if (_zx_object_get_property(vmo, ZX_PROP_NAME, vmo_name, sizeof(vmo_name)) != ZX_OK)
      vmo_name[0] = '\0';
    zx_info_handle_basic_t info;
    if (_zx_object_get_info(vmo, ZX_INFO_HANDLE_BASIC, &info, sizeof(info), NULL, NULL) != ZX_OK) {
      name = "<dlopen_vmo>";
    } else {
      if (vmo_name[0] == '\0') {
        snprintf(synthetic_name, sizeof(synthetic_name), "<VMO#%" PRIu64 ">", info.koid);
      } else {
        snprintf(synthetic_name, sizeof(synthetic_name), "<VMO#%" PRIu64 "=%s>", info.koid,
                 vmo_name);
      }
      name = synthetic_name;
    }
  }

  // Calculate how many slots are needed for dependencies.
  size_t ndeps = 1;  // Account for a NULL terminator.
  for (size_t i = 0; temp_dso.l_map.l_ld[i].d_tag; i++) {
    if (temp_dso.l_map.l_ld[i].d_tag == DT_NEEDED)
      ++ndeps;
  }

  /* Allocate storage for the new DSO. When there is TLS, this
   * storage must include a reservation for all pre-existing
   * threads to obtain copies of both the new TLS, and an
   * extended DTV capable of storing an additional slot for
   * the newly-loaded DSO. */
  size_t namelen = strlen(name);
  alloc_size = (sizeof *p + ndeps * sizeof(p->deps[0]) + namelen + 1);
  if (runtime && temp_dso.tls.image) {
    size_t per_th = temp_dso.tls.size + temp_dso.tls.align + sizeof(void*) * (tls_cnt + 3);
    n_th = atomic_load(&libc.thread_count);
    if (n_th > SSIZE_MAX / per_th)
      alloc_size = SIZE_MAX;
    else
      alloc_size += n_th * per_th;
  }
  p = dl_alloc(alloc_size);
  if (!p) {
    unmap_library(&temp_dso);
    return ZX_ERR_NO_MEMORY;
  }
  *p = temp_dso;
  p->refcnt = 1;
  p->needed_by = needed_by;
  p->l_map.l_name = (void*)&p->buf[ndeps];
  memcpy(p->l_map.l_name, name, namelen);
  p->l_map.l_name[namelen] = '\0';
  assign_module_id(p);
  if (runtime)
    do_tls_layout(p, p->l_map.l_name + namelen + 1, n_th);

  dso_set_next(tail, p);
  dso_set_prev(p, tail);
  tail = p;

  *loaded = p;
  return ZX_OK;
}

LIBC_NO_SAFESTACK static zx_status_t load_library(const char* name, int rtld_mode,
                                                  struct dso* needed_by, struct dso** loaded) {
  if (!*name)
    return ZX_ERR_INVALID_ARGS;

  *loaded = find_library(name);
  if (*loaded != NULL)
    return ZX_OK;

  zx_handle_t vmo;
  zx_status_t status = get_library_vmo(name, &vmo);
  if (status == ZX_OK) {
    status = load_library_vmo(vmo, name, rtld_mode, needed_by, loaded);
    _zx_handle_close(vmo);
  }

  return status;
}

LIBC_NO_SAFESTACK static void load_deps(struct dso* p) {
  for (; p; p = dso_next(p)) {
    struct dso** deps = NULL;
    // The two preallocated DSOs don't get space allocated for ->deps.
    if (runtime && p->deps == NULL && p != &ldso && p != &vdso)
      deps = p->deps = p->buf;
    for (size_t i = 0; p->l_map.l_ld[i].d_tag; i++) {
      if (p->l_map.l_ld[i].d_tag != DT_NEEDED)
        continue;
      const char* name = p->strings + p->l_map.l_ld[i].d_un.d_val;
      struct dso* dep;
      zx_status_t status = load_library(name, 0, p, &dep);
      if (status != ZX_OK) {
        error("Error loading shared library %s: %s (needed by %s)", name,
              _zx_status_get_string(status), p->l_map.l_name);
        if (runtime)
          longjmp(*rtld_fail, 1);
      } else if (deps != NULL) {
        *deps++ = dep;
      }
    }
  }
}

LIBC_NO_SAFESTACK NO_ASAN static void reloc_all(struct dso* p) {
  size_t dyn[DT_NUM];
  for (; p; p = dso_next(p)) {
    if (p->relocated)
      continue;
    decode_vec(p->l_map.l_ld, dyn, DT_NUM);
    // _dl_start did apply_relr already.
    if (p != &ldso) {
      apply_relr(p->l_map.l_addr, laddr(p, dyn[DT_RELR]), dyn[DT_RELRSZ]);
    }
    do_relocs(p, laddr(p, dyn[DT_JMPREL]), dyn[DT_PLTRELSZ], 2 + (dyn[DT_PLTREL] == DT_RELA));
    do_relocs(p, laddr(p, dyn[DT_REL]), dyn[DT_RELSZ], 2);
    do_relocs(p, laddr(p, dyn[DT_RELA]), dyn[DT_RELASZ], 3);

    // _dl_locked_report_globals needs the precise relro bounds so those are
    // what get stored.  But actually applying them requires page truncation.
    const size_t relro_start = p->relro_start & -PAGE_SIZE;
    const size_t relro_end = p->relro_end & -PAGE_SIZE;

    if (head != &ldso && relro_start != relro_end) {
      zx_status_t status = _zx_vmar_protect(p->vmar, ZX_VM_PERM_READ, saddr(p, relro_start),
                                            relro_end - relro_start);
      if (status == ZX_ERR_BAD_HANDLE && p == &ldso && p->vmar == ZX_HANDLE_INVALID) {
        debugmsg(
            "No VMAR_LOADED handle received;"
            " cannot protect RELRO for %s\n",
            p->l_map.l_name);
      } else if (status != ZX_OK) {
        error(
            "Error relocating %s: RELRO protection"
            " %p+%#zx failed: %s",
            p->l_map.l_name, laddr(p, relro_start), relro_end - relro_start,
            _zx_status_get_string(status));
        if (runtime)
          longjmp(*rtld_fail, 1);
      }
    }

    // Hold the VMAR handle only long enough to apply RELRO.
    // Now it's no longer needed and the mappings cannot be
    // changed any more (only unmapped).
    if (p->vmar != ZX_HANDLE_INVALID && !KEEP_DSO_VMAR) {
      _zx_handle_close(p->vmar);
      p->vmar = ZX_HANDLE_INVALID;
    }

    p->relocated = 1;
  }
}

LIBC_NO_SAFESTACK NO_ASAN static void kernel_mapped_dso(struct dso* p) {
  size_t min_addr = -1, max_addr = 0, cnt;
  const Phdr* ph = p->phdr;
  for (cnt = p->phnum; cnt--; ph = (void*)((char*)ph + p->phentsize)) {
    switch (ph->p_type) {
      case PT_LOAD:
        if (ph->p_vaddr < min_addr)
          min_addr = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > max_addr)
          max_addr = ph->p_vaddr + ph->p_memsz;
        break;
      case PT_DYNAMIC:
        p->l_map.l_ld = laddr(p, ph->p_vaddr);
        break;
      case PT_GNU_RELRO:
        p->relro_start = ph->p_vaddr;
        p->relro_end = ph->p_vaddr + ph->p_memsz;
        break;
      case PT_NOTE:
        if (p->build_id_note == NULL)
          find_buildid_note(p, ph);
        break;
    }
  }
  min_addr &= -PAGE_SIZE;
  max_addr = (max_addr + PAGE_SIZE - 1) & -PAGE_SIZE;
  p->map = laddr(p, min_addr);
  p->map_len = max_addr - min_addr;
  assign_module_id(p);
}

void __libc_exit_fini(void) {
  struct dso* p;
  size_t dyn[DT_NUM];
  for (p = fini_head; p; p = p->fini_next) {
    if (!p->constructed)
      continue;
    decode_vec(p->l_map.l_ld, dyn, DT_NUM);
    if (dyn[0] & (1 << DT_FINI_ARRAY)) {
      size_t n = dyn[DT_FINI_ARRAYSZ] / sizeof(size_t);
      size_t* fn = (size_t*)laddr(p, dyn[DT_FINI_ARRAY]) + n;
      while (n--)
        ((void (*)(void)) * --fn)();
    }
#ifndef NO_LEGACY_INITFINI
    if ((dyn[0] & (1 << DT_FINI)) && dyn[DT_FINI])
      fpaddr(p, dyn[DT_FINI])();
#endif
  }
}

static void do_init_fini(struct dso* p) {
  size_t dyn[DT_NUM];
  /* Allow recursive calls that arise when a library calls
   * dlopen from one of its constructors, but block any
   * other threads until all ctors have finished. */
  pthread_mutex_lock(&init_fini_lock);
  for (; p; p = dso_prev(p)) {
    if (p->constructed)
      continue;
    p->constructed = 1;
    decode_vec(p->l_map.l_ld, dyn, DT_NUM);
    if (dyn[0] & ((1 << DT_FINI) | (1 << DT_FINI_ARRAY))) {
      p->fini_next = fini_head;
      fini_head = p;
    }
#ifndef NO_LEGACY_INITFINI
    if ((dyn[0] & (1 << DT_INIT)) && dyn[DT_INIT])
      fpaddr(p, dyn[DT_INIT])();
#endif
    if (dyn[0] & (1 << DT_INIT_ARRAY)) {
      size_t n = dyn[DT_INIT_ARRAYSZ] / sizeof(size_t);
      size_t* fn = laddr(p, dyn[DT_INIT_ARRAY]);
      while (n--)
        ((void (*)(void)) * fn++)();
    }
  }
  pthread_mutex_unlock(&init_fini_lock);
}

void __libc_start_init(void) {
  // If a preinit hook spawns a thread that calls dlopen, that thread will
  // get to do_init_fini and block on the lock.  Now the main thread finishes
  // preinit hooks and releases the lock.  Then it's a race for which thread
  // gets the lock and actually runs all the normal constructors.  This is
  // expected, but to avoid such races preinit hooks should be very careful
  // about what they do and rely on.
  pthread_mutex_lock(&init_fini_lock);
  size_t dyn[DT_NUM];
  decode_vec(head->l_map.l_ld, dyn, DT_NUM);
  if (dyn[0] & (1ul << DT_PREINIT_ARRAY)) {
    size_t n = dyn[DT_PREINIT_ARRAYSZ] / sizeof(size_t);
    size_t* fn = laddr(head, dyn[DT_PREINIT_ARRAY]);
    while (n--)
      ((void (*)(void)) * fn++)();
  }
  pthread_mutex_unlock(&init_fini_lock);

  do_init_fini(tail);
}

// This function exists just to have a breakpoint set on its entry point.
// Define it in assembly as a single return instruction to avoid any ABI
// interactions.
void _dl_debug_state(void);
__asm__(
    ".pushsection .text._dl_debug_state,\"ax\",%progbits\n"
    ".type _dl_debug_state,%function\n"
    "_dl_debug_state: ret\n"
    ".size _dl_debug_state, . - _dl_debug_state\n"
    ".popsection");

__attribute__((__visibility__("hidden"))) void* __tls_get_new(size_t offset, size_t modid) {
  pthread_t self = __pthread_self();

  if (modid <= (size_t)self->head.dtv[0]) {
    return (char*)self->head.dtv[modid] + offset + DTP_OFFSET;
  }

  /* This is safe without any locks held because, if the caller
   * is able to request the Nth entry of the DTV, the DSO list
   * must be valid at least that far out and it was synchronized
   * at program startup or by an already-completed call to dlopen. */
  struct dso* p;
  for (p = head; p->tls_id != modid; p = dso_next(p))
    ;

  /* Get new DTV space from new DSO if needed */
  if (modid > (size_t)self->head.dtv[0]) {
    void** newdtv = p->new_dtv + (modid + 1) * atomic_fetch_add(&p->new_dtv_idx, 1);
    memcpy(newdtv, self->head.dtv, ((size_t)self->head.dtv[0] + 1) * sizeof(void*));
    newdtv[0] = (void*)modid;
    self->head.dtv = newdtv;
  }

  /* Get new TLS memory from all new DSOs up to the requested one */
  unsigned char* mem;
  for (p = head;; p = dso_next(p)) {
    if (!p->tls_id || self->head.dtv[p->tls_id])
      continue;
    mem = p->new_tls + (p->tls.size + p->tls.align) * atomic_fetch_add(&p->new_tls_idx, 1);
    mem += ((uintptr_t)p->tls.image - (uintptr_t)mem) & (p->tls.align - 1);
    self->head.dtv[p->tls_id] = mem;
    memcpy(mem, p->tls.image, p->tls.len);
    if (p->tls_id == modid)
      break;
  }
  return mem + offset + DTP_OFFSET;
}

LIBC_NO_SAFESTACK thrd_info_t __init_main_thread(zx_handle_t thread_self) {
  pthread_attr_t attr = DEFAULT_PTHREAD_ATTR;

  char thread_self_name[ZX_MAX_NAME_LEN];
  if (_zx_object_get_property(thread_self, ZX_PROP_NAME, thread_self_name,
                              sizeof(thread_self_name)) != ZX_OK)
    strcpy(thread_self_name, "(initial-thread)");
  thrd_t td = __allocate_thread(attr._a_guardsize, attr._a_stacksize, thread_self_name, NULL);
  if (td == NULL) {
    debugmsg("No memory for %zu bytes thread-local storage.\n", libc.tls_size);
    _exit(127);
  }

  zx_status_t status = zxr_thread_adopt(thread_self, &td->zxr_thread);
  if (status != ZX_OK)
    CRASH_WITH_UNIQUE_BACKTRACE();

  zxr_tp_set(thread_self, pthread_to_tp(td));

  thrd_info_t info = {td, &runtime};
  return info;
}

LIBC_NO_SAFESTACK static void update_tls_size(void) {
  libc.tls_cnt = tls_cnt;
  libc.tls_align = tls_align;
  libc.tls_size =
      ZX_ALIGN((1 + tls_cnt) * sizeof(void*) + tls_offset + sizeof(struct pthread) + tls_align * 2,
               tls_align);
  // TODO(mcgrathr): The TLS block is always allocated in whole pages.
  // We should keep track of the available slop to the end of the page
  // and make dlopen use that for new dtv/TLS space when it fits.
}

/* Stage 1 of the dynamic linker is defined in dlstart.c. It calls the
 * following stage 2 and stage 3 functions via primitive symbolic lookup
 * since it does not have access to their addresses to begin with. */

/* Stage 2 of the dynamic linker is called after relative relocations
 * have been processed. It can make function calls to static functions
 * and access string literals and static data, but cannot use extern
 * symbols. Its job is to perform symbolic relocations on the dynamic
 * linker itself, but some of the relocations performed may need to be
 * replaced later due to copy relocations in the main program. */

static dl_start_return_t __dls3(void* start_arg);

LIBC_NO_SAFESTACK NO_ASAN __attribute__((__visibility__("hidden"))) dl_start_return_t __dls2(
    void* start_arg, void* vdso_map) {
  ldso.l_map.l_addr = (uintptr_t)__ehdr_start;

  Ehdr* ehdr = (void*)ldso.l_map.l_addr;
  ldso.l_map.l_name = (char*)"libc.so";
  ldso.global = -1;
  ldso.phnum = ehdr->e_phnum;
  ldso.phdr = laddr(&ldso, ehdr->e_phoff);
  ldso.phentsize = ehdr->e_phentsize;
  kernel_mapped_dso(&ldso);
  decode_dyn(&ldso);

  if (vdso_map != NULL) {
    // The vDSO was mapped in by our creator.  Stitch it in as
    // a preloaded shared object right away, so ld.so itself
    // can depend on it and require its symbols.

    vdso.l_map.l_addr = (uintptr_t)vdso_map;
    vdso.l_map.l_name = (char*)"<vDSO>";
    vdso.global = -1;

    Ehdr* ehdr = vdso_map;
    vdso.phnum = ehdr->e_phnum;
    vdso.phdr = laddr(&vdso, ehdr->e_phoff);
    vdso.phentsize = ehdr->e_phentsize;
    kernel_mapped_dso(&vdso);
    decode_dyn(&vdso);

    dso_set_prev(&vdso, &ldso);
    dso_set_next(&ldso, &vdso);
    tail = &vdso;
  }

  /* Prepare storage for to save clobbered REL addends so they
   * can be reused in stage 3. There should be very few. If
   * something goes wrong and there are a huge number, abort
   * instead of risking stack overflow. */
  size_t dyn[DT_NUM];
  decode_vec(ldso.l_map.l_ld, dyn, DT_NUM);
  size_t* rel = laddr(&ldso, dyn[DT_REL]);
  size_t rel_size = dyn[DT_RELSZ];
  size_t addend_rel_cnt = 0;
  apply_addends_to = rel;
  for (; rel_size; rel += 2, rel_size -= 2 * sizeof(size_t)) {
    switch (R_TYPE(rel[1])) {
      // These types do not need a saved addend.  Only REL_RELATIVE uses an
      // addend at all, and all REL_RELATIVE relocs in ldso were already
      // processed in phase 1 and are just skipped now.  Note this must
      // match the logic in do_relocs so that the indices always match up.
      case REL_RELATIVE:
      case REL_GOT:
      case REL_PLT:
      case REL_COPY:
        break;
      default:
        addend_rel_cnt++;
    }
  }
  if (addend_rel_cnt >= ADDEND_LIMIT)
    CRASH_WITH_UNIQUE_BACKTRACE();
  size_t addends[addend_rel_cnt];
  saved_addends = addends;

  head = &ldso;
  reloc_all(&ldso);

  ldso.relocated = 0;

  // Make sure all the relocations have landed before calling __dls3,
  // which relies on them.
  atomic_signal_fence(memory_order_seq_cst);

  return __dls3(start_arg);
}

#define LIBS_VAR "LD_DEBUG="
#define TRACE_VAR "LD_TRACE="

LIBC_NO_SAFESTACK static void scan_env_strings(const char* strings, const char* limit,
                                               uint32_t count) {
  while (count-- > 0) {
    char* end = memchr(strings, '\0', limit - strings);
    if (end == NULL) {
      break;
    }
    if (end - strings >= sizeof(LIBS_VAR) - 1 && !memcmp(strings, LIBS_VAR, sizeof(LIBS_VAR) - 1)) {
      if (strings[sizeof(LIBS_VAR) - 1] != '\0') {
        log_libs = true;
      }
    } else if (end - strings >= sizeof(TRACE_VAR) - 1 &&
               !memcmp(strings, TRACE_VAR, sizeof(TRACE_VAR) - 1)) {
      // Features like Intel Processor Trace require specific output in a
      // specific format. Thus this output has its own env var.
      if (strings[sizeof(TRACE_VAR) - 1] != '\0') {
        trace_maps = true;
      }
    }
    strings = end + 1;
  }
}

/* Stage 3 of the dynamic linker is called with the dynamic linker/libc
 * fully functional. Its job is to load (if not already loaded) and
 * process dependencies and relocations for the main application and
 * transfer control to its entry point. */

LIBC_NO_SAFESTACK static void* dls3(zx_handle_t exec_vmo, const char* argv0,
                                    const char* env_strings, const char* env_strings_limit,
                                    uint32_t env_strings_count) {
  // First load our own dependencies.  Usually this will be just the
  // vDSO, which is already loaded, so there will be nothing to do.
  // In a sanitized build, we'll depend on the sanitizer runtime DSO
  // and load that now (and its dependencies, such as the unwinder).
  load_deps(&ldso);

  // Now reorder the list so that we appear last, after all our
  // dependencies.  This ensures that e.g. the sanitizer runtime's
  // malloc will be chosen over ours, even if the application
  // doesn't itself depend on the sanitizer runtime SONAME.
  dso_set_prev(dso_next(&ldso), NULL);
  detached_head = dso_next(&ldso);
  dso_set_prev(&ldso, tail);
  dso_set_next(&ldso, NULL);
  dso_set_next(tail, &ldso);

  static struct dso app;

  libc.page_size = PAGE_SIZE;

  scan_env_strings(env_strings, env_strings_limit, env_strings_count);

  zx_status_t status = map_library(exec_vmo, &app);
  _zx_handle_close(exec_vmo);
  if (status != ZX_OK) {
    debugmsg("%s: %s: Not a valid dynamic program (%s)\n", ldso.l_map.l_name, argv0,
             _zx_status_get_string(status));
    _exit(1);
  }

  app.l_map.l_name = (char*)argv0;

  if (app.tls.size) {
    libc.tls_head = tls_tail = &app.tls;
    app.tls_id = tls_cnt = 1;
#ifdef TLS_ABOVE_TP
    app.tls.offset = (tls_offset + app.tls.align - 1) & -app.tls.align;
    tls_offset = app.tls.offset + app.tls.size;
#else
    tls_offset = app.tls.offset =
        app.tls.size + (-((uintptr_t)app.tls.image + app.tls.size) & (app.tls.align - 1));
#endif
    tls_align = MAXP2(tls_align, app.tls.align);
  }

  app.global = 1;
  decode_dyn(&app);

  /* Initial dso chain consists only of the app. */
  head = tail = &app;

  // Load preload/needed libraries, add their symbols to the global
  // namespace, and perform all remaining relocations.
  //
  // Do TLS layout for DSOs after loading, but before relocation.
  // This needs to be after the main program's TLS setup (just
  // above), which has to be the first since it can use static TLS
  // offsets (local-exec TLS model) that are presumed to start at
  // the beginning of the static TLS block.  But we may have loaded
  // some libraries (sanitizer runtime) before that, so we don't do
  // each library's TLS setup directly in load_library_vmo.

  load_deps(&app);

  app.global = 1;
  for (struct dso* p = dso_next(&app); p != NULL; p = dso_next(p)) {
    p->global = 1;
    do_tls_layout(p, NULL, 0);
  }

  for (size_t i = 0; app.l_map.l_ld[i].d_tag; i++) {
    if (!DT_DEBUG_INDIRECT && app.l_map.l_ld[i].d_tag == DT_DEBUG)
      app.l_map.l_ld[i].d_un.d_ptr = (size_t)&debug;
    if (DT_DEBUG_INDIRECT && app.l_map.l_ld[i].d_tag == DT_DEBUG_INDIRECT) {
      size_t* ptr = (size_t*)app.l_map.l_ld[i].d_un.d_ptr;
      *ptr = (size_t)&debug;
    }
  }

  /* The main program must be relocated LAST since it may contin
   * copy relocations which depend on libraries' relocations. */
  reloc_all(dso_next(&app));
  reloc_all(&app);

  update_tls_size();
  static_tls_cnt = tls_cnt;

  if (ldso_fail)
    _exit(127);

  // Logically we could now switch to "runtime mode", because
  // startup-time dynamic linking work per se is done now.  However,
  // the real concrete meaning of "runtime mode" is that the dlerror
  // machinery is usable.  It's not usable until the thread descriptor
  // has been set up.  So the switch to "runtime mode" happens in
  // __init_main_thread instead.

  atomic_init(&unlogged_tail, (uintptr_t)tail);

  debug.r_version = 1;
  debug.r_brk = (uintptr_t)&_dl_debug_state;
  debug.r_map = &head->l_map;
  debug.r_ldbase = ldso.l_map.l_addr;
  debug.r_state = 0;

  // If setting ZX_PROP_PROCESS_DEBUG_ADDR fails, crashlogger backtraces, debugger
  // sessions, etc. will be problematic, but this isn't fatal.
  _zx_object_set_property(__zircon_process_self, ZX_PROP_PROCESS_DEBUG_ADDR, &_dl_debug_addr,
                          sizeof(_dl_debug_addr));

  // Check if the process has to issue a debug trap.
  if (should_break_on_load()) {
    debug_break();
  }

  _dl_debug_state();

  if (log_libs)
    _dl_log_unlogged();

  if (trace_maps) {
    for (struct dso* p = &app; p != NULL; p = dso_next(p)) {
      trace_load(p);
    }
  }

  // Reset from the argv0 value so we don't save a dangling pointer
  // into the caller's stack frame.
  app.l_map.l_name = (char*)"";

  // Check for a PT_GNU_STACK header requesting a main thread stack size.
  libc.stack_size = ZIRCON_DEFAULT_STACK_SIZE;
  for (size_t i = 0; i < app.phnum; i++) {
    if (app.phdr[i].p_type == PT_GNU_STACK) {
      size_t size = app.phdr[i].p_memsz;
      if (size > 0)
        libc.stack_size = size;
      break;
    }
  }

  const Ehdr* ehdr = (void*)app.map;
  return laddr(&app, ehdr->e_entry);
}

LIBC_NO_SAFESTACK NO_ASAN static dl_start_return_t __dls3(void* start_arg) {
  zx_handle_t bootstrap = (uintptr_t)start_arg;

  uint32_t nbytes, nhandles;
  zx_status_t status = processargs_message_size(bootstrap, &nbytes, &nhandles);
  if (status != ZX_OK) {
    error("processargs_message_size bootstrap handle %#x failed: %d (%s)", bootstrap, status,
          _zx_status_get_string(status));
    nbytes = nhandles = 0;
  }

  // Do not allow any zero length VLAs allocated on the stack.
  //
  // TODO(44088) : See this bug for options which might allow us to avoid the
  // need for variable length arrays of any form at this stage.
  if ((nbytes == 0) || (nhandles == 0)) {
    CRASH_WITH_UNIQUE_BACKTRACE();
    _exit(1);
  }

  PROCESSARGS_BUFFER(buffer, nbytes);
  zx_handle_t handles[nhandles];
  zx_proc_args_t* procargs;
  uint32_t* handle_info;
  if (status == ZX_OK)
    status =
        processargs_read(bootstrap, buffer, nbytes, handles, nhandles, &procargs, &handle_info);
  if (status != ZX_OK) {
    error(
        "bad message of %u bytes, %u handles"
        " from bootstrap handle %#x: %d (%s)",
        nbytes, nhandles, bootstrap, status, _zx_status_get_string(status));
    nbytes = nhandles = 0;
  }

  zx_handle_t exec_vmo = ZX_HANDLE_INVALID;
  for (int i = 0; i < nhandles; ++i) {
    switch (PA_HND_TYPE(handle_info[i])) {
      case PA_LDSVC_LOADER:
        if (loader_svc != ZX_HANDLE_INVALID || handles[i] == ZX_HANDLE_INVALID) {
          error("bootstrap message bad LOADER_SVC %#x vs %#x", handles[i], loader_svc);
        }
        loader_svc = handles[i];
        break;
      case PA_VMO_EXECUTABLE:
        if (exec_vmo != ZX_HANDLE_INVALID || handles[i] == ZX_HANDLE_INVALID) {
          error("bootstrap message bad EXEC_VMO %#x vs %#x", handles[i], exec_vmo);
        }
        exec_vmo = handles[i];
        break;
      case PA_FD:
        if (logger != ZX_HANDLE_INVALID || handles[i] == ZX_HANDLE_INVALID) {
          error("bootstrap message bad FD %#x vs %#x", handles[i], logger);
        }
        logger = handles[i];
        break;
      case PA_VMAR_LOADED:
        if (ldso.vmar != ZX_HANDLE_INVALID || handles[i] == ZX_HANDLE_INVALID) {
          error("bootstrap message bad VMAR_LOADED %#x vs %#x", handles[i], ldso.vmar);
        }
        ldso.vmar = handles[i];
        break;
      case PA_PROC_SELF:
        __zircon_process_self = handles[i];
        break;
      case PA_VMAR_ROOT:
        __zircon_vmar_root_self = handles[i];
        break;
      default:
        _zx_handle_close(handles[i]);
        break;
    }
  }

  // TODO(mcgrathr): For now, always use a kernel log channel.
  // This needs to be replaced by a proper unprivileged logging scheme ASAP.
  if (logger == ZX_HANDLE_INVALID) {
    _zx_debuglog_create(ZX_HANDLE_INVALID, 0, &logger);
  }

  if (__zircon_process_self == ZX_HANDLE_INVALID)
    error("bootstrap message bad no proc self");
  if (__zircon_vmar_root_self == ZX_HANDLE_INVALID)
    error("bootstrap message bad no root vmar");

  // At this point we can make system calls and have our essential
  // handles, so things are somewhat normal.
  early_init();

  // The initial processargs message may not pass the application
  // name or any other arguments, so we check that condition.
  void* entry =
      dls3(exec_vmo, procargs->args_num == 0 ? "" : (const char*)&buffer[procargs->args_off],
           (const char*)&buffer[procargs->environ_off], (const char*)&buffer[nbytes],
           procargs->environ_num);

  if (vdso.global <= 0) {
    // Nothing linked against the vDSO.  Ideally we would unmap the
    // vDSO, but there is no way to do it because the unmap system call
    // would try to return to the vDSO code and crash.
    if (ldso.global < 0) {
      // TODO(mcgrathr): We could free all heap data structures, and
      // with some vDSO assistance unmap ourselves and unwind back to
      // the user entry point.  Thus a program could link against the
      // vDSO alone and not use this libc/ldso at all after startup.
      // We'd need to be sure there are no TLSDESC entries pointing
      // back to our code, but other than that there should no longer
      // be a way to enter our code.
    } else {
      debugmsg("Dynamic linker %s doesn't link in vDSO %s???\n", ldso.l_map.l_name,
               vdso.l_map.l_name);
      _exit(127);
    }
  } else if (ldso.global <= 0) {
    // This should be impossible.
    CRASH_WITH_UNIQUE_BACKTRACE();
  }

#ifdef __aarch64__
// The official psABI document at
// https://developer.arm.com/documentation/ihi0057/latest (2020Q2 version) does
// not assign a DWARF register number for TPIDR_mode.  But ARM has posted
// https://github.com/ARM-software/abi-aa/pull/53 to assign new numbers.
#define DWARG_REGNO_TP 36  // TPIDR_EL0
#elif defined(__x86_64__)
#define DWARG_REGNO_TP 58  // %fs.base
#elif defined(__riscv)
#define DWARG_REGNO_TP 4  // tp = x4
#endif

  // This has to be inside some function so that it can use extended asm to
  // inject constants from C.  It has to be somewhere that definitely doesn't
  // get optimized away as unreachable by the compiler so that it's actually
  // assembled into the final shared library.
  //
  // This establishes a new protocol with the debugger: there will be a
  // debugging section called .zxdb_debug_api; this is allocated for the
  // convenience of zxdb's current implementation, but in principle should be
  // non-allocated like other such sections.  ELF symbols in this section
  // provide named API "calls".  Each "call" is a DWARF expression whose
  // offset into the section and size in bytes are indicated by the st_value
  // and st_size fields of the symbol.  The protocol for what values each call
  // expects on the stack and/or delivers on the stack on return is described
  // for each call below.  Every call may need access to process memory via
  // DW_OP_deref et al.  Some calls need access to thread registers via
  // DW_OP_breg*; these calls document that need in their "Input:" section.
  // Any DW_OP_addr operations encode an address relative to the load address
  // of the module containing this section.
  uintptr_t bogon;
  __asm__ volatile(
  // Since libc is linked with --gc-sections, the .zxdb_debug_api section
  // will be dropped as unreferenced since it's an allocated section.  By
  // rights, it should be a non-allocated section, but making it allocated
  // simplifies things for zxdb right now and is harmless enough.  But, it
  // means something must prevent the section from being GC'd.  So this
  // useless instruction serves that purpose.
#ifdef __aarch64__
      "adrp %0, zxdb.thrd_t\n"
#elif defined(__riscv)
      "lla %0, zxdb.thrd_t\n"
#elif defined(__x86_64__)
      "lea zxdb.thrd_t(%%rip), %0\n"
#else
#error "what machine?"
#endif
      ".pushsection .zxdb_debug_api,\"a\",%%progbits\n"
#define DEBUG_API(name) #name ":\n"
#define DEBUG_API_END(name) ".size " #name ", . - " #name "\n"

      // zxdb.thrd_t: Yield the thrd_current() value from thread registers.
      // zxdb.pthread_t: Yield the pthread_self() value from thread registers.
      // Input: Consumes no stack entries; uses thread registers.
      // Output: Pushes one word, to be displayed as a thrd_t value.
      // This is the value thrd_current() returns in this thread; as per the
      // C standard, this value never changes for the lifetime of the thread.
      // In Fuchsia's implementation thrd_t and pthread_t happen to be the
      // same thing and thrd_current() and pthread_self() always return the
      // same thing.  That is why these two debugging APIs are just aliases
      // for the same code, but it is not something a debugger should assume
      // generically as it might differ in future implementations.
      DEBUG_API(zxdb.thrd_t)
      DEBUG_API(zxdb.pthread_t)
      ".byte %c[bregx]\n"
      "  .uleb128 %c[tp_regno]\n"
      "  .sleb128 %c[pthread_tp_offset]\n"
      DEBUG_API_END(zxdb.thrd_t)
      DEBUG_API_END(zxdb.pthread_t)

      // zxdb.link_map_tls_modid: Yield TLS module ID from `struct link_map`.
      // Input: Pops a `struct link_map *` value; uses no thread registers.
      // The valid `struct link_map *` pointer values can be obtained by walking
      // the list from the `struct r_debug` structure accessed via the ELF
      // DT_DEBUG protocol or the Zircon ZX_PROP_PROCESS_DEBUG_ADDR protocol.
      // Output: Pushes one word, the TLS module ID or 0 if no PT_TLS segment.
      // This value is fixed at load time, so it can be computed just once per
      // module and cached.  The TLS module ID is a positive integer that
      // appears in GOT slots with DTPMOD relocations and can be used with
      // zxdb.tlsbase (below) to refer to the given module's PT_TLS segment.
      DEBUG_API(zxdb.link_map_tls_modid)
      ".byte %c[plus_uconst]\n"
      "  .uleb128 %c[tls_id_offset]\n"
      ".byte %c[deref]\n"
      DEBUG_API_END(zxdb.link_map_tls_modid)

      // zxdb.tlsbase: Yield the address of a given module's TLS block.
      // Input: Pops one word, the TLS module ID; uses thread registers.
      // Output: Pushes one word, the address of the thread's TLS block for
      // that module or 0 if the thread hasn't allocated one.  Each module
      // with a PT_TLS segment has a TLS block in each thread that corresponds
      // to the SHT_TLS symbols that appear in that module, which their
      // st_value offsets being the offsets into that TLS block (i.e. `p_vaddr
      // + st_value` is where the initial contents for that symbol appear).
      // When a particular thread has not yet allocated a particular module's
      // TLS block the debugger can show the initial values from the PT_TLS
      // segment's p_vaddr, but cannot modify the values until the thread does
      // an actual TLS access to get the block allocated.  A later attempt on
      // the same thread and module ID might yield a nonzero value.  Once a
      // nonzero value has been delivered for a given thread and module, it
      // will never change but becomes an invalid pointer when the thread dies
      // and when the module is unloaded.
      DEBUG_API(zxdb.tlsbase)
      // This matches the logic in the __tls_get_addr implementation.
      ".byte %c[bregx]\n"  // Compute &pthread_self()->head.dtv.
      "  .uleb128 %c[tp_regno]\n"
      "  .sleb128 %c[dtv_offset]\n"
      ".byte %c[deref]\n"  // tos is now the DTV address itself.
      ".byte %c[over]\n"   // Push a copy of the module ID.
      ".byte %c[over]\n"   // Save a copy of the DTV address for later.
      ".byte %c[deref]\n"  // tos is now the thread's DTV generation.
      ".byte %c[le]\n"     // modid <= generation?
      ".byte %c[bra]\n"    // If yes, goto 1.
      "  .short 1f-0f\n"
      "0:\n"
      ".byte %c[drop], %c[drop]\n"  // Clear the stack.
      ".byte %c[lit0]\n"            // Push return value of zero.
      ".byte %c[skip]\n"            // Branch to the end, i.e. return.
      "  .short 2f-1f\n"
      "1:\n"
      ".byte %c[swap]\n"  // Get the module ID back to the top of the stack.
      // The module ID is an index into the DTV; scale that to the byte offset.
      ".byte %c[const1u], %c[dtvscale], %c[mul]\n"  // tos *= dtvscale
      ".byte %c[plus], %c[deref]\n"                 // Return dtv[id].
      "2:\n"
      DEBUG_API_END(zxdb.tlsbase)

#undef DEBUG_API
#undef DEBUG_API_END
      ".popsection"
      : "=r"(bogon)
      :
      // DW_OP_* constants per DWARF spec.
      [ bra ] "i"(0x28), [ bregx ] "i"(0x92), [ const1u ] "i"(0x08), [ deref ] "i"(0x06),
      [ drop ] "i"(0x13), [ dup ] "i"(0x12), [ le ] "i"(0x2c), [ lit0 ] "i"(0x30),
      [ mul ] "i"(0x1e), [ over ] "i"(0x14), [ plus ] "i"(0x22), [ plus_uconst ] "i"(0x23),
      [ skip ] "i"(0x2f), [ swap ] "i"(0x16),

      // Used in thrd_t.
      [ tp_regno ] "i"(DWARG_REGNO_TP), [ pthread_tp_offset ] "i"(-PTHREAD_TP_OFFSET),

      // Used in link_map_tls_modid.
      [ tls_id_offset ] "i"(offsetof(struct dso, tls_id)),

      // Used in tlsbase.
      [ dtv_offset ] "i"(TP_OFFSETOF(head.dtv)), [ dtvscale ] "i"(sizeof((tcbhead_t){}.dtv[0])));

#undef OP

  return DL_START_RETURN(entry, start_arg);
}

// Do sanitizer setup and whatever else must be done before dls3.
LIBC_NO_SAFESTACK NO_ASAN static void early_init(void) {
  __asan_early_init();
#ifdef DYNLINK_LDSVC_CONFIG
  // Inform the loader service to look for libraries of the right variant.
  loader_svc_config(DYNLINK_LDSVC_CONFIG);
#endif
}

static void set_global(struct dso* p, int global) {
  if (p->global > 0) {
    // Short-circuit if it's already fully global.  Its deps will be too.
    return;
  } else if (p->global == global) {
    // This catches circular references as well as other redundant walks.
    return;
  }
  p->global = global;
  if (p->deps != NULL) {
    for (struct dso** dep = p->deps; *dep != NULL; ++dep) {
      set_global(*dep, global);
    }
  }
}

// This function is unsanitized so this function can easily be called before
// globals are registered by hwasan, and it can be used to register globals
// early for hwasan.
NO_ASAN
static struct dl_phdr_info get_phdr_info(const struct dso* current) {
  struct dl_phdr_info info = {
      .dlpi_addr = (uintptr_t)current->l_map.l_addr,
      .dlpi_name = current->l_map.l_name,
      .dlpi_phdr = current->phdr,
      .dlpi_phnum = current->phnum,
      .dlpi_adds = gencnt,
      .dlpi_subs = 0,
      .dlpi_tls_modid = current->tls_id,
      .dlpi_tls_data = current->tls.image,
  };
  return info;
}

// Similar to dl_iterate_phdr, iterate over all loaded DSOs, but the only
// callback is to __sanitizer_module_loaded which a sanitizer runtime can
// override. This is called in two places:
//
// 1) In dlopen right before iterating through .init_array.
// 2) In the initial execution path at the start of start_main when shadow call
//    stack is setup.
//
// This function is hidden because it should only be used within libc. This
// function is also unsanitized so this function can easily be called before
// globals are registered by hwasan, and it can be used to register globals
// early for hwasan.
NO_ASAN
__attribute__((visibility("hidden"))) void _dl_iterate_loaded_libs(void) {
  for (struct dso* current = tail; current; current = dso_prev(current)) {
    if (current->constructed)
      continue;
    struct dl_phdr_info info = get_phdr_info(current);
    __sanitizer_module_loaded(&info, sizeof(info));
  }
}

static void* dlopen_internal(zx_handle_t vmo, const char* file, int mode) {
  // N.B. This lock order must be consistent with other uses such as
  // ThreadSuspender in the __sanitizer_memory_snapshot implementation.
  _dl_wrlock();
  __thread_allocation_inhibit();

  struct dso* orig_tail = tail;

  struct dso* p;
  zx_status_t status = (vmo != ZX_HANDLE_INVALID ? load_library_vmo(vmo, file, mode, head, &p)
                                                 : load_library(file, mode, head, &p));

  if (status != ZX_OK) {
    error("Error loading shared library %s: %s", file, _zx_status_get_string(status));
  fail:
    __thread_allocation_release();
    _dl_unlock();
    return NULL;
  }

  if (p == NULL) {
    if (!(mode & RTLD_NOLOAD))
      CRASH_WITH_UNIQUE_BACKTRACE();
    error("Library %s is not already loaded", file);
    goto fail;
  }

  struct tls_module* orig_tls_tail = tls_tail;
  size_t orig_tls_cnt = tls_cnt;
  size_t orig_tls_offset = tls_offset;
  size_t orig_tls_align = tls_align;

  struct dl_alloc_checkpoint checkpoint;
  dl_alloc_checkpoint(&checkpoint);

  jmp_buf jb;
  rtld_fail = &jb;
  if (setjmp(*rtld_fail)) {
    /* Clean up anything new that was (partially) loaded */
    if (p && p->deps)
      set_global(p, 0);
    for (p = dso_next(orig_tail); p; p = dso_next(p))
      unmap_library(p);
    if (!orig_tls_tail)
      libc.tls_head = 0;
    tls_tail = orig_tls_tail;
    tls_cnt = orig_tls_cnt;
    tls_offset = orig_tls_offset;
    tls_align = orig_tls_align;
    tail = orig_tail;
    dso_set_next(tail, NULL);
    dl_alloc_rollback(&checkpoint);
    goto fail;
  }

  /* First load handling */
  if (!p->deps) {
    load_deps(p);
    set_global(p, -1);
    reloc_all(p);
    set_global(p, 0);
  }

  if (mode & RTLD_GLOBAL) {
    set_global(p, 1);
  }

  update_tls_size();

  // Check if the process has set the state to break on this load.
  if (should_break_on_load()) {
    debug_break();
  }

  _dl_debug_state();
  if (trace_maps) {
    trace_load(p);
  }

  // Allow thread creation, now that the TLS bookkeeping is consistent.
  __thread_allocation_release();

  // Bump the dl_iterate_phdr dlpi_adds counter.
  gencnt++;

  // Collect the current new tail before we release the lock.
  // Another dlopen can come in and advance the tail, but we
  // alone are responsible for making sure that do_init_fini
  // starts with the first object we just added.
  struct dso* new_tail = tail;

  // The next _dl_log_unlogged can safely read the 'struct dso' list from
  // head up through new_tail.  Most fields will never change again.
  atomic_store_explicit(&unlogged_tail, (uintptr_t)new_tail, memory_order_release);

  _dl_unlock();

  if (log_libs)
    _dl_log_unlogged();

  // Run the __sanitizer_module_loaded hook on newly loaded libraries. This is
  // useful for sanitizers that need to observe something from loaded libs
  // before module constructors are called. An example of this is hwasan with
  // __hwasan_library_loaded which needs to register interposable globals in
  // newly loaded libs before module constructors potentially access these
  // interposed globals.
  _dl_iterate_loaded_libs();

  do_init_fini(new_tail);

  return p;
}

void* dlopen(const char* file, int mode) {
  if (!file)
    return head;
  return dlopen_internal(ZX_HANDLE_INVALID, file, mode);
}

void* dlopen_vmo(zx_handle_t vmo, int mode) {
  if (vmo == ZX_HANDLE_INVALID) {
    errno = EINVAL;
    return NULL;
  }
  return dlopen_internal(vmo, NULL, mode);
}

zx_handle_t dl_set_loader_service(zx_handle_t new_svc) {
  zx_handle_t old_svc;
  _dl_wrlock();
  old_svc = loader_svc;
  loader_svc = new_svc;
  _dl_unlock();
  return old_svc;
}

__attribute__((__visibility__("hidden"))) int __dl_invalid_handle(void* h) {
  struct dso* p;
  for (p = head; p; p = dso_next(p))
    if (h == p)
      return 0;
  error("Invalid library handle %p", (void*)h);
  return 1;
}

static void* addr2dso(size_t a) {
  struct dso* p;
  for (p = head; p; p = dso_next(p)) {
    if (a - (size_t)p->map < p->map_len)
      return p;
  }
  return 0;
}

void* __tls_get_addr(size_t*);

static bool find_sym_for_dlsym(struct dso* p, const char* name, uint32_t* name_gnu_hash,
                               uint32_t* name_sysv_hash, void** result) {
  // Check if we already have seen this DSO before.
  if (p->in_dlsym)
    return false;

  const Sym* sym;
  if (p->ghashtab != NULL) {
    if (*name_gnu_hash == 0)
      *name_gnu_hash = gnu_hash(name);
    sym = gnu_lookup(*name_gnu_hash, p->ghashtab, p, name);
  } else {
    if (*name_sysv_hash == 0)
      *name_sysv_hash = sysv_hash(name);
    sym = sysv_lookup(name, *name_sysv_hash, p);
  }
  if (sym && (sym->st_info & 0xf) == STT_TLS) {
    *result = __tls_get_addr((size_t[]){p->tls_id, sym->st_value});
    return true;
  }
  if (sym && sym->st_value && (1 << (sym->st_info & 0xf) & OK_TYPES)) {
    *result = laddr(p, sym->st_value);
    return true;
  }
  if (p->deps) {
    p->in_dlsym = true;
    for (struct dso** dep = p->deps; *dep != NULL; ++dep) {
      if (find_sym_for_dlsym(*dep, name, name_gnu_hash, name_sysv_hash, result)) {
        p->in_dlsym = false;
        return true;
      }
    }
    p->in_dlsym = false;
  }
  return false;
}

static void* do_dlsym(struct dso* p, const char* s, void* ra) {
  if (p == head || p == RTLD_DEFAULT || p == RTLD_NEXT) {
    if (p == RTLD_DEFAULT) {
      p = head;
    } else if (p == RTLD_NEXT) {
      p = addr2dso((size_t)ra);
      if (!p)
        p = head;
      p = dso_next(p);
    }
    struct symdef def = find_sym(p, s, 0);
    if (!def.sym)
      goto failed;
    if ((def.sym->st_info & 0xf) == STT_TLS)
      return __tls_get_addr((size_t[]){def.dso->tls_id, def.sym->st_value});
    return laddr(def.dso, def.sym->st_value);
  }
  if (__dl_invalid_handle(p))
    return 0;
  uint32_t gnu_hash = 0, sysv_hash = 0;
  void* result;
  if (find_sym_for_dlsym(p, s, &gnu_hash, &sysv_hash, &result))
    return result;
failed:
  error("Symbol not found: %s", s);
  return 0;
}

int dladdr(const void* addr, Dl_info* info) {
  struct dso* p;

  _dl_rdlock();
  p = addr2dso((size_t)addr);
  _dl_unlock();

  if (!p)
    return 0;

  Sym* bestsym = NULL;
  void* best = 0;

  Sym* sym = p->syms;
  uint32_t nsym = count_syms(p);
  for (; nsym; nsym--, sym++) {
    if (sym->st_value && (1 << (sym->st_info & 0xf) & OK_TYPES) &&
        (1 << (sym->st_info >> 4) & OK_BINDS)) {
      void* symaddr = laddr(p, sym->st_value);
      if (symaddr > addr || symaddr < best)
        continue;
      best = symaddr;
      bestsym = sym;
      if (addr == symaddr)
        break;
    }
  }

  info->dli_fname = p->l_map.l_name;
  info->dli_fbase = (void*)p->l_map.l_addr;
  info->dli_sname = bestsym == NULL ? NULL : p->strings + bestsym->st_name;
  info->dli_saddr = bestsym == NULL ? NULL : best;

  return 1;
}

void* dlsym(void* restrict p, const char* restrict s) {
  void* res;
  _dl_wrlock();
  res = do_dlsym(p, s, __builtin_return_address(0));
  _dl_unlock();
  return res;
}

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info* info, size_t size, void* data),
                    void* data) {
  struct dso* current;
  int ret = 0;
  for (current = head; current;) {
    struct dl_phdr_info info = get_phdr_info(current);

    ret = (callback)(&info, sizeof(info), data);

    if (ret != 0)
      break;

    _dl_rdlock();
    current = dso_next(current);
    _dl_unlock();
  }
  return ret;
}

__attribute__((__visibility__("hidden"))) void __dl_vseterr(const char*, va_list);

#define LOADER_SVC_MSG_MAX 1024

LIBC_NO_SAFESTACK static zx_status_t loader_svc_rpc(uint64_t ordinal, const void* data, size_t len,
                                                    zx_handle_t request_handle,
                                                    zx_handle_t* result) {
  // Use a static buffer rather than one on the stack to avoid growing
  // the stack size too much.  Calls to this function are always
  // serialized anyway, so there is no danger of collision.
  static ldmsg_req_t req;
  size_t req_len;
  zx_status_t status = ldmsg_req_encode(ordinal, &req, &req_len, (const char*)data, len);
  if (status != ZX_OK) {
    _zx_handle_close(request_handle);
    error("message of %zu bytes too large for loader service protocol", len);
    return status;
  }

  if (result != NULL) {
    // Don't return an uninitialized value if the channel call
    // succeeds but doesn't provide any handles.
    *result = ZX_HANDLE_INVALID;
  }

  ldmsg_rsp_t rsp;
  memset(&rsp, 0, sizeof(rsp));

  zx_channel_call_args_t call = {
      .wr_bytes = &req,
      .wr_num_bytes = req_len,
      .wr_handles = &request_handle,
      .wr_num_handles = request_handle == ZX_HANDLE_INVALID ? 0 : 1,
      .rd_bytes = &rsp,
      .rd_num_bytes = sizeof(rsp),
      .rd_handles = result,
      .rd_num_handles = result == NULL ? 0 : 1,
  };

  uint32_t reply_size;
  uint32_t handle_count;
  status = _zx_channel_call(loader_svc, 0, ZX_TIME_INFINITE, &call, &reply_size, &handle_count);
  if (status != ZX_OK) {
    error("_zx_channel_call of %u bytes to loader service: %d (%s)", call.wr_num_bytes, status,
          _zx_status_get_string(status));
    return status;
  }

  size_t expected_reply_size = ldmsg_rsp_get_size(&rsp);
  if (reply_size != expected_reply_size) {
    error("loader service reply %u bytes != %u", reply_size, expected_reply_size);
    status = ZX_ERR_INVALID_ARGS;
    goto err;
  }
  if (rsp.header.ordinal != ordinal) {
    error("loader service reply opcode %u != %u", rsp.header.ordinal, ordinal);
    status = ZX_ERR_INVALID_ARGS;
    goto err;
  }
  if (rsp.rv != ZX_OK) {
    // |result| is non-null if |handle_count| > 0, because
    // |handle_count| <= |rd_num_handles|.
    if (handle_count > 0 && *result != ZX_HANDLE_INVALID) {
      error("loader service error %d reply contains handle %#x", rsp.rv, *result);
      status = ZX_ERR_INVALID_ARGS;
      goto err;
    }
    status = rsp.rv;
  }
  return status;

err:
  if (handle_count > 0) {
    _zx_handle_close(*result);
    *result = ZX_HANDLE_INVALID;
  }
  return status;
}

LIBC_NO_SAFESTACK static void loader_svc_config(const char* config) {
  zx_status_t status =
      loader_svc_rpc(LDMSG_OP_CONFIG, config, strlen(config), ZX_HANDLE_INVALID, NULL);
  if (status != ZX_OK)
    debugmsg("LDMSG_OP_CONFIG(%s): %s\n", config, _zx_status_get_string(status));
}

LIBC_NO_SAFESTACK static zx_status_t get_library_vmo(const char* name, zx_handle_t* result) {
  if (loader_svc == ZX_HANDLE_INVALID) {
    error("cannot look up \"%s\" with no loader service", name);
    return ZX_ERR_UNAVAILABLE;
  }
  return loader_svc_rpc(LDMSG_OP_LOAD_OBJECT, name, strlen(name), ZX_HANDLE_INVALID, result);
}

LIBC_NO_SAFESTACK zx_status_t dl_clone_loader_service(zx_handle_t* out) {
  if (loader_svc == ZX_HANDLE_INVALID) {
    return ZX_ERR_UNAVAILABLE;
  }
  zx_handle_t h0, h1;
  zx_status_t status;
  if ((status = _zx_channel_create(0, &h0, &h1)) != ZX_OK) {
    return status;
  }
  ldmsg_req_t req;
  size_t req_len;
  if ((status = ldmsg_req_encode(LDMSG_OP_CLONE, &req, &req_len, NULL, sizeof(req))) != ZX_OK) {
    return status;
  }

  ldmsg_rsp_t rsp;
  memset(&rsp, 0, sizeof(rsp));

  zx_channel_call_args_t call = {
      .wr_bytes = &req,
      .wr_num_bytes = (uint32_t)req_len,
      .wr_handles = &h1,
      .wr_num_handles = 1,
      .rd_bytes = &rsp,
      .rd_num_bytes = sizeof(rsp),
      .rd_handles = NULL,
      .rd_num_handles = 0,
  };
  uint32_t reply_size;
  uint32_t handle_count;
  if ((status = _zx_channel_call(loader_svc, 0, ZX_TIME_INFINITE, &call, &reply_size,
                                 &handle_count)) != ZX_OK) {
    // Do nothing.
  } else if ((reply_size != ldmsg_rsp_get_size(&rsp)) || (rsp.header.ordinal != LDMSG_OP_CLONE)) {
    status = ZX_ERR_INVALID_ARGS;
  } else if (rsp.rv != ZX_OK) {
    status = rsp.rv;
  }

  if (status != ZX_OK) {
    _zx_handle_close(h0);
  } else {
    *out = h0;
  }
  return status;
}

LIBC_NO_SAFESTACK __attribute__((__visibility__("hidden"))) void _dl_log_write(const char* buffer,
                                                                               size_t len) {
  if (logger != ZX_HANDLE_INVALID) {
    while (len > 0) {
      size_t chunk = len < ZX_LOG_RECORD_DATA_MAX ? len : ZX_LOG_RECORD_DATA_MAX;
      // Write only a single line at a time so each line gets tagged.
      const char* nl = memchr(buffer, '\n', chunk);
      if (nl != NULL) {
        chunk = nl + 1 - buffer;
      }
      zx_status_t status = _zx_debuglog_write(logger, 0, buffer, chunk);
      if (status != ZX_OK) {
        CRASH_WITH_UNIQUE_BACKTRACE();
      }
      buffer += chunk;
      len -= chunk;
    }
  } else {
    zx_status_t status = _zx_debug_write(buffer, len);
    if (status != ZX_OK) {
      CRASH_WITH_UNIQUE_BACKTRACE();
    }
  }
}

LIBC_NO_SAFESTACK static size_t errormsg_write(FILE* f, const unsigned char* buf, size_t len) {
  if (f != NULL && f->wpos > f->wbase) {
    _dl_log_write((const char*)f->wbase, f->wpos - f->wbase);
  }

  if (len > 0) {
    _dl_log_write((const char*)buf, len);
  }

  if (f != NULL) {
    f->wend = f->buf + f->buf_size;
    f->wpos = f->wbase = f->buf;
  }

  return len;
}

LIBC_NO_SAFESTACK static int errormsg_vprintf(const char* restrict fmt, va_list ap) {
  FILE f = {
      .lbf = EOF,
      .write = errormsg_write,
      .buf = (void*)fmt,
      .buf_size = 0,
      .lock = -1,
  };
  return vfprintf(&f, fmt, ap);
}

LIBC_NO_SAFESTACK static void debugmsg(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  errormsg_vprintf(fmt, ap);
  va_end(ap);
}

LIBC_NO_SAFESTACK static void error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (!runtime) {
    errormsg_vprintf(fmt, ap);
    ldso_fail = 1;
    va_end(ap);
    return;
  }
  __dl_vseterr(fmt, ap);
  va_end(ap);
}

zx_status_t __sanitizer_change_code_protection(uintptr_t addr, size_t len, bool writable) {
  static const char kBadDepsMessage[] =
      "module compiled with -fxray-instrument loaded in process without it";

  if (!KEEP_DSO_VMAR) {
    __sanitizer_log_write(kBadDepsMessage, sizeof(kBadDepsMessage) - 1);
    CRASH_WITH_UNIQUE_BACKTRACE();
  }

  struct dso* p;
  _dl_rdlock();
  p = addr2dso((size_t)__builtin_return_address(0));
  _dl_unlock();

  if (!p) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  if (addr < saddr(p, p->code_start) || len > saddr(p, p->code_end) - addr) {
    debugmsg("Cannot change protection outside of the code range\n");
    return ZX_ERR_OUT_OF_RANGE;
  }

  uint32_t options = ZX_VM_PERM_READ | ZX_VM_PERM_EXECUTE | (writable ? ZX_VM_PERM_WRITE : 0);
  zx_status_t status = _zx_vmar_protect(p->vmar, options, (zx_vaddr_t)addr, len);
  if (status != ZX_OK) {
    debugmsg("Failed to change protection of [%p, %p): %s\n", addr, addr + len,
             _zx_status_get_string(status));
  }
  return status;
}

// The _dl_rdlock is held or equivalent.
void _dl_locked_report_globals(sanitizer_memory_snapshot_callback_t* callback, void* callback_arg) {
  for (struct dso* mod = head; mod != NULL; mod = dso_next(mod)) {
    for (unsigned int i = 0; i < mod->phnum; ++i) {
      const Phdr* ph = &mod->phdr[i];
      // Report every nonempty writable segment.
      if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W)) {
        uintptr_t start = ph->p_vaddr;
        uintptr_t end = start + ph->p_memsz;
        // If this segment contains the RELRO region, exclude that leading
        // range of the segment.  With lld behavior, that's the entire segment
        // because RELRO gets a separate aligned segment.  With GNU behavior,
        // it's just a leading portion of the main writable segment.  lld uses
        // a page-rounded p_memsz for PT_GNU_RELRO (with the actual size in
        // p_filesz) unlike GNU linkers (where p_memsz==p_filesz), so
        // relro_end might actually be past end.
        if (mod->relro_start >= start && mod->relro_start <= end) {
          start = mod->relro_end < end ? mod->relro_end : end;
        }
        if (start < end) {
          callback(laddr(mod, start), end - start, callback_arg);
        }
      }
    }
  }
}

#ifdef __clang__
// Under -fsanitize-coverage, the startup code path before __dls3 cannot
// use PLT calls, so its calls to the sancov hook are a problem.  We use
// some assembler chicanery to redirect those calls to the local symbol
// _dynlink_sancov_trampoline.  Since the target of the PLT relocs is
// local, the linker will elide the PLT entry and resolve the calls
// directly to our definition.  The trampoline checks the 'runtime' flag to
// distinguish calls before final relocation is complete, and only calls
// into the sanitizer runtime once it's actually up.  Because of the
// .weakref chicanery, the _dynlink_sancov_* symbols must be in a separate
// assembly file.

#include "hwasan-stubs.h"
#include "sancov-stubs.h"
#include "sanitizer-stubs.h"

#define SANCOV_STUB(name) SANCOV_STUB_ASM("__sanitizer_cov_" #name)
#define SANCOV_STUB_ASM(name) SANITIZER_STUB_ASM(name, SANITIZER_STUB_ASM_BODY(name))

SANCOV_STUBS

#if __has_feature(hwaddress_sanitizer)
// HWASan stubs are needed here also to elide PLT relocations made during startup.
#define HWASAN_STUB(name) HWASAN_STUB_ASM("__hwasan_" #name)
#define HWASAN_STUB_ASM(name) SANITIZER_STUB_ASM(name, SANITIZER_STUB_ASM_BODY(name))
HWASAN_STUBS
#endif  // __has_feature(hwaddress_sanitizer)

#endif
