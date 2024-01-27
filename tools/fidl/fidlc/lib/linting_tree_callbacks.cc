// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/fidl/fidlc/include/fidl/linting_tree_callbacks.h"

#include <zircon/assert.h>

#include <fstream>

#include <re2/re2.h>

#include "tools/fidl/fidlc/include/fidl/utils.h"

namespace fidl::linter {

LintingTreeCallbacks::LintingTreeCallbacks() {
  // Anonymous derived class; unique to the LintingTreeCallbacks
  class CallbackTreeVisitor : public fidl::raw::DeclarationOrderTreeVisitor {
   private:
    int gap_subre_count = 1;  // index 0 unused, since match starts with 1
    // Regex OR expression should try line comment first:
    const int kLineComment = gap_subre_count++;
    const int kIgnoredToken = gap_subre_count++;
    const int kWhiteSpace = gap_subre_count++;

   public:
    explicit CallbackTreeVisitor(const LintingTreeCallbacks& callbacks)
        : callbacks_(callbacks), kGapTextRegex_(GapTextRegex()) {}

    void OnFile(const std::unique_ptr<raw::File>& element) override {
      for (auto& callback : callbacks_.file_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnFile(element);
      for (auto& callback : callbacks_.exit_file_callbacks_) {
        callback(*element);
      }
    }
    void OnSourceElementStart(const raw::SourceElement& element) override {
      ProcessGapText(element.start());
      for (auto& callback : callbacks_.source_element_callbacks_) {
        callback(element);
      }
    }
    void OnSourceElementEnd(const raw::SourceElement& element) override {
      ProcessGapText(element.end());
    }
    void OnAliasDeclaration(const std::unique_ptr<raw::AliasDeclaration>& element) override {
      ProcessGapText(element->start());
      for (auto& callback : callbacks_.alias_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnAliasDeclaration(element);
      ProcessGapText(element->end());
    }
    void OnUsing(const std::unique_ptr<raw::Using>& element) override {
      ProcessGapText(element->start());
      for (auto& callback : callbacks_.using_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnUsing(element);
      ProcessGapText(element->end());
    }
    void OnConstDeclaration(const std::unique_ptr<raw::ConstDeclaration>& element) override {
      ProcessGapText(element->start());
      for (auto& callback : callbacks_.const_declaration_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnConstDeclaration(element);
      for (auto& callback : callbacks_.exit_const_declaration_callbacks_) {
        callback(*element);
      }
      ProcessGapText(element->end());
    }
    void OnProtocolDeclaration(const std::unique_ptr<raw::ProtocolDeclaration>& element) override {
      ProcessGapText(element->start());
      for (auto& callback : callbacks_.protocol_declaration_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnProtocolDeclaration(element);
      for (auto& callback : callbacks_.exit_protocol_declaration_callbacks_) {
        callback(*element);
      }
      ProcessGapText(element->end());
    }
    void OnProtocolMethod(const std::unique_ptr<raw::ProtocolMethod>& element) override {
      ProcessGapText(element->start());
      if (element->maybe_request != nullptr) {
        for (auto& callback : callbacks_.method_callbacks_) {
          callback(*element);
        }
      } else {
        for (auto& callback : callbacks_.event_callbacks_) {
          callback(*element);
        }
      }
      DeclarationOrderTreeVisitor::OnProtocolMethod(element);
      ProcessGapText(element->end());
    }
    void OnAttribute(const std::unique_ptr<raw::Attribute>& element) override {
      for (auto& callback : callbacks_.attribute_callbacks_) {
        callback(*element);
      }
    }
    void OnOrdinaledLayoutMember(
        const std::unique_ptr<raw::OrdinaledLayoutMember>& element) override {
      ProcessGapText(element->start());
      for (auto& callback : callbacks_.ordinaled_layout_member_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnOrdinaledLayoutMember(element);
      ProcessGapText(element->end());
    }
    void OnStructLayoutMember(const std::unique_ptr<raw::StructLayoutMember>& element) override {
      ProcessGapText(element->start());
      for (auto& callback : callbacks_.struct_layout_member_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnStructLayoutMember(element);
      ProcessGapText(element->end());
    }
    void OnValueLayoutMember(const std::unique_ptr<raw::ValueLayoutMember>& element) override {
      ProcessGapText(element->start());
      for (auto& callback : callbacks_.value_layout_member_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnValueLayoutMember(element);
      ProcessGapText(element->end());
    }
    void OnLayout(const std::unique_ptr<raw::Layout>& element) override {
      ProcessGapText(element->start());
      for (auto& callback : callbacks_.layout_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnLayout(element);
      for (auto& callback : callbacks_.exit_layout_callbacks_) {
        callback(*element);
      }
      ProcessGapText(element->end());
    }
    void OnTypeDeclaration(const std::unique_ptr<raw::TypeDeclaration>& element) override {
      ProcessGapText(element->start());
      for (auto& callback : callbacks_.type_decl_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnTypeDeclaration(element);
      for (auto& callback : callbacks_.exit_type_decl_callbacks_) {
        callback(*element);
      }
      ProcessGapText(element->end());
    }
    void OnIdentifierLayoutParameter(
        const std::unique_ptr<raw::IdentifierLayoutParameter>& element) override {
      // For the time being, the the first type parameter in a layout must either be a
      // TypeConstructor (like `vector<uint8>`), or else a reference to on (like `vector<Foo>`).
      // Because of this, we can treat an IdentifierLayoutParameter as a TypeConstructor for the
      // purposes of linting.
      for (auto& callback : callbacks_.identifier_layout_parameter_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnIdentifierLayoutParameter(element);
    }
    void OnTypeConstructor(const std::unique_ptr<raw::TypeConstructor>& element) override {
      for (auto& callback : callbacks_.type_constructor_callbacks_) {
        callback(*element);
      }
      DeclarationOrderTreeVisitor::OnTypeConstructor(element);
    }
    // --- end new syntax ---

   private:
    std::string GapTextRegex() {
      std::string subre[gap_subre_count];

      // Regex OR expression should try line comment first:
      subre[kLineComment] = R"REGEX(//(?:\S*[ \t]*\S+)*)REGEX";
      subre[kIgnoredToken] = R"REGEX(\S+)REGEX";
      subre[kWhiteSpace] = R"REGEX((?:[ \t]+\n?)|\n)REGEX";
      // white space spanning multiple lines will be split on the
      // newline, with the newline included.

      return "^(" + subre[kLineComment] + ")|(" + subre[kIgnoredToken] + ")|(" +
             subre[kWhiteSpace] + ")";
    }

    // "GapText" includes everything between source elements (or between a
    // source element and the beginning or the end of the file). This
    // includes whitespace, comments, and certain tokens not processed as
    // source elements, including colons, curly braces, square brackets,
    // parentheses, commas, and semicolons.
    //
    // Break up the gap text into the different types to pass to the
    // appropriate callbacks. Include the leading characters on the line
    // as an additional parameter, to give callbacks some insight into
    // their line context. For example, is the whitespace preceded by
    // a line comment (meaning it is not a blank line)? Is the line comment
    // preceded by any non-whitespace characters (it is not a comment-
    // only line)?
    void OnGapText(std::string_view gap_view, const SourceFile& source_file,
                   std::string_view line_so_far_view) {
      auto remaining_gap_view = gap_view;
      auto remaining_line_so_far_view = line_so_far_view;
      re2::StringPiece match[gap_subre_count];
      while (!remaining_gap_view.empty()) {
        // The regex_search loop should consume the entire gap
        bool found = kGapTextRegex_.Match(remaining_gap_view, 0, remaining_gap_view.size(),
                                          re2::RE2::UNANCHORED, match, gap_subre_count);
        ZX_ASSERT_MSG(found, "gap content did not match any of the expected regular expressions");

        auto view = remaining_gap_view;
        view.remove_suffix(remaining_gap_view.size() - match[0].length());
        auto line_prefix_view = remaining_line_so_far_view;
        line_prefix_view.remove_suffix(remaining_gap_view.size());

        if (!match[kLineComment].empty()) {
          // TODO(fxbug.dev/7979): Remove FirstLineIsRegularComment() check
          // when no longer needed.
          // If there are multiple contiguous lines starting with the
          // doc comment marker ("///"), they will be merged (including
          // newlines, but with the 3 slashes stripped off) into a single
          // string, and generate an "Attribute" with |name| "Doc", and
          // |value| the multi-line string. BUT the span()
          // std::string_view for the Attribute SourceElement has ONLY the
          // first line. So when LintingTreeCallbacks processes the |gap_text|,
          // the first line is not part of the gap, but the remaining lines
          // show up as gap text comments.
          if (utils::FirstLineIsRegularComment(view)) {  // not Doc Comment
            auto line_comment = SourceSpan(view, source_file);
            for (auto& callback : callbacks_.line_comment_callbacks_) {
              // starts with the comment marker (2 slashes) and ends with
              // the last non-space character before the first newline
              callback(line_comment, line_prefix_view);
            }
          }
        } else if (!match[kIgnoredToken].empty()) {
          auto ignored_token = SourceSpan(view, source_file);
          for (auto& callback : callbacks_.ignored_token_callbacks_) {
            // includes (but may not be limited to): "as" : ; , { } [ ] ( )
            callback(ignored_token);
          }
        } else if (!match[kWhiteSpace].empty()) {
          auto white_space = SourceSpan(view, source_file);
          for (auto& callback : callbacks_.white_space_up_to_newline_callbacks_) {
            // All whitespace only (space, tab, newline)
            callback(white_space, line_prefix_view);
          }
        } else {
          ZX_PANIC("should never be reached; bad regex?");
        }
        if (view.back() == '\n') {
          remaining_line_so_far_view.remove_prefix(
              (view.data() - remaining_line_so_far_view.data()) + view.size());
        }
        remaining_gap_view.remove_prefix(match[0].length());
      }
    }

    void ProcessGapText(const fidl::Token& next_token) {
      const char* gap_start = next_token.previous_end().data().data();
      if (gap_start > end_of_last_gap_) {
        auto const& source_file = next_token.span().source_file();
        auto const& source_view = source_file.data();
        const char* source_start = source_view.data();
        const char* source_end = source_view.data() + source_view.size();

        if (gap_start > end_of_last_token_) {
          if (end_of_last_token_ == nullptr) {
            gap_start = source_start;
          } else {
            gap_start = end_of_last_token_;
          }
        }

        // The gap starts from the end of the last processed token and
        // ends with the beginning of the next token to be processed.
        // It is then broken down into either contiguous whitespace,
        // a line comment (excluding trailing whitespace at the end of
        // the line), or other characters (also called "ignored tokens,"
        // which can include the word "as", and various punctuation.)
        auto next_view = next_token.data();
        const char* gap_end = next_view.data();
        auto gap_view = source_view;
        gap_view.remove_prefix(gap_start - source_start);
        gap_view.remove_suffix(source_end - gap_end);

        // Get a view of the gap PLUS characters prior to the gap up to the
        // beginning of the line. There may still be multiple lines in the
        // gap, but the first line in the gap will be a complete line.
        // (The last line in the gap is typically not the complete line,
        // unless the next token happens to start on column 1 of the next line.
        auto line_so_far_view = source_view;
        line_so_far_view.remove_suffix(source_end - gap_end);
        if (gap_start > source_start) {
          auto preceding_newline =
              line_so_far_view.find_last_of('\n', gap_start - source_start - 1);
          if (preceding_newline != std::string_view::npos) {
            line_so_far_view.remove_prefix(preceding_newline + 1);
          }
        }

        OnGapText(gap_view, source_file, line_so_far_view);
        end_of_last_gap_ = gap_end;
        end_of_last_token_ = gap_end + next_view.size();
      }
    }

    const LintingTreeCallbacks& callbacks_;
    re2::RE2 kGapTextRegex_;
    const char* end_of_last_gap_ = nullptr;
    const char* end_of_last_token_ = nullptr;
  };

  tree_visitor_ = std::make_unique<CallbackTreeVisitor>(*this);
}  // namespace linter

void LintingTreeCallbacks::Visit(const std::unique_ptr<raw::File>& element) {
  tree_visitor_->OnFile(element);
}

}  // namespace fidl::linter
