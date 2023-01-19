// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package expectation

import (
	"go.fuchsia.dev/fuchsia/src/connectivity/network/testing/conformance/expectation/outcome"
)

var igmpExpectations map[AnvlCaseNumber]outcome.Outcome = map[AnvlCaseNumber]outcome.Outcome{
	// TODO(https://fxbug.dev/120281): Run the IGMPv3 suite instead.
	{1, 1}: Fail,
	// TODO(https://fxbug.dev/120281): Run the IGMPv3 suite instead.
	{2, 9}:  Fail,
	{2, 10}: Pass,
	{2, 11}: Pass,
	{3, 1}:  Pass,
	{3, 4}:  Pass,
	{3, 5}:  Pass,
	// TODO(https://fxbug.dev/104720): Investigate flake.
	// TODO(https://fxbug.dev/120281): Run the IGMPv3 suite instead.
	// Note that if https://fxbug.dev/104720 recurs in the v3 suite, it
	// should be marked Flaky, otherwise marked Pass.
	{3, 6}: Fail,
	{3, 7}: Pass,
	{5, 1}: Pass,
	{5, 2}: Pass,
	{5, 3}: Pass,
	// TODO(https://fxbug.dev/120281): Run the IGMPv3 suite instead.
	{5, 5}: Fail,
	{5, 7}: Pass,
	{5, 8}: Fail,
	{5, 9}: Fail,
	// TODO(https://fxbug.dev/118202): Fix.
	{5, 11}: Fail,
	{6, 2}:  Pass,
	{6, 8}:  Pass,
}
