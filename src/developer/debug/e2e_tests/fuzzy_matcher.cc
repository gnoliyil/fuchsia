// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/e2e_tests/fuzzy_matcher.h"

#include <iostream>
#include <string>

namespace zxdb {

bool FuzzyMatcher::MatchesLine(std::initializer_list<std::string_view> substrs) {
  std::string content_for_debug;
  while (content_) {
    std::string line;
    std::getline(content_, line);
    content_for_debug += line + "\n";

    size_t pos = 0;
    for (auto& substr : substrs) {
      pos = line.find(substr, pos);
      if (pos == std::string::npos)
        break;
    }
    if (pos != std::string::npos)
      return true;
  }
  std::cerr << "Cannot find pattern { ";
  for (auto& substr : substrs)
    std::cerr << "\"" << substr << "\" ";
  std::cerr << "} in the following content:\n" << content_for_debug;
  return false;
}

}  // namespace zxdb
