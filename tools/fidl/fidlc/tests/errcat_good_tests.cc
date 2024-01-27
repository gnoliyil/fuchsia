// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zxtest/zxtest.h>

#include "tools/fidl/fidlc/tests/error_test.h"
#include "tools/fidl/fidlc/tests/test_library.h"

// This file is meant to hold standalone tests for each of the "good" examples used in the documents
// at //docs/reference/fidl/language/error-catalog. These cases are redundant with the other tests
// in this suite - their purpose is not to serve as tests for the features at hand, but rather to
// provide well-vetted and tested examples of the "correct" way to fix FIDL errors.

namespace {

// LINT.IfChange

TEST(ConstsTests, Good0003) {
  TestLibrary library;
  library.AddFile("good/fi-0003.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ConstsTests, Good0004) {
  TestLibrary library;
  library.AddFile("good/fi-0004.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0007) {
  TestLibrary library;
  library.AddFile("good/fi-0007.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0009) {
  TestLibrary library;
  library.AddFile("good/fi-0009.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0010a) {
  TestLibrary library;
  library.AddFile("good/fi-0010-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0011) {
  TestLibrary library;
  library.AddFile("good/fi-0011.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0012) {
  TestLibrary library;
  library.AddFile("good/fi-0012.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0013) {
  TestLibrary library;
  library.AddFile("good/fi-0013.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0014) {
  TestLibrary library;
  library.AddFile("good/fi-0014.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0015) {
  TestLibrary library;
  library.AddFile("good/fi-0015.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0016) {
  TestLibrary library;
  library.AddFile("good/fi-0016.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0017) {
  TestLibrary library;
  library.AddFile("good/fi-0017.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0020) {
  TestLibrary library;
  library.AddFile("good/fi-0020.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0022) {
  TestLibrary library;
  library.AddFile("good/fi-0022.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0023) {
  TestLibrary library;
  library.AddFile("good/fi-0023.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0025) {
  SharedAmongstLibraries shared;
  TestLibrary dependency(&shared, "dependent.fidl", R"FIDL(library dependent;

type Something = struct {};
)FIDL");
  ASSERT_COMPILED(dependency);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0025.test.fidl");
}

TEST(ErrcatTests, Good0028a) {
  TestLibrary library;
  library.AddFile("good/fi-0028-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0029) {
  TestLibrary library;
  library.AddFile("good/fi-0029.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0030) {
  TestLibrary library;
  library.AddFile("good/fi-0030.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0031) {
  TestLibrary library;
  library.AddFile("good/fi-0031.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0032) {
  TestLibrary library;
  library.AddFile("good/fi-0032.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0038ab) {
  SharedAmongstLibraries shared;
  TestLibrary dependency(&shared);
  dependency.AddFile("good/fi-0038-a.test.fidl");
  ASSERT_COMPILED(dependency);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0038-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0038ac) {
  SharedAmongstLibraries shared;
  TestLibrary dependency(&shared);
  dependency.AddFile("good/fi-0038-a.test.fidl");
  ASSERT_COMPILED(dependency);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0038-c.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0039ab) {
  SharedAmongstLibraries shared;
  TestLibrary dependency(&shared);
  dependency.AddFile("good/fi-0039-a.test.fidl");
  ASSERT_COMPILED(dependency);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0039-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0039ac) {
  SharedAmongstLibraries shared;
  TestLibrary dependency(&shared);
  dependency.AddFile("good/fi-0039-a.test.fidl");
  ASSERT_COMPILED(dependency);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0039-c.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0041a) {
  TestLibrary library;
  library.AddFile("good/fi-0041-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0041b) {
  TestLibrary library;
  library.AddFile("good/fi-0041-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0042) {
  SharedAmongstLibraries shared;
  TestLibrary dependency(&shared);
  dependency.AddFile("good/fi-0042-a.test.fidl");
  ASSERT_COMPILED(dependency);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0042-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0043) {
  SharedAmongstLibraries shared;
  TestLibrary dependency1(&shared);
  dependency1.AddFile("good/fi-0043-a.test.fidl");
  ASSERT_COMPILED(dependency1);
  TestLibrary dependency2(&shared);
  dependency2.AddFile("good/fi-0043-b.test.fidl");
  ASSERT_COMPILED(dependency2);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0043-c.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0044) {
  SharedAmongstLibraries shared;
  TestLibrary dependency1(&shared);
  dependency1.AddFile("good/fi-0044-a.test.fidl");
  ASSERT_COMPILED(dependency1);
  TestLibrary dependency2(&shared);
  dependency2.AddFile("good/fi-0044-b.test.fidl");
  ASSERT_COMPILED(dependency2);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0044-c.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0045) {
  SharedAmongstLibraries shared;
  TestLibrary dependency(&shared);
  dependency.AddFile("good/fi-0045-a.test.fidl");
  ASSERT_COMPILED(dependency);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0045-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0046) {
  TestLibrary library;
  library.AddFile("good/fi-0046.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0047) {
  TestLibrary library;
  library.AddFile("good/fi-0047.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0048) {
  TestLibrary library;
  library.AddFile("good/fi-0048.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0049) {
  TestLibrary library;
  library.AddFile("good/fi-0049.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0050) {
  TestLibrary library;
  library.AddFile("good/fi-0050.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0051) {
  SharedAmongstLibraries shared;
  TestLibrary dependency(&shared);
  dependency.AddFile("good/fi-0051-a.test.fidl");
  ASSERT_COMPILED(dependency);
  TestLibrary library(&shared);
  library.AddFile("good/fi-0051-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0052a) {
  TestLibrary library;
  library.AddFile("good/fi-0052-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0052b) {
  TestLibrary library;
  library.AddFile("good/fi-0052-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0053a) {
  TestLibrary library;
  library.AddFile("good/fi-0053-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0053b) {
  TestLibrary library;
  library.AddFile("good/fi-0053-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0054) {
  TestLibrary library;
  library.AddFile("good/fi-0054.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0055) {
  TestLibrary library;
  library.AddFile("good/fi-0055.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0058) {
  TestLibrary library;
  library.AddFile("good/fi-0058.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0059) {
  TestLibrary library;
  library.AddFile("good/fi-0059.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0060) {
  TestLibrary library;
  library.AddFile("good/fi-0060.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0061) {
  TestLibrary library;
  library.AddFile("good/fi-0061.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0062a) {
  TestLibrary library;
  library.AddFile("good/fi-0062-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0062b) {
  TestLibrary library;
  library.AddFile("good/fi-0062-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0063) {
  TestLibrary library;
  library.AddFile("good/fi-0063.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0064a) {
  TestLibrary library;
  library.AddFile("good/fi-0064-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0064b) {
  TestLibrary library;
  library.AddFile("good/fi-0064-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0065a) {
  TestLibrary library;
  library.AddFile("good/fi-0065-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0065b) {
  TestLibrary library;
  library.AddFile("good/fi-0065-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0065c) {
  TestLibrary library;
  library.AddFile("good/fi-0065-c.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0068A) {
  TestLibrary library;
  library.AddFile("good/fi-0068-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0068B) {
  TestLibrary library;
  library.AddFile("good/fi-0068-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0069) {
  TestLibrary library;
  library.AddFile("good/fi-0069.test.fidl");
}

TEST(ErrcatTests, Good0070) {
  TestLibrary library;
  library.AddFile("good/fi-0070.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0071a) {
  TestLibrary library;
  library.AddFile("good/fi-0071-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0071b) {
  TestLibrary library;
  library.AddFile("good/fi-0071-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0072a) {
  TestLibrary library;
  library.AddFile("good/fi-0072-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0072b) {
  TestLibrary library;
  library.AddFile("good/fi-0072-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0073) {
  TestLibrary library;
  library.AddFile("good/fi-0073.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0074) {
  TestLibrary library;
  library.AddFile("good/fi-0074.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0075) {
  TestLibrary library;
  library.AddFile("good/fi-0075.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0078a) {
  TestLibrary library;
  library.AddFile("good/fi-0078-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0078b) {
  TestLibrary library;
  library.AddFile("good/fi-0078-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0079) {
  TestLibrary library;
  library.AddFile("good/fi-0079.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0081) {
  TestLibrary library;
  library.AddFile("good/fi-0081.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0082) {
  TestLibrary library;
  library.AddFile("good/fi-0082.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0083) {
  TestLibrary library;
  library.AddFile("good/fi-0083.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0084) {
  TestLibrary library;
  library.AddFile("good/fi-0084.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0085a) {
  TestLibrary library;
  library.AddFile("good/fi-0085-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0085b) {
  TestLibrary library;
  library.AddFile("good/fi-0085-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0086a) {
  TestLibrary library;
  library.AddFile("good/fi-0086-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0086b) {
  TestLibrary library;
  library.AddFile("good/fi-0086-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0087) {
  TestLibrary library;
  library.AddFile("good/fi-0087.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0088) {
  TestLibrary library;
  library.AddFile("good/fi-0088.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0089a) {
  TestLibrary library;
  library.AddFile("good/fi-0089-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0089b) {
  TestLibrary library;
  library.AddFile("good/fi-0089-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0090) {
  TestLibrary library;
  library.AddFile("good/fi-0090.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0091) {
  TestLibrary library;
  library.AddFile("good/fi-0091.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0092) {
  TestLibrary library;
  library.AddFile("good/fi-0092.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0094a) {
  TestLibrary library;
  library.AddFile("good/fi-0094-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0094b) {
  TestLibrary library;
  library.AddFile("good/fi-0094-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0095a) {
  TestLibrary library;
  library.AddFile("good/fi-0095-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0095b) {
  TestLibrary library;
  library.AddFile("good/fi-0095-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0096a) {
  TestLibrary library;
  library.AddFile("good/fi-0096-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0096b) {
  TestLibrary library;
  library.AddFile("good/fi-0096-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0097a) {
  TestLibrary library;
  library.AddFile("good/fi-0097-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0097b) {
  TestLibrary library;
  library.AddFile("good/fi-0097-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0098a) {
  TestLibrary library;
  library.AddFile("good/fi-0098-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0098b) {
  TestLibrary library;
  library.AddFile("good/fi-0098-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0099a) {
  TestLibrary library;
  library.AddFile("good/fi-0099-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0099b) {
  TestLibrary library;
  library.AddFile("good/fi-0099-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0100a) {
  TestLibrary library;
  library.AddFile("good/fi-0100-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0100b) {
  TestLibrary library;
  library.AddFile("good/fi-0100-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0101) {
  TestLibrary library;
  library.AddFile("good/fi-0101.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0102) {
  TestLibrary library;
  library.AddFile("good/fi-0102.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0103) {
  TestLibrary library;
  library.AddFile("good/fi-0103.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0104) {
  TestLibrary library;
  library.AddFile("good/fi-0104.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0105a) {
  TestLibrary library;
  library.AddFile("good/fi-0105-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0105b) {
  TestLibrary library;
  library.AddFile("good/fi-0105-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0106) {
  TestLibrary library;
  library.AddFile("good/fi-0106.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0107a) {
  TestLibrary library;
  library.AddFile("good/fi-0107-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0107b) {
  TestLibrary library;
  library.AddFile("good/fi-0107-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0108) {
  TestLibrary library;
  library.AddFile("good/fi-0108.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0109) {
  TestLibrary library;
  library.AddFile("good/fi-0109.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0110a) {
  TestLibrary library;
  library.AddFile("good/fi-0110-a.test.fidl");
  library.UseLibraryZx();
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0110b) {
  TestLibrary library;
  library.AddFile("good/fi-0110-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0111) {
  TestLibrary library;
  library.AddFile("good/fi-0111.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0112) {
  TestLibrary library;
  library.AddFile("good/fi-0112.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0113) {
  TestLibrary library;
  library.AddFile("good/fi-0113.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0114a) {
  TestLibrary library;
  library.AddFile("good/fi-0114-a.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0114b) {
  TestLibrary library;
  library.AddFile("good/fi-0114-b.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0115a) {
  TestLibrary library;
  library.AddFile("good/fi-0115-a.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0115b) {
  TestLibrary library;
  library.AddFile("good/fi-0115-b.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0116a) {
  TestLibrary library;
  library.AddFile("good/fi-0116-a.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0116b) {
  TestLibrary library;
  library.AddFile("good/fi-0116-b.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0117a) {
  TestLibrary library;
  library.AddFile("good/fi-0117-a.test.fidl");
  library.UseLibraryZx();
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0117b) {
  TestLibrary library;
  library.AddFile("good/fi-0117-b.test.fidl");
  library.UseLibraryFdf();
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0118) {
  TestLibrary library;
  library.AddFile("good/fi-0118.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0119a) {
  TestLibrary library;
  library.AddFile("good/fi-0119-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0119b) {
  TestLibrary library;
  library.AddFile("good/fi-0119-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0120a) {
  TestLibrary library;
  library.AddFile("good/fi-0120-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0120b) {
  TestLibrary library;
  library.AddFile("good/fi-0120-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0121) {
  TestLibrary library;
  library.AddFile("good/fi-0121.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0122) {
  TestLibrary library;
  library.AddFile("good/fi-0122.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0123) {
  TestLibrary library;
  library.AddFile("good/fi-0123.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0124) {
  TestLibrary library;
  library.AddFile("good/fi-0124.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0125) {
  TestLibrary library;
  library.AddFile("good/fi-0125.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0126) {
  TestLibrary library;
  library.AddFile("good/fi-0126.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0127) {
  TestLibrary library;
  library.AddFile("good/fi-0127.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0128) {
  TestLibrary library;
  library.AddFile("good/fi-0128.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0129a) {
  TestLibrary library;
  library.AddFile("good/fi-0129-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0129b) {
  TestLibrary library;
  library.AddFile("good/fi-0129-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0130) {
  TestLibrary library;
  library.AddFile("good/fi-0130.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0131a) {
  TestLibrary library;
  library.AddFile("good/fi-0131-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0131b) {
  TestLibrary library;
  library.AddFile("good/fi-0131-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0132) {
  TestLibrary library;
  library.AddFile("good/fi-0132.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0133) {
  TestLibrary library;
  library.AddFile("good/fi-0133.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0135) {
  TestLibrary library;
  library.AddFile("good/fi-0135.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0139a) {
  TestLibrary library;
  library.AddFile("good/fi-0139-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0139b) {
  TestLibrary library;
  library.AddFile("good/fi-0139-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0140a) {
  TestLibrary library;
  library.AddFile("good/fi-0140-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0140b) {
  TestLibrary library;
  library.AddFile("good/fi-0140-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0141) {
  TestLibrary library;
  library.AddFile("good/fi-0141.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0142) {
  TestLibrary library;
  library.AddFile("good/fi-0142.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0144) {
  TestLibrary library;
  library.AddFile("good/fi-0144.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0143) {
  TestLibrary library;
  library.AddFile("good/fi-0143.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0146) {
  TestLibrary library;
  library.AddFile("good/fi-0146.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0147) {
  TestLibrary library;
  library.AddFile("good/fi-0147.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0148a) {
  TestLibrary library;
  library.AddFile("good/fi-0148-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0148b) {
  TestLibrary library;
  library.AddFile("good/fi-0148-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0149a) {
  TestLibrary library;
  library.AddFile("good/fi-0149-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0149b) {
  TestLibrary library;
  library.AddFile("good/fi-0149-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0150a) {
  TestLibrary library;
  library.AddFile("good/fi-0150-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0150b) {
  TestLibrary library;
  library.AddFile("good/fi-0150-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0151a) {
  TestLibrary library;
  library.AddFile("good/fi-0151-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0151b) {
  TestLibrary library;
  library.AddFile("good/fi-0151-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0152) {
  TestLibrary library;
  library.AddFile("good/fi-0152.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0153) {
  TestLibrary library;
  library.AddFile("good/fi-0153.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0154a) {
  TestLibrary library;
  library.AddFile("good/fi-0154-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0154b) {
  TestLibrary library;
  library.AddFile("good/fi-0154-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0155) {
  TestLibrary library;
  library.AddFile("good/fi-0155.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0156) {
  TestLibrary library;
  library.AddFile("good/fi-0156.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0157) {
  TestLibrary library;
  library.AddFile("good/fi-0157.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0159) {
  TestLibrary library;
  library.AddFile("good/fi-0159.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0160a) {
  TestLibrary library;
  library.AddFile("good/fi-0160-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0160b) {
  TestLibrary library;
  library.AddFile("good/fi-0160-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0161) {
  TestLibrary library;
  library.AddFile("good/fi-0161.test.fidl");
}

TEST(ErrcatTests, Good0162) {
  TestLibrary library;
  library.AddFile("good/fi-0162.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0163) {
  TestLibrary library;
  library.AddFile("good/fi-0163.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0164) {
  TestLibrary library;
  library.AddFile("good/fi-0164.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0165) {
  TestLibrary library;
  library.AddFile("good/fi-0165.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0166) {
  TestLibrary library;
  library.AddFile("good/fi-0166.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0168) {
  TestLibrary library;
  library.AddFile("good/fi-0168.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0169) {
  TestLibrary library;
  library.AddFile("good/fi-0169.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0170) {
  TestLibrary library;
  library.AddFile("good/fi-0170.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0171) {
  TestLibrary library;
  library.AddFile("good/fi-0171.test.fidl");
  library.UseLibraryZx();
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0172) {
  TestLibrary library;
  library.AddFile("good/fi-0172.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0173) {
  TestLibrary library;
  library.AddFile("good/fi-0173.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0175) {
  TestLibrary library;
  library.AddFile("good/fi-0175.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0177) {
  TestLibrary library;
  library.AddFile("good/fi-0177.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0179) {
  TestLibrary library;
  library.AddFile("good/fi-0179.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kAllowNewTypes);
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0180) {
  TestLibrary library;
  library.AddFile("good/fi-0180.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0181) {
  TestLibrary library;
  library.AddFile("good/fi-0181.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0182a) {
  TestLibrary library;
  library.AddFile("good/fi-0182-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0182b) {
  TestLibrary library;
  library.AddFile("good/fi-0182-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0183a) {
  TestLibrary library;
  library.AddFile("good/fi-0183-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0183b) {
  TestLibrary library;
  library.AddFile("good/fi-0183-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0184a) {
  TestLibrary library;
  library.AddFile("good/fi-0184-a.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0184b) {
  TestLibrary library;
  library.AddFile("good/fi-0184-b.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0185) {
  TestLibrary library;
  library.AddFile("good/fi-0185.test.fidl");
  ASSERT_COMPILED(library);
}
TEST(ErrcatTests, Good0186) {
  TestLibrary library;
  library.AddFile("good/fi-0186.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0187) {
  TestLibrary library;
  library.AddFile("good/fi-0187.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0188) {
  TestLibrary library;
  library.AddFile("good/fi-0188.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0189) {
  TestLibrary library;
  library.AddFile("good/fi-0189.test.fidl");
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0190) {
  TestLibrary library;
  library.AddFile("good/fi-0190.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0191) {
  TestLibrary library;
  library.AddFile("good/fi-0191.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_COMPILED(library);
}
TEST(ErrcatTests, Good0192) {
  TestLibrary library;
  library.AddFile("good/fi-0192.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_COMPILED(library);
}

TEST(ErrcatTests, Good0193) {
  TestLibrary library;
  library.AddFile("good/fi-0193.test.fidl");
  ASSERT_COMPILED(library);
}

// LINT.ThenChange(/docs/reference/fidl/language/errcat.md)

}  // namespace
