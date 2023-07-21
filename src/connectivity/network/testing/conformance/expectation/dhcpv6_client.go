// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package expectation

import "go.fuchsia.dev/fuchsia/src/connectivity/network/testing/conformance/expectation/outcome"

var dhcpv6ClientExpectations map[AnvlCaseNumber]outcome.Outcome = map[AnvlCaseNumber]outcome.Outcome{
	{1, 1}:  Pass,
	{2, 1}:  Pass,
	{3, 1}:  Pass,
	{4, 1}:  Pass,
	{5, 1}:  Pass,
	{5, 2}:  Pass,
	{6, 1}:  Pass,
	{7, 1}:  Pass,
	{7, 2}:  Pass,
	{7, 3}:  Pass,
	{7, 4}:  Pass,
	{8, 1}:  Pass,
	{9, 1}:  Pass,
	{10, 1}: Pass,
	{11, 1}: Pass,
	{12, 1}: Pass,
	{13, 1}: Pass,
	{13, 2}: Pass,
	{14, 1}: Pass,
	{15, 1}: Pass,
	{16, 1}: Pass,
	{17, 1}: Pass,
	{18, 1}: Pass,
	{18, 2}: Pass,
	{18, 3}: Pass,
	{18, 4}: Pass,
	{19, 1}: Pass,
	{19, 2}: Fail,
	{19, 3}: Inconclusive,
	{19, 4}: Inconclusive,
	{20, 1}: Inconclusive,
	{20, 2}: Inconclusive,
	{21, 1}: Fail,
	{21, 2}: Inconclusive,
	{22, 1}: Pass,
	{22, 2}: Pass,
	{22, 3}: Pass,
	{22, 4}: Pass,
	{22, 5}: Pass,
	{22, 6}: Fail,
	{23, 1}: Fail,
	{23, 2}: Inconclusive,
	{23, 3}: Inconclusive,
	{23, 4}: Inconclusive,
	{23, 5}: Inconclusive,
	// TODO(http://fxbug.dev/116499): Root-cause and fix.
	{24, 1}: Flaky,
	{24, 2}: Pass,
	// TODO(http://fxbug.dev/116499): Root-cause and fix.
	{24, 3}: Flaky,
	{24, 4}: Pass,
	{24, 5}: Pass,
	{24, 6}: Pass,
	{24, 7}: Pass,
	{25, 1}: Pass,
	{25, 2}: Pass,
	{25, 3}: Pass,
	// TODO(http://fxbug.dev/116499): Root-cause and fix.
	{25, 4}:  Flaky,
	{25, 5}:  Pass,
	{25, 6}:  Pass,
	{26, 1}:  Fail,
	{26, 2}:  Pass,
	{26, 3}:  Pass,
	{26, 4}:  Pass,
	{27, 1}:  Inconclusive,
	{27, 2}:  Inconclusive,
	{27, 3}:  Inconclusive,
	{27, 4}:  Fail,
	{28, 1}:  Inconclusive,
	{28, 2}:  Inconclusive,
	{28, 3}:  Inconclusive,
	{28, 4}:  Inconclusive,
	{28, 5}:  Inconclusive,
	{28, 6}:  Inconclusive,
	{28, 7}:  Inconclusive,
	{28, 8}:  Inconclusive,
	{29, 1}:  Inconclusive,
	{29, 2}:  Inconclusive,
	{29, 3}:  Inconclusive,
	{29, 4}:  Inconclusive,
	{29, 5}:  Fail,
	{29, 6}:  Pass,
	{29, 7}:  Fail,
	{29, 8}:  Pass,
	{29, 9}:  Pass,
	{29, 10}: Pass,
	{29, 11}: Inconclusive,
	{29, 12}: Inconclusive,
	{29, 13}: Pass,
	{30, 1}:  Pass,
	{30, 2}:  Inconclusive,
	{32, 1}:  Pass,
	{32, 3}:  Pass,
	{33, 1}:  Pass,
	{34, 1}:  Pass,
	{34, 2}:  Pass,
	{34, 3}:  Pass,
	{34, 4}:  Pass,
	{35, 1}:  Fail,
}

