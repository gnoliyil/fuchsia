// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package project

import (
	"fmt"
	"sort"

	"go.fuchsia.dev/fuchsia/tools/check-licenses/file"
	"go.fuchsia.dev/fuchsia/tools/check-licenses/license"
)

// AnalyzeLicenses loops over every project that was created during this run,
// and performs a license search on the licenses and regular files included
// in each project.
func AnalyzeLicenses() error {
	// Convert the projects map into a list and sort it, to make this function consistent.
	filteredProjectsList := make([]*Project, 0, len(FilteredProjects))
	for _, p := range FilteredProjects {
		filteredProjectsList = append(filteredProjectsList, p)
	}
	sort.Sort(Order(filteredProjectsList))

	for _, p := range filteredProjectsList {
		plusVal(NumFilteredProjects, p.Root)
		// Analyze the license files in each project.
		sort.Sort(file.Order(p.LicenseFile))
		for _, l := range p.LicenseFile {
			if results, err := license.Search(p.Root, l); err != nil {
				return fmt.Errorf("Issue analyzing Project defined in [%v]: %v\n", p.ReadmePath, err)
			} else {
				p.SearchResults = append(p.SearchResults, results...)
				for _, r := range results {
					key := string(r.LicenseData.Data)
					if _, ok := p.SearchResultsDeduped[key]; !ok {
						p.SearchResultsDeduped[key] = r
					}
				}
			}
			// Set the license URLs in the license file objects.
			l.UpdateURLs(p.Name, p.URL)
		}

		// Analyze the copyright headers in the files in each project.
		sort.Sort(file.Order(p.SearchableFiles))
		for _, f := range p.SearchableFiles {
			if len(f.Text) == 0 {
				continue
			}
			if results, err := license.SearchHeaders(p.Root, f); err != nil {
				return fmt.Errorf("Issue analyzing Project defined in [%v]: %v\n", p.ReadmePath, err)
			} else {
				p.SearchResults = append(p.SearchResults, results...)
			}
		}
	}

	// Perform any cleanup steps in the license package.
	license.Finalize()
	return nil
}
