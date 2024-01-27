// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package readme

import (
	"encoding/json"
	"flag"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
)

var testDataDir = flag.String("test_data_dir", "", "Path to test data directory")

func TestListFilesRecursive(t *testing.T) {
	path := filepath.Join(*testDataDir, "readme", "listFilesRecursive")
	got, err := listFilesRecursive(path)
	if err != nil {
		t.Fatalf("%v: expected no error, got %v.", t.Name(), err)
	}

	want := []string{
		"a/b/c/file",
		"a/file",
		"aa/bb/file",
	}
	if diff := cmp.Diff(want, got); diff != "" {
		t.Errorf("%s: compare file list mismatch: (-want +got):\n%s", t.Name(), diff)
	}
}

func TestLoadReadmeFile(t *testing.T) {
	path := filepath.Join(*testDataDir, "readme", "README.fuchsia")
	got, err := NewReadmeFromFile(path)
	if err != nil {
		t.Fatalf("%v: expected no error, got %v.", t.Name(), err)
	}

	wantPath := filepath.Join(*testDataDir, "readme", "want.json")
	wantJson, err := os.ReadFile(wantPath)
	if err != nil {
		t.Fatalf("%v: failed to read in 'want' path %s: %v.", t.Name(), wantPath, err)
	}

	want := &Readme{}
	decoder := json.NewDecoder(strings.NewReader(string(wantJson)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(want); err != nil {
		t.Fatalf("%v: failed to decode want struct: %v.", t.Name(), err)
	}

	if diff := cmp.Diff(want, got); diff != "" {
		t.Errorf("%s: compare readmes mismatch: (-want +got):\n%s", t.Name(), diff)
	}
}

func runReadmeDiffTest(t *testing.T, wantPath string, got *Readme) {
	wantJson, err := os.ReadFile(wantPath)
	if err != nil {
		t.Fatalf("%v: failed to read in 'want' path %s: %v.", t.Name(), wantPath, err)
	}

	want := &Readme{}
	decoder := json.NewDecoder(strings.NewReader(string(wantJson)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(want); err != nil {
		t.Fatalf("%v: failed to decode want struct: %v.", t.Name(), err)
	}

	want.Sort()
	got.Sort()
	if diff := cmp.Diff(want, got); diff != "" {
		t.Errorf("%s: compare readmes mismatch: (-want +got):\n%s", t.Name(), diff)
	}

}
