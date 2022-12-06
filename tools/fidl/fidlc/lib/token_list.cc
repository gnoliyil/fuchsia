// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidl/fidlc/include/fidl/token_list.h"

#include <map>

#include "tools/fidl/fidlc/include/fidl/raw_ast.h"

namespace fidl::raw {

TokenPointerList TokenPointerListBuilder::Build() {
  ZX_DEBUG_ASSERT(last_added_ordinal_ == 0);
  // When we start, |last_added_ordinal_| is 0, implying that the first token has already been
  // added. Ensure that this is true.
  if (ast_->start().ordinal() == 0) {
    building_.push_back(&ast_->start());
  } else {
    // The AST always has the libary token as its |start()|, however it may have a comment before
    // the library declaration. If library isn't the first thing in the file, we need to start from
    // the actual first token.
    building_.push_back(&ast_->tokens[last_added_ordinal_]);
  }

  // Because we inherit from |DeclarationOrderTreeVisitor|, we will visit every |Token| held by the
  // raw AST (ie, demarcating the start or end of a |SourceElement|, rather than just in the
  // |tokens| vector).
  OnFile(ast_);

  // Walk until the end of the tokens vector, to get capture trailing comments.
  last_added_ordinal_++;
  while (last_added_ordinal_ < ast_->tokens.size()) {
    building_.push_back(&ast_->tokens[last_added_ordinal_]);
    last_added_ordinal_++;
  }
  ZX_ASSERT(ast_->tokens.size() == building_.size());
  return building_;
}

void TokenPointerListBuilder::OnSourceElementStart(const SourceElement& element) {
  OnToken(&element.start());
}

void TokenPointerListBuilder::OnSourceElementEnd(const SourceElement& element) {
  OnToken(&element.end());
}

void TokenPointerListBuilder::OnToken(const Token* token) {
  ZX_ASSERT(last_added_ordinal_ <= token->ordinal());
  if (last_added_ordinal_ == token->ordinal()) {
    // Already in the pointer list, so ensure we don't add it twice.
    return;
  }

  // All of these are discarded tokens not held in the AST. Reference directly into the Token.
  last_added_ordinal_++;
  while (last_added_ordinal_ < token->ordinal()) {
    building_.push_back(&ast_->tokens[last_added_ordinal_]);
    last_added_ordinal_++;
  }

  // This token is held by the raw AST, so point to that instead.
  building_.push_back(token);
}

}  // namespace fidl::raw
