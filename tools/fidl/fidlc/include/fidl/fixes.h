// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDL_FIDLC_INCLUDE_FIDL_FIXES_H_
#define TOOLS_FIDL_FIDLC_INCLUDE_FIDL_FIXES_H_

#include <lib/fit/function.h>

#include <map>
#include <string_view>

#include "lib/fit/result.h"
#include "tools/fidl/fidlc/include/fidl/experimental_flags.h"
#include "tools/fidl/fidlc/include/fidl/fixables.h"
#include "tools/fidl/fidlc/include/fidl/raw_ast.h"
#include "tools/fidl/fidlc/include/fidl/reporter.h"
#include "tools/fidl/fidlc/include/fidl/source_manager.h"
#include "tools/fidl/fidlc/include/fidl/transformer.h"

namespace fidl::fix {

// Enumerates the kinds of failures that may occur during a fix attempt.
enum struct Status {
  // Everything is fine so far, but we're not yet done.
  kOk,
  // The library (and/or dependencies) provided had at least one non-fixable build error.
  kErrorPreFix,
  // The fix operation itself was a failure.
  kErrorDuringFix,
  // The post-fix formatting operation failed - something went seriously wrong during the fix.
  kErrorPostFix,
  // All other kinds of failures.
  kErrorOther,
  // The fix has completed successfully!
  kComplete,
};

// A map of filepaths to their transformed contents.
using OutputMap = std::map<const SourceFile*, std::string>;

// The result of a failed transform operation, containing both errors encountered, and the final
// |Status|.
struct Failure {
  const Status status;
  const std::vector<Error> errors;
};

using TransformResult = fit::result<Failure, OutputMap>;

// A "fix" is a transformation function, which takes some deprecated FIDL files and automatically
// upgrades them to some newer configuration. This could involve changing how things are spelled in
// the syntax, or more complex changes like back-porting support for new features.
class Fix {
 public:
  virtual ~Fix() = default;

  // Perform the actual transformation.
  virtual TransformResult Transform(Reporter* reporter) = 0;

  // Ensure that all of the flags required for this fix are set.
  Status ValidateFlags();

 protected:
  Fix(const Fixable fixable, const std::unique_ptr<SourceManager>& library,
      const ExperimentalFlags experimental_flags)
      : fixable_(fixable), library_(library), experimental_flags_(experimental_flags) {}

  // Get pointers to all of the source files.
  std::vector<const SourceFile*> GetSourceFiles();

  // Execute the steps of the owned |Transformer| in order, reporting errors along the way, and
  // prepare a |TransformResult| as needed.
  template <typename T>
  TransformResult Execute(std::unique_ptr<T> transformer,
                          std::vector<const SourceFile*> source_files, Reporter* reporter);

  const Fixable fixable_;
  const std::unique_ptr<SourceManager>& library_;
  const ExperimentalFlags experimental_flags_;
};

class ParsedFix : public Fix {
 public:
  ParsedFix(const Fixable fixable, const std::unique_ptr<SourceManager>& library,
            const ExperimentalFlags experimental_flags)
      : Fix(fixable, library, experimental_flags) {}

  TransformResult Transform(Reporter* reporter) final;

 protected:
  // Retrieve the appropriate transformer. Because each derivation will have a specific
  // |Transformer| derivation that it is targeting, this is a virtual class that must be overridden
  // by derived class implementations.
  virtual std::unique_ptr<ParsedTransformer> GetParsedTransformer(
      const std::vector<const SourceFile*> source_files,
      const fidl::ExperimentalFlags& experimental_flags, Reporter* reporter) = 0;

 private:
  using Fix::Execute;
  using Fix::GetSourceFiles;
  using Fix::library_;
};

// A fix that does nothing. This is retained both for testing purposes, and to ensure there is
// always at least one "example" |Fix| implementation, even when no active fixes are being
// performed.
class NoopParsedFix final : public ParsedFix {
 public:
  NoopParsedFix(const std::unique_ptr<SourceManager>& library,
                const ExperimentalFlags experimental_flags)
      : ParsedFix(Fixable::Get(Fixable::Kind::kNoop), library, experimental_flags) {}
  ~NoopParsedFix() = default;

 protected:
  std::unique_ptr<ParsedTransformer> GetParsedTransformer(
      std::vector<const SourceFile*> source_files,
      const fidl::ExperimentalFlags& experimental_flags, Reporter* reporter) final;
};

class ProtocolModifierFix final : public ParsedFix {
 public:
  ProtocolModifierFix(const std::unique_ptr<SourceManager>& library,
                      const ExperimentalFlags experimental_flags)
      : ParsedFix(Fixable::Get(Fixable::Kind::kProtocolModifier), library, experimental_flags) {}
  ~ProtocolModifierFix() = default;

 protected:
  std::unique_ptr<ParsedTransformer> GetParsedTransformer(
      std::vector<const SourceFile*> source_files,
      const fidl::ExperimentalFlags& experimental_flags, Reporter* reporter) final;
};

}  // namespace fidl::fix

#endif  // TOOLS_FIDL_FIDLC_INCLUDE_FIDL_FIXES_H_
