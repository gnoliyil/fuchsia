// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Package testparser parses test stdout into structured results.
package testparser

import (
	"bytes"
	"regexp"

	"go.fuchsia.dev/fuchsia/src/connectivity/network/testing/conformance/parseoutput"
	"go.fuchsia.dev/fuchsia/tools/testing/runtests"
)

// Parse takes stdout from a test program and returns structured results.
// Internally, a variety of test program stdout formats are supported.
// If no structured results were identified, an empty slice is returned.
func Parse(stdout []byte) []runtests.TestCaseResult {
	lines := bytes.Split(stdout, []byte{'\n'})
	res := []*regexp.Regexp{
		moblyTestPreamblePattern,
		ctsTestPreamblePattern,
		dartSystemTestPreamblePattern,
		trfTestPreamblePattern,
		googleTestPreamblePattern,
		goTestPreamblePattern,
		rustTestPreamblePattern,
		zirconUtestPreamblePattern,
		parseoutput.TestPreamblePattern,
	}
	remainingLines, match := firstMatch(lines, res)

	var cases []runtests.TestCaseResult
	switch match {
	case moblyTestPreamblePattern:
		cases = parseMoblyTest(remainingLines)
	case ctsTestPreamblePattern:
		cases = parseVulkanCtsTest(remainingLines)
	case dartSystemTestPreamblePattern:
		cases = parseDartSystemTest(remainingLines)
	case trfTestPreamblePattern:
		cases = parseTrfTest(lines)
	case googleTestPreamblePattern:
		cases = parseGoogleTest(remainingLines)
	case goTestPreamblePattern:
		cases = parseGoTest(remainingLines)
	case rustTestPreamblePattern:
		cases = parseRustTest(remainingLines)
	case zirconUtestPreamblePattern:
		cases = parseZirconUtest(remainingLines)
	case parseoutput.TestPreamblePattern:
		cases = parseNetworkConformanceTest(remainingLines)
	}

	// Ensure that an empty set of cases is serialized to JSON as an empty
	// array, not as null.
	if cases == nil {
		cases = []runtests.TestCaseResult{}
	}
	return cases
}

func firstMatch(
	lines [][]byte,
	res []*regexp.Regexp,
) ([][]byte, *regexp.Regexp) {
	for num, line := range lines {
		for _, re := range res {
			if re.Match(line) {
				return lines[num:], re
			}
		}
	}
	return nil, nil
}
