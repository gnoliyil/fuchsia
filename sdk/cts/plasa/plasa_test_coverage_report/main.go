// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file

// This program converts a directory of YAML reports produced by `clang_doc` into
// a report usable by test coverage.
//
// Please refer to the file README.md in this directory for more information
// about the program and its use.

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path"
	"sort"

	"go.fuchsia.dev/fuchsia/sdk/cts/plasa/model"
)

var (
	plasaManifestFile = flag.String("plasa-manifest-file", "", "The Plasa manifest file to read in")
	output            = flag.String("output", "", "The test coverage report output file")
	fragmentPrefix    = flag.String("fragment-prefix", "", "The path prefixed to each fragment file, usually only needed for tests")
)

// PlasaManifest represents the contents of a Plasa manifest file.
type PlasaManifest []PlasaFragment

// PlasaFragment is a single element of a Plasa manifest
type PlasaFragment struct {
	// Dest is the (optional) destination to which a fragment should be packaged.
	Dest string `json:"dest,omitempty"`
	// File is the GN file label pointing at a fragment.
	File string `json:"file,omitempty"`
	// Kind is the fragment file content type.
	Kind string `json:"kind"`
	// Path is the file path on the local filesystem where the fragment file
	// can be found and loaded from.
	Path string `json:"path"`
}

// TestCoverageReport is the data model for test coverage reporting.  It is
// written out in JSON output encoding.
type TestCoverageReport struct {
	// Items contains all the test coverage report items added to this report.
	Items []TestCoverageReportItem `json:"items"`
	// seen contains the elements that have already been inserted. No duplication
	// is allowed.
	seen map[string]struct{} `json:",ignore"`
}

/// NewTestCoverageReport initializes a new test coverage report type.
func NewTestCoverageReport() TestCoverageReport {
	return TestCoverageReport{
		Items: nil,
		seen:  map[string]struct{}{},
	}
}

// TestCoverageReportItem is a single item reported to the test coverage.
type TestCoverageReportItem struct {
	// Name is the fully qualified name of the report item, such as "::ns::Foo".
	// The exact format depends on Kind.   It is assumed that the Name is unique within a Kind.
	Name string `json:"name"`
	// The Kind of the fully qualified name.  For example, it could be "cc_api"
	// or "fidl_api".
	Kind string `json:"kind"`
}

// KindCCAPI denotes a C++ API element.
const KindCCAPI = "cc_api"

func readManifest(r io.Reader) (PlasaManifest, error) {
	var m PlasaManifest
	d := json.NewDecoder(r)
	d.DisallowUnknownFields()
	if err := d.Decode(&m); err != nil {
		return m, fmt.Errorf("while decoding plasa manifest: %w", err)
	}
	return m, nil
}

func paths(m PlasaManifest) []string {
	var ret []string
	for _, f := range m {
		s := path.Join(*fragmentPrefix, f.Path)
		ret = append(ret, s)
	}
	return ret
}

func (m *TestCoverageReport) add(r model.Report) {
	for _, i := range r.Items {
		n := i.Name
		if _, ok := m.seen[n]; ok {
			// Skip seen elements.
			continue
		}
		ci := TestCoverageReportItem{
			Name: i.Name,
			Kind: KindCCAPI,
		}
		m.Items = append(m.Items, ci)
		m.seen[n] = struct{}{}
	}
}

func (m TestCoverageReport) writeTo(w io.Writer) error {
	sort.SliceStable(m.Items, func(i, j int) bool {
		return m.Items[i].Name < m.Items[j].Name
	})
	e := json.NewEncoder(w)
	e.SetEscapeHTML(false)
	e.SetIndent("", "    ")
	if err := e.Encode(m); err != nil {
		return fmt.Errorf("could not encode as JSON: %w", err)
	}
	return nil
}

func filter(m io.Reader, w io.Writer) error {
	p, err := readManifest(m)
	if err != nil {
		return fmt.Errorf("could not read manifest: %v: %w", plasaManifestFile, err)
	}
	paths := paths(p)
	out := NewTestCoverageReport()
	for _, p := range paths {
		pf, err := os.Open(p)
		if err != nil {
			return fmt.Errorf("could not read fragment: %v: %w", p, err)
		}

		r, err := model.ReadReportJSON(pf)
		if err != nil {
			return fmt.Errorf("could not parse report: %v: %w", p, err)
		}
		out.add(r)
	}
	if err := out.writeTo(w); err != nil {
		return fmt.Errorf("while writing test coverage report: %w", err)
	}
	return nil
}

func run(plasaManifestFile, output string) error {
	if plasaManifestFile == "" {
		return fmt.Errorf("missing flag: --plasa-manifest-file=...")
	}
	m, err := os.Open(plasaManifestFile)
	if err != nil {
		return fmt.Errorf("could not open: %v: %w", plasaManifestFile, err)
	}
	defer m.Close()
	var w io.WriteCloser

	if output == "" {
		w = os.Stdout
	} else {
		w, err = os.Create(output)
		if err != nil {
			return fmt.Errorf("could not open output: %v: %w", output, err)
		}
		defer w.Close()
	}
	return filter(m, w)
}

func main() {
	flag.Parse()
	if err := run(*plasaManifestFile, *output); err != nil {
		fmt.Fprintf(os.Stderr, "plasa_test_coverage_report/main.run(): %v", err)
		os.Exit(-1)
	}
}
