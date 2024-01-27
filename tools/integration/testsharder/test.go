// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package testsharder

import (
	"time"

	"go.fuchsia.dev/fuchsia/tools/build"
)

// RunAlgorithm describes how to run a test using the test's `Runs` field.
type RunAlgorithm string

const (
	// KeepGoing means to run the test for as many times as `Runs`
	// regardless of the result of each test run.
	KeepGoing RunAlgorithm = "KEEP_GOING"
	// StopOnFailure means to try the test up to `Runs` times
	// and to break on the first failure.
	StopOnFailure RunAlgorithm = "STOP_ON_FAILURE"
	// StopOnSuccess means to try the test up to `Runs` times
	// and to break on the first success.
	StopOnSuccess RunAlgorithm = "STOP_ON_SUCCESS"
)

// Test is a struct used to hold information about a build.Test and how to run it.
type Test struct {
	build.Test

	// Runs is the number of times this test should be run.
	Runs int `json:"runs,omitempty"`

	// RunAlgorithm determines how `Runs` will be used to run the test.
	RunAlgorithm RunAlgorithm `json:"run_algorithm,omitempty"`

	// RealmLabel is an optional arg passed to runtests to specify a
	// realm.
	RealmLabel string `json:"realm_label,omitempty"`

	// StopRepeatingAfterSecs is the duration for which to repeatedly run a
	// test.
	StopRepeatingAfterSecs int `json:"stop_repeating_after_secs,omitempty"`

	// Timeout is the timeout that should be set for each run of this test.
	Timeout time.Duration `json:"timeout_nanos,omitempty"`

	// Affected indicates whether the test is affected by the change under test.
	// It will only be set for tests running within tryjobs.
	Affected bool `json:"affected,omitempty"`

	// Tags are test metadata copied over from test-list.json.
	Tags []build.TestTag `json:"tags,omitempty"`
}

func (t *Test) applyModifier(m TestModifier) {
	// Only apply MaxAttempts if the test is not already set to be multiplied.
	if m.MaxAttempts > 0 && t.RunAlgorithm != StopOnFailure {
		t.Runs = m.MaxAttempts
		t.RunAlgorithm = StopOnSuccess
	}
	if m.TotalRuns >= 0 {
		if t.RunAlgorithm == StopOnFailure {
			numRuns := min(t.Runs, m.TotalRuns)
			if numRuns <= 0 {
				numRuns = max(t.Runs, m.TotalRuns)
			}
			t.Runs = numRuns
		} else {
			t.Runs = m.TotalRuns
			t.RunAlgorithm = StopOnFailure
		}
	}
	if m.Affected {
		t.Affected = true
	}
}

func (t *Test) minRequiredRuns() int {
	if t.RunAlgorithm == KeepGoing || t.RunAlgorithm == StopOnFailure {
		return max(t.Runs, 1)
	}
	return 1
}

func (t *Test) maxRuns() int {
	if t.Runs == 0 {
		return multipliedTestMaxRuns
	}
	return t.Runs
}

func (t *Test) applyTestListTags(tl build.TestListEntry) {
	t.Tags = tl.Tags
}

func (t *Test) Hermetic() bool {
	for _, tag := range t.Tags {
		if tag.Key == "hermetic" && tag.Value == "true" {
			return true
		}
	}
	return false
}