var dhcpv6ClientExpectationsNS3 map[AnvlCaseNumber]outcome.Outcome = map[AnvlCaseNumber]outcome.Outcome{
	{1, 1}:  Pass,
	{2, 1}:  Pass,
	{3, 1}:  Pass,
	{4, 1}:  Pass,
	{5, 1}:  Pass,
	{5, 2}:  Pass,
	{6, 1}:  Pass,
	{7, 1}:  Pass,
	{7, 2}:  Pass,
	{7, 3}:  Pass,
	{7, 4}:  Pass,
	{8, 1}:  Pass,
	{9, 1}:  Pass,
	{10, 1}: Pass,
	{11, 1}: Pass,
	{12, 1}: Pass,
	{13, 1}: Pass,
	{13, 2}: Pass,
	{14, 1}: Pass,
	{15, 1}: Pass,
	{16, 1}: Pass,
	{17, 1}: Pass,
	{18, 1}: Pass,
	{18, 2}: Pass,
	{18, 3}: Pass,
	{18, 4}: Pass,
	{19, 1}: Pass,
	{19, 2}: Fail,
	{19, 3}: Inconclusive,
	{19, 4}: Inconclusive,
	{20, 1}: Inconclusive,
	{20, 2}: Inconclusive,
	{21, 1}: Fail,
	{21, 2}: Inconclusive,
	{22, 1}: Pass,
	{22, 2}: Pass,
	{22, 3}: Pass,
	{22, 4}: Pass,
	{22, 5}: Pass,
	{22, 6}: Fail,
	{23, 1}: Fail,
	{23, 2}: Inconclusive,
	{23, 3}: Inconclusive,
	{23, 4}: Inconclusive,
	{23, 5}: Inconclusive,
	// TODO(http://fxbug.dev/116499): Root-cause and fix.
	{24, 1}: Flaky,
	{24, 2}: Pass,
	// TODO(http://fxbug.dev/116499): Root-cause and fix.
	{24, 3}: Flaky,
	{24, 4}: Pass,
	{24, 5}: Pass,
	{24, 6}: Pass,
	{24, 7}: Pass,
	{25, 1}: Pass,
	{25, 2}: Pass,
	{25, 3}: Pass,
	// TODO(http://fxbug.dev/116499): Root-cause and fix.
	{25, 4}:  Flaky,
	{25, 5}:  Pass,
	{25, 6}:  Pass,
	{26, 1}:  Fail,
	{26, 2}:  Pass,
	{26, 3}:  Pass,
	{26, 4}:  Pass,
	{27, 1}:  Inconclusive,
	{27, 2}:  Inconclusive,
	{27, 3}:  Inconclusive,
	{27, 4}:  Fail,
	{28, 1}:  Inconclusive,
	{28, 2}:  Inconclusive,
	{28, 3}:  Inconclusive,
	{28, 4}:  Inconclusive,
	{28, 5}:  Inconclusive,
	{28, 6}:  Inconclusive,
	{28, 7}:  Inconclusive,
	{28, 8}:  Inconclusive,
	{29, 1}:  Inconclusive,
	{29, 2}:  Inconclusive,
	{29, 3}:  Inconclusive,
	{29, 4}:  Inconclusive,
	{29, 5}:  Fail,
	{29, 6}:  Pass,
	{29, 7}:  Fail,
	{29, 8}:  Pass,
	{29, 9}:  Pass,
	{29, 10}: Pass,
	{29, 11}: Inconclusive,
	{29, 12}: Inconclusive,
	{29, 13}: Pass,
	{30, 1}:  Pass,
	{30, 2}:  Inconclusive,
	{32, 1}:  Pass,
	{32, 3}:  Pass,
	{33, 1}:  Pass,
	{34, 1}:  Pass,
	{34, 2}:  Pass,
	{34, 3}:  Pass,
	{34, 4}:  Pass,
	{35, 1}:  Fail,
}
