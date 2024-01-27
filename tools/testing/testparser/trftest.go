// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package testparser

import (
	"regexp"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/build"
	"go.fuchsia.dev/fuchsia/tools/testing/runtests"
)

var (
	trfTestPreamblePattern = regexp.MustCompile(`^Running test 'fuchsia-pkg:\/\/.*$`)
	// ex: "[PASSED]	InlineDirTest.InlineDirPino"
	trfTestCasePattern = regexp.MustCompile(`\[(PASSED|FAILED|INCONCLUSIVE|TIMED_OUT|ERROR|SKIPPED)\]\t(.*)$`)
	// ex: "[stderr - NodeManagerTest.TruncateExceptionCase]"
	// Note that we don't try to parse for end of stderr, because the trf stdout always prints the real stderr msg
	// after the matched trfTestCaseStderr pattern one line at a time.
	// For ex:
	//   [stderr - suite1.case1]
	//   first line of error msg from case1
	//   [stderr - suite1.case2]
	//   first line of error msg from case2
	//   [stderr - suite1.case1]
	//   second line of error msg from case1
	//   [stderr - suite1.case2]
	//   second line of error msg from case2
	trfTestCaseStderr = regexp.MustCompile(`^\[stderr - (?P<test_name>.*?)\]$`)
	// legacy_test is the generic test name given when trf is running a v1 test suite.
	// trf treats all v1 test as legacy_test and does not format the stdout correctly.
	trfLegacyTest = regexp.MustCompile(`\[RUNNING\]\tlegacy_test`)
	// ex: "[expectation-comparer] INFO: ChrootTest.Success success is expected."
	// ex: "[expectation-comparer] INFO: ChrootTest.ProcMemSelfMapsNoEscapeProcOpen skip is expected."
	// ex: "[expectation-comparer] INFO: ChrootTest.ProcMemSelfFdsNoEscapeProcOpen failure is expected, so it will be reported to the test runner as having passed."
	trfTestExpectationPattern = regexp.MustCompile(`(.*)INFO: (?P<test_name>.*?) (?P<expectation>.*?) is expected(.*)$`)
)

// Parse tests run by the Test Runner Framework (TRF)
func parseTrfTest(lines [][]byte) []runtests.TestCaseResult {
	var res []runtests.TestCaseResult
	testCases := make(map[string]runtests.TestCaseResult)
	errorMessages := make(map[string]*strings.Builder)
	expectations := make(map[string]string)

	currentTestName := ""
	foundStderr := false
	linesToCapture := 3 // Total number of lines in stderr to capture as failure_reason
	linesCaptured := 0

	for _, line := range lines {
		line := string(line)
		// Stop parsing if running legacy_test, see: fxbug.dev/91055
		if trfLegacyTest.MatchString(line) {
			return []runtests.TestCaseResult{}
		}
		if m := trfTestCasePattern.FindStringSubmatch(line); m != nil {
			tc := createTRFTestCase(m[2], m[1])
			testCases[tc.DisplayName] = tc
			currentTestName = ""
			continue
		}
		// We make the assumption that the stderr message always follows a match to trfTestCaseStderr.
		// And we only capture the first [linesToCapture] line after a match to trfTestCaseStderr is found.
		if m := trfTestCaseStderr.FindStringSubmatch(line); m != nil {
			currentTestName = m[1]
			if _, ok := errorMessages[currentTestName]; !ok {
				errorMessages[currentTestName] = &strings.Builder{}
				foundStderr = true
				linesCaptured = 0
			}
			continue
		}
		if foundStderr && currentTestName != "" {
			if linesCaptured < linesToCapture {
				if linesCaptured == 0 {
					errorMessages[currentTestName].WriteString(line)
				} else {
					errorMessages[currentTestName].WriteString("\n" + line)
				}
				linesCaptured++
			} else {
				foundStderr = false
				currentTestName = ""
			}
			continue
		}
		if m := trfTestExpectationPattern.FindStringSubmatch(line); m != nil {
			expectations[m[2]] = m[3]
		}
	}

	for testName, testCase := range testCases {
		if msg, ok := errorMessages[testName]; ok {
			if testCase.Status == runtests.TestFailure {
				testCase.FailReason = msg.String()
			}
		}
		if expectation, ok := expectations[testName]; ok {
			testCase.Tags = []build.TestTag{
				{
					Key:   "expectation",
					Value: expectation,
				},
			}
		}
		res = append(res, testCase)
	}
	return res
}

func createTRFTestCase(caseName, result string) runtests.TestCaseResult {
	var status runtests.TestResult
	switch result {
	case "PASSED":
		status = runtests.TestSuccess
	case "FAILED":
		status = runtests.TestFailure
	case "INCONCLUSIVE":
		status = runtests.TestFailure
	case "TIMED_OUT":
		status = runtests.TestAborted
	case "ERROR":
		status = runtests.TestFailure
	case "SKIPPED":
		status = runtests.TestSkipped
	}
	return runtests.TestCaseResult{
		DisplayName: caseName,
		CaseName:    caseName,
		Status:      status,
		Format:      "FTF",
	}
}
