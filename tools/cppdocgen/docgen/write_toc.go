// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package docgen

import (
	"fmt"
	"io"
)

func WriteToc(settings WriteSettings, index *Index, f io.Writer) {
	fmt.Fprintf(f, "# Generated by cppdocgen. Do not edit.\n")

	fmt.Fprintf(f, "toc:\n")
	fmt.Fprintf(f, "- title: \"Overview\"\n")
	fmt.Fprintf(f, "  path: index.md\n")
	fmt.Fprintf(f, "- heading: \"%s header files\"\n", settings.LibName)

	for _, h := range index.Headers {
		title := h.CustomTitle()
		if title == "" {
			fmt.Fprintf(f, "- title: \"%s\"\n", settings.GetUserIncludePath(h.Name))
		} else {
			fmt.Fprintf(f, "- title: \"%s\"\n", title)
		}

		fmt.Fprintf(f, "  path: %s%s\n", settings.TocPath, h.ReferenceFileName())
	}
}
