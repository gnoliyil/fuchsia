// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidl/fidlc/include/fidl/raw_ast.h"
#include "tools/fidl/fidlc/include/fidl/token.h"
#include "tools/fidl/fidlc/include/fidl/tree_visitor.h"

#ifndef TOOLS_FIDL_FIDLC_INCLUDE_FIDL_TOKEN_LIST_H_
#define TOOLS_FIDL_FIDLC_INCLUDE_FIDL_TOKEN_LIST_H_

namespace fidl::raw {

// A list of pointers to every |Token| in a particular source file. It does not own the |Token|s it
// points to - those are all held in some raw AST (|raw::File|, to be exact) that must outlive this
// vector.
//
// Every unique |Token| in the owning raw AST is represented exactly once in this vector. Each
// |Token| may be sourced from one of two locations in that raw AST:
//
//   1. |Token|s which appear at least once in the raw AST as either |start_| or |end_| nodes are
//      included once for unique |Token|. Each such |Token| pointer points directly into the raw AST
//      proper.
//   2. |Token|s that never appear in the raw AST proper are plucked from the |raw::File.tokens|
//      side list, which lists all of the |Token|s that appeared in the file. Things like equal
//      signs, brackets, or comments may appear here.
//
// As an example, consider the following declaration-less FIDL library:
//
//   library example.lib;
//
// This produces 6 tokens arranged into the following |TokenPointerList|:
//
//   0. "library" -> pointing to the |raw::File.start_| node.
//   1. "example" -> pointing to the |raw::CompoundIdentifier.start_| node.
//   2. "." -> pointing to the |raw::File.tokens| side list.
//   3. "lib" -> pointing to the |raw::CompoundIdentifier.end_| node.
//   4. ";" -> pointing to the |raw::File.tokens| side list.
//   5. EOF -> pointing to the |raw::File.end_| node.
using TokenPointerList = std::vector<const Token*>;

class TokenPointerListBuilder final : public DeclarationOrderTreeVisitor {
 public:
  explicit TokenPointerListBuilder(const std::unique_ptr<raw::File>& ast) : ast_(ast) {
    building_.reserve(ast->tokens.size());
  }

  TokenPointerList Build();

 private:
  void OnSourceElementStart(const SourceElement&) override;
  void OnSourceElementEnd(const SourceElement&) override;
  void OnToken(const Token*);

  const std::unique_ptr<raw::File>& ast_;
  TokenPointerList building_;

  // Represents the ordinal of the last token we added to the |TokenPointerList| being built.
  size_t last_added_ordinal_ = 0;
};

}  // namespace fidl::raw

#endif  // TOOLS_FIDL_FIDLC_INCLUDE_FIDL_TOKEN_LIST_H_
