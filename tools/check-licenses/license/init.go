// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package license

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/check-licenses/file"
)

var (
	AllPatterns                 []*Pattern
	AllCopyrightPatterns        []*Pattern
	AllSearchResults            []*SearchResult
	AllLicenseFileSearchResults []*SearchResult
	AllowlistPatternMap         map[string][]string

	Unrecognized *Pattern
	Empty        *Pattern
)

func init() {
	AllPatterns = make([]*Pattern, 0)
	AllCopyrightPatterns = make([]*Pattern, 0)
	AllSearchResults = make([]*SearchResult, 0)
	AllLicenseFileSearchResults = make([]*SearchResult, 0)
	AllowlistPatternMap = make(map[string][]string, 0)
}

func Initialize(c *LicenseConfig) error {
	Config = c

	// Save the config file to the out directory (if defined).
	if b, err := json.MarshalIndent(c, "", "  "); err != nil {
		return err
	} else {
		plusFile("_config.json", b)
	}

	// Initialize all license patterns.
	for _, pr := range c.PatternRoots {
		for _, root := range pr.Paths {
			if err := filepath.Walk(root, patternsWalker); err != nil {
				return err
			}
		}
	}

	// If the license file is 0 bytes, add it to the Empty pattern.
	re, err := regexp.Compile(`(\A\z)`)
	if err != nil {
		return err
	}
	Empty = &Pattern{
		Name:               "_empty",
		RelPath:            "_empty",
		Matches:            make([]*file.FileData, 0),
		Allowlist:          []*Allowlist{},
		PreviousMatches:    make(map[string]bool),
		PreviousMismatches: make(map[string]bool),
		Re:                 re,
	}
	AllPatterns = append(AllPatterns, Empty)

	// Unrecognized license texts won't be added to the resulting NOTICE file.
	// This is good behavior, all texts should be recognized. But until we can add
	// all the necessary license patterns, add all unrecognized texts to a catch-all
	// pattern.
	re, err = regexp.Compile("(?P<text>.*)")
	if err != nil {
		return err
	}
	Unrecognized = &Pattern{
		Name:               "_unrecognized",
		RelPath:            "_unrecognized",
		Category:           "Unrecognized",
		Type:               "Unrecognized",
		Matches:            make([]*file.FileData, 0),
		Allowlist:          []*Allowlist{},
		PreviousMatches:    make(map[string]bool),
		PreviousMismatches: make(map[string]bool),
		Re:                 re,
	}
	AllPatterns = append(AllPatterns, Unrecognized)

	for _, p := range AllPatterns {
		base := filepath.Base(p.RelPath)
		path := filepath.Join(filepath.Join("patterns", p.RelPath), base)
		plusFile(path, []byte(p.Re.String()))
	}

	return nil
}

// Helper function used to traverse a license patterns directory,
// and create Pattern objects for each "*.lic" file found.
func patternsWalker(path string, info os.FileInfo, err error) error {
	if info.IsDir() {
		return nil
	}

	if !(strings.HasSuffix(info.Name(), ".lic") || strings.HasSuffix(info.Name(), ".txt")) {
		return nil
	}

	pattern, err := NewPattern(path)
	if err != nil {
		return err
	}

	plusVal(NumPatterns, path)
	if strings.Contains(filepath.Base(path), "copyright") {
		pattern.isHeader = true
		AllCopyrightPatterns = append(AllCopyrightPatterns, pattern)
	}
	AllPatterns = append(AllPatterns, pattern)

	return nil
}
