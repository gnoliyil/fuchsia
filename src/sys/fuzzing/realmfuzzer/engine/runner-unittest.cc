// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/fuzzing/realmfuzzer/engine/runner.h"

#include "src/sys/fuzzing/common/options.h"
#include "src/sys/fuzzing/realmfuzzer/engine/runner-test.h"

namespace fuzzing {
namespace {

TEST_F(RealmFuzzerRunnerTest, LoadCorpus) {
  // In a real fuzzer, the parameters would be supplied by the 'program.args' from the adapter's
  // component manifest.
  //
  // See also:
  //   //src/sys/fuzzing/realmfuzzer/testing/data/BUILD.gn
  SetAdapterParameters(std::vector<std::string>({"data/corpus", "--ignored"}));
  Configure(MakeOptions());
  // Results are sorted.
  auto seed_corpus = runner()->GetCorpus(CorpusType::SEED);
  ASSERT_EQ(seed_corpus.size(), 2000U);

  // Inputs are sorted by contents, so specific inputs are at predictable indices, e.g.
  // 1999 == (2 * 676) + (24 * 26) + 23
  EXPECT_EQ(seed_corpus[0], Input("aaa"));
  EXPECT_EQ(seed_corpus[1999], Input("cyx"));
}

#define RUNNER_TYPE RealmFuzzerRunner
#define RUNNER_TEST RealmFuzzerRunnerTest
#include "src/sys/fuzzing/common/runner-unittest.inc"
#undef RUNNER_TYPE
#undef RUNNER_TEST

TEST_F(RealmFuzzerRunnerTest, MergeSeedError) {
  MergeSeedError(/* expected */ ZX_ERR_INVALID_ARGS);
}

TEST_F(RealmFuzzerRunnerTest, Merge) { Merge(/* keep_errors= */ true); }

}  // namespace
}  // namespace fuzzing
