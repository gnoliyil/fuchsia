// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDL_FIDLC_INCLUDE_FIDL_DIAGNOSTIC_TYPES_H_
#define TOOLS_FIDL_FIDLC_INCLUDE_FIDL_DIAGNOSTIC_TYPES_H_

#include <zircon/assert.h>

#include <memory>
#include <set>
#include <sstream>
#include <string_view>

#include "tools/fidl/fidlc/include/fidl/fixables.h"
#include "tools/fidl/fidlc/include/fidl/flat_ast.h"
#include "tools/fidl/fidlc/include/fidl/program_invocation.h"
#include "tools/fidl/fidlc/include/fidl/source_span.h"
#include "tools/fidl/fidlc/include/fidl/token.h"
#include "tools/fidl/fidlc/include/fidl/types.h"
#include "tools/fidl/fidlc/include/fidl/utils.h"
#include "tools/fidl/fidlc/include/fidl/versioning_types.h"

namespace fidl {

// Forward decls
namespace raw {
class AttributeList;
}  // namespace raw

using ErrorId = uint32_t;

namespace internal {

constexpr std::string_view kFormatMarker = "{}";

std::string Display(char c);
std::string Display(const std::string& s);
std::string Display(std::string_view s);
std::string Display(const std::set<std::string>& s);
std::string Display(const std::set<std::string_view>& s);
std::string Display(const SourceSpan& s);
std::string Display(const Token::KindAndSubkind& t);
std::string Display(types::Openness o);
std::string Display(const raw::AttributeList* a);
std::string Display(const std::vector<std::string_view>& library_name);
std::string Display(const flat::Attribute* a);
std::string Display(const flat::AttributeArg* a);
std::string Display(const flat::Constant* c);
std::string Display(flat::Element::Kind k);
std::string Display(flat::Decl::Kind k);
std::string Display(const flat::Element* e);
std::string Display(std::vector<const flat::Decl*>& d);
std::string Display(const flat::Type* t);
std::string Display(const flat::Name& n);
std::string Display(const Platform& p);
std::string Display(const Version& v);
std::string Display(const VersionRange& r);
std::string Display(const VersionSet& s);
template <typename T, typename = decltype(std::to_string(std::declval<T>()))>
std::string Display(T val) {
  return std::to_string(val);
}

inline void FormatHelper(std::stringstream& out, std::string_view msg) {
  ZX_ASSERT(msg.find(kFormatMarker) == std::string::npos);
  out << msg;
}

template <typename T, typename... Rest>
void FormatHelper(std::stringstream& out, std::string_view msg, T t, const Rest&... rest) {
  auto i = msg.find(kFormatMarker);
  ZX_ASSERT(i != std::string::npos);
  out << msg.substr(0, i) << Display(t);
  auto remaining_msg = msg.substr(i + kFormatMarker.size());
  FormatHelper(out, remaining_msg, rest...);
}

template <typename... Args>
std::string FormatDiagnostic(std::string_view msg, const Args&... args) {
  std::stringstream s;
  FormatHelper(s, msg, args...);
  return s.str();
}

constexpr size_t CountFormatArgs(std::string_view s) {
  size_t i = s.find(kFormatMarker, 0);
  size_t total = 0;
  while (i != std::string::npos) {
    total++;
    i = s.find(kFormatMarker, i + kFormatMarker.size());
  }
  return total;
}

// No-op non-constexpr function used to produce an error.
inline void IncorrectNumberOfFormatArgs() {}

template <typename... Args>
constexpr void CheckFormatArgs(std::string_view msg) {
  static_assert(
      (std::is_same_v<Args, std::remove_const_t<std::remove_reference_t<Args>>> && ...),
      "remove redundant `const` or `&`; DiagnosticDef args are always passed by const reference");
  static_assert(((!std::is_pointer_v<Args> || std::is_const_v<std::remove_pointer_t<Args>>)&&...),
                "use a const pointer; DiagnosticDef args should not be mutable pointers");
  static_assert(((!std::is_same_v<Args, std::string>)&&...),
                "use std::string_view, not std::string");

  // We can't static_assert below because the compiler doesn't know msg is
  // always constexpr. Instead, we generate a "must be initialized by a constant
  // expression" error by calling a non-constexpr function. The error only
  // happens if the condition is true. Otherwise, the call gets eliminated.
  if (sizeof...(Args) != internal::CountFormatArgs(msg)) {
    IncorrectNumberOfFormatArgs();
  }
}

}  // namespace internal

using utils::identity_t;

// A tag that indicates whether a diagnostic definition is an error or warning.
// In the future this could be extended to include hints, suggestions, etc.
enum class DiagnosticKind {
  kError,
  kWarning,
  kRetired,
};

// Extra fields set on some diagnostic definitions.
struct DiagnosticOptions {
  // If true, the error message will link to the diagnostic on fuchsia.dev.
  bool documented = true;
  // If set, the error message will instruct the user to run fidl-fix.
  std::optional<Fixable::Kind> fixable;
};

struct DiagnosticDef {
  constexpr explicit DiagnosticDef(ErrorId id, DiagnosticKind kind, std::string_view msg,
                                   DiagnosticOptions opts)
      : id(id), kind(kind), msg(msg), opts(opts) {}
  DiagnosticDef(const DiagnosticDef&) = delete;

