// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_LIBC_INCLUDE_STDIO_H_
#define ZIRCON_KERNEL_LIB_LIBC_INCLUDE_STDIO_H_

#include <stdarg.h>
#include <stddef.h>
#include <zircon/compiler.h>

// All anybody really wants from stdio is printf.

#ifdef __cplusplus
#include <ktl/string_view.h>

class FILE {
 public:
  // This is basically equivalent to having a virtual Write function with
  // subclasses providing their own data members in lieu of ptr.  But it's
  // simpler and avoids a vtable that might need address fixup at load time
  // (and the double indirection for a single-entry vtable--at the cost of
  // double indirection for the ptr data in a callback that uses it).

  using Callback = int(void*, ktl::string_view);

  FILE() = default;

  constexpr FILE(Callback* write, void* ptr) : write_(write), ptr_(ptr) {}

  template <typename T>
  explicit FILE(T* writer)
      : write_([](void* ptr, ktl::string_view s) { return static_cast<T*>(ptr)->Write(s); }),
        ptr_(writer) {}

  // This is what fprintf calls to do output.
  int Write(ktl::string_view s) { return write_(ptr_, s); }

  constexpr explicit operator bool() const { return write_; }

  constexpr bool operator==(const FILE& other) const {
    return write_ == other.write_ && ptr_ == other.ptr_;
  }

  constexpr bool operator!=(const FILE& other) const { return !(*this == other); }

  // This is not defined by libc itself.  The kernel defines it to point at
  // the default console output mechanism.
  static FILE stdout_;

 private:
  Callback* write_ = nullptr;
  void* ptr_ = nullptr;
};

#define stdout (&FILE::stdout_)

inline int fputc(int c, FILE* f) {
  const unsigned char uc = static_cast<unsigned char>(c);
  return f->Write({reinterpret_cast<const char*>(&uc), 1}) == 1 ? uc : -1;
}

inline int putc(int c, FILE* f) { return fputc(c, f); }

inline int putchar(int c) { return fputc(c, stdout); }

inline int fputs(const char* s, FILE* f) {
  ktl::string_view str(s);
  return f->Write(str) == static_cast<int>(str.size()) ? 0 : -1;
}

inline int puts(const char* s) { return fputs(s, stdout) == 0 ? putchar('\n') : -1; }

#else  // !__cplusplus

// C users just need the function declarations.
typedef struct _FILE_is_opaque FILE;

#endif  // __cplusplus

__BEGIN_CDECLS

int printf(const char*, ...) __PRINTFLIKE(1, 2);
int fprintf(FILE*, const char*, ...) __PRINTFLIKE(2, 3);
int snprintf(char* buf, size_t len, const char*, ...) __PRINTFLIKE(3, 4);

int vprintf(const char*, va_list);
int vfprintf(FILE*, const char*, va_list);
int vsnprintf(char* buf, size_t len, const char*, va_list);

__END_CDECLS

#if DISABLE_DEBUG_OUTPUT
// The declarations stand so these can be used without parens to get
// the real functions (e.g. &printf or (printf)(...)).
#define printf(...)
#define vprintf(fmt, args)
#endif

#endif  // ZIRCON_KERNEL_LIB_LIBC_INCLUDE_STDIO_H_