  // Returns a string of the form "fi-NNNN".
  std::string FormatId() const;

  ErrorId id;
  DiagnosticKind kind;
  std::string_view msg;
  DiagnosticOptions opts;
};

// The definition of an error. All instances of ErrorDef are in diagnostics.h.
// Template args define format parameters in the error message.
template <ErrorId Id, typename... Args>
struct ErrorDef final : DiagnosticDef {
  constexpr explicit ErrorDef(std::string_view msg, DiagnosticOptions opts = {})
      : DiagnosticDef(Id, DiagnosticKind::kError, msg, opts) {
    internal::CheckFormatArgs<Args...>(msg);
  }
};

// The definition of a warning. All instances of WarningDef are in
// diagnostics.h. Template args define format parameters in the warning message.
template <ErrorId Id, typename... Args>
struct WarningDef final : DiagnosticDef {
  constexpr explicit WarningDef(std::string_view msg, DiagnosticOptions opts = {})
      : DiagnosticDef(Id, DiagnosticKind::kWarning, msg, opts) {
    internal::CheckFormatArgs<Args...>(msg);
  }
};

// The definition of an obsolete error. These are never displayed to the user -
// they are merely used to retire error numerals from circulation.
template <ErrorId Id>
struct RetiredDef final : DiagnosticDef {
  constexpr explicit RetiredDef()
      : DiagnosticDef(Id, DiagnosticKind::kRetired, "retired diagnostic", {}) {}
};

// A Diagnostic is the result of instantiating a DiagnosticDef with arguments.
// It stores a formatted std::string where "{}" markers have been replaced by
// arguments. It also stores a SourceSpan indicating where the problem occurred.
struct Diagnostic {
  template <typename... Args>
  Diagnostic(const DiagnosticDef& def, SourceSpan span, const Args&... args)
      : def(def), span(span), msg(internal::FormatDiagnostic(def.msg, args...)) {}
  Diagnostic(const Diagnostic&) = delete;

  // The factory functions below could be constructors, and std::make_unique
  // would work fine. However, template error messages are better with static
  // functions because it doesn't have to try every constructor.

  template <ErrorId Id, typename... Args>
  static std::unique_ptr<Diagnostic> MakeError(const ErrorDef<Id, Args...>& def, SourceSpan span,
                                               const identity_t<Args>&... args) {
    return std::make_unique<Diagnostic>(def, span, args...);
  }

  template <ErrorId Id, typename... Args>
  static std::unique_ptr<Diagnostic> MakeWarning(const WarningDef<Id, Args...>& def,
                                                 SourceSpan span, const identity_t<Args>&... args) {
    return std::make_unique<Diagnostic>(def, span, args...);
  }

  // Formats the error message to a string.
  std::string Format(const ProgramInvocation& program_invocation) const;

  const DiagnosticDef& def;
  const SourceSpan span;
  const std::string msg;
};

}  // namespace fidl

#endif  // TOOLS_FIDL_FIDLC_INCLUDE_FIDL_DIAGNOSTIC_TYPES_H_
